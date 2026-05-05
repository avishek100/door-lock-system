/*
 * ================================================================
 *   ESP32 Smart Door Lock — ILI9341 2.8" TFT + XPT2046 Touch
 *   + MFRC522 RFID + Solenoid Lock via Relay
 *
 *  Features:
 *    - RFID card scan → name lookup → grant/deny
 *    - ILI9341 TFT feedback screen with touch menu
 *    - Touch UI: Admin panel (add/block card, view log)
 *    - Solenoid lock control via relay
 *    - WiFi + web-based Smart Register dashboard
 *
 *  Libraries (install via Arduino Library Manager):
 *    1. MFRC522             by GithubCommunity
 *    2. Adafruit ILI9341    by Adafruit
 *    3. Adafruit GFX        by Adafruit
 *    4. XPT2046_Touchscreen by Paul Stoffregen
 *    5. WiFi                (built-in ESP32)
 *    6. WebServer           (built-in ESP32)
 * ================================================================
 *
 *  PIN MAP — ILI9341 2.8" with XPT2046 touch
 *
 *  TFT/Display (HSPI)      ESP32
 *  ------------------      -----
 *  VCC                 →   3.3V
 *  GND                 →   GND
 *  CS                  →   GPIO 15
 *  RESET               →   GPIO 25
 *  DC (A0)             →   GPIO 12
 *  SDI (MOSI)          →   GPIO 13
 *  SCK                 →   GPIO 14
 *  LED (backlight)     →   3.3V (always on) or GPIO for PWM
 *
 *  XPT2046 Touch (shares HSPI)
 *  ------------------      -----
 *  T_CLK               →   GPIO 14  (shared with TFT SCK)
 *  T_CS                →   GPIO 22  (separate!)
 *  T_DIN               →   GPIO 13  (shared with TFT MOSI)
 *  T_DO                →   GPIO 27  (separate MISO for touch)
 *  T_IRQ               →   GPIO 21  (interrupt)
 *
 *  MFRC522 RFID (VSPI)     ESP32
 *  ------------------      -----
 *  SDA (SS)            →   GPIO 5
 *  SCK                 →   GPIO 18
 *  MOSI                →   GPIO 23
 *  MISO                →   GPIO 19
 *  RST                 →   GPIO 4
 *  3.3V                →   3.3V
 *  GND                 →   GND
 *
 *  Relay Module            ESP32
 *  ------------------      -----
 *  IN                  →   GPIO 2
 *  VCC                 →   5V
 *  GND                 →   GND
 *
 *  Solenoid Lock           Relay & Power
 *  ------------------      -------------
 *  + (Red)             →   Relay NO
 *  - (Black)           →   12V GND
 *  12V+                →   Relay COM
 * ================================================================
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

// ── WiFi ──────────────────────────────────────────────────────
const char* WIFI_SSID     = "Bikash_DHInternet";
const char* WIFI_PASSWORD = "DHInternet@20992086";

// ── Pins ──────────────────────────────────────────────────────
// RFID — VSPI (default SPI)
#define RFID_SS   5
#define RFID_RST  4

// TFT — HSPI
#define TFT_CS    15
#define TFT_RST   25
#define TFT_DC    12
#define TFT_SCK   14
#define TFT_MOSI  13

// Touch — shares HSPI SCK/MOSI, has own CS and MISO
#define TOUCH_CS   22
#define TOUCH_IRQ  21
#define TOUCH_MISO 27   // T_DO pin

// Relay
#define RELAY_PIN  2
#define LOCK_OPEN  LOW    // Active-LOW relay: LOW = unlock
#define LOCK_CLOSE HIGH

// ── Timing ────────────────────────────────────────────────────
#define UNLOCK_MS      3000   // Door open duration
#define MSG_MS         2500   // Result screen duration
#define TOUCH_DEBOUNCE 300    // ms between touch events

// ── Touch calibration (adjust for your screen) ────────────────
#define TOUCH_X_MIN  712
#define TOUCH_X_MAX  3727
#define TOUCH_Y_MIN  881
#define TOUCH_Y_MAX  2981
#define SCREEN_W     320
#define SCREEN_H     240

// ── Colors (RGB565) ───────────────────────────────────────────
#define C_BG       0x0841
#define C_PANEL    0x10A2
#define C_WHITE    0xFFFF
#define C_GREEN    0x07E0
#define C_DKGREEN  0x03E0
#define C_RED      0xF800
#define C_BLUE     0x001F
#define C_LBLUE    0x4C9F
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF
#define C_ORANGE   0xFD20
#define C_GRAY     0x8410
#define C_DKGRAY   0x4208
#define C_PURPLE   0x780F

// ── Admin PIN ─────────────────────────────────────────────────
const String ADMIN_PIN = "1234";

// ── Card database ─────────────────────────────────────────────
#define MAX_CARDS 20
struct Card {
  String uid;
  String name;
  String role;
  bool   blocked;
};

Card cards[MAX_CARDS] = {
  // *** REPLACE UIDs with actual values from Serial Monitor ***
  { "A1B2C3D4", "Alice Johnson",  "Admin",   false },
  { "12345678", "Bob Smith",      "Staff",   false },
  { "AABBCCDD", "Carol Williams", "Visitor", false },
  { "11223344", "David Brown",    "Staff",   false },
  { "DEADBEEF", "Eve Davis",      "Admin",   false },
};
int cardCount = 5;

// ── Access log (circular, last 50 entries) ────────────────────
#define MAX_LOG 50
struct LogEntry {
  String uid, name, role, ts;
  bool granted;
};
LogEntry logBuf[MAX_LOG];
int logCount = 0, logHead = 0;

// ── Screen states ─────────────────────────────────────────────
enum Screen {
  SCR_IDLE,
  SCR_GRANTED,
  SCR_DENIED,
  SCR_ADMIN_PIN,
  SCR_ADMIN_MENU,
  SCR_VIEW_LOG,
  SCR_SCAN_NEW,
  SCR_LOG_WEB
};
Screen currentScreen = SCR_IDLE;

// ── Hardware objects ──────────────────────────────────────────
MFRC522 rfid(RFID_SS, RFID_RST);

SPIClass hSPI(HSPI);
Adafruit_ILI9341 tft(&hSPI, TFT_DC, TFT_CS, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

WebServer server(80);

// ── State ─────────────────────────────────────────────────────
unsigned long lastTouch = 0;
String adminPinEntry = "";
bool waitingForNewCard = false;
int logPage = 0;

// =================================================================
//  HELPERS
// =================================================================
String getUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

int findCard(const String& uid) {
  for (int i = 0; i < cardCount; i++)
    if (cards[i].uid.equalsIgnoreCase(uid)) return i;
  return -1;
}

String getTimestamp() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "N/A";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
  return String(buf);
}

void addLog(const String& uid, const String& name,
            const String& role, bool granted) {
  logBuf[logHead] = { uid, name, role, getTimestamp(), granted };
  logHead = (logHead + 1) % MAX_LOG;
  if (logCount < MAX_LOG) logCount++;
}

// Map raw touch → screen pixels (landscape)
bool getTouchPoint(int& sx, int& sy) {
  if (!ts.tirqTouched() || !ts.touched()) return false;
  unsigned long now = millis();
  if (now - lastTouch < TOUCH_DEBOUNCE) return false;
  lastTouch = now;
  TS_Point p = ts.getPoint();
  sx = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_W);
  sy = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_H);
  sx = constrain(sx, 0, SCREEN_W - 1);
  sy = constrain(sy, 0, SCREEN_H - 1);
  return true;
}

// =================================================================
//  DRAW HELPERS
// =================================================================

// ── Common header bar ──────────────────────────────────────────
void drawHeader(const char* title, uint16_t color) {
  tft.fillRect(0, 0, 320, 36, color);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print(title);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextSize(1);
    tft.setCursor(200, 14);
    tft.print(WiFi.localIP().toString());
  }
}

// ── Rounded button ────────────────────────────────────────────
void drawButton(int x, int y, int w, int h,
                const char* label, uint16_t bg, uint16_t fg = C_WHITE) {
  tft.fillRoundRect(x, y, w, h, 6, bg);
  tft.drawRoundRect(x, y, w, h, 6, fg);
  int tx = x + (w - strlen(label) * 6) / 2;
  int ty = y + (h - 8) / 2;
  tft.setTextColor(fg);
  tft.setTextSize(1);
  tft.setCursor(tx, ty);
  tft.print(label);
}

// ── Padlock shackle drawn without drawArc ─────────────────────
// Approximates a U-shape using pixel-plotted semicircle
void drawShackle(int cx, int topY, int outerR, int innerR, uint16_t color) {
  for (int angle = 0; angle <= 180; angle += 3) {
    float rad = angle * PI / 180.0;
    // Outer arc
    int ox = cx + (int)(outerR * cos(rad));
    int oy = topY - (int)(outerR * sin(rad));
    tft.drawPixel(ox, oy, color);
    tft.drawPixel(ox + 1, oy, color);  // 2px thick
    // Inner arc
    int ix = cx + (int)(innerR * cos(rad));
    int iy = topY - (int)(innerR * sin(rad));
    tft.drawPixel(ix, iy, color);
  }
  // Side legs to close the shackle into the lock body
  tft.drawLine(cx - outerR, topY, cx - outerR, topY + 12, color);
  tft.drawLine(cx - outerR + 1, topY, cx - outerR + 1, topY + 12, color);
  tft.drawLine(cx + outerR, topY, cx + outerR, topY + 12, color);
  tft.drawLine(cx + outerR - 1, topY, cx + outerR - 1, topY + 12, color);
}

// =================================================================
//  SCREEN DRAW FUNCTIONS
// =================================================================

// ── IDLE ──────────────────────────────────────────────────────
void drawIdle() {
  tft.fillScreen(C_BG);
  drawHeader("SMART DOOR LOCK", 0x0210);

  // Lock body
  tft.fillRoundRect(130, 82, 60, 48, 4, C_DKGRAY);
  tft.drawRoundRect(128, 80, 64, 52, 4, C_GRAY);
  // Keyhole circle
  tft.fillCircle(160, 104, 8, C_GRAY);
  // Keyhole slot
  tft.fillRect(157, 104, 6, 14, C_DKGRAY);
  // Shackle (semicircle, no drawArc needed)
  drawShackle(160, 82, 22, 16, C_GRAY);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(28, 148);
  tft.print("Tap RFID Card");

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(60, 172);
  tft.print("or touch screen for admin");

  tft.setTextColor(C_GRAY);
  tft.setCursor(8, 198);
  tft.print("Log: ");
  tft.print(logCount);
  tft.print(" entries");

  drawButton(238, 210, 74, 22, "ADMIN", C_DKGRAY, C_GRAY);
}

// ── ACCESS GRANTED ────────────────────────────────────────────
void drawGranted(const String& name, const String& role) {
  tft.fillScreen(C_BG);
  drawHeader("  ACCESS GRANTED", C_DKGREEN);

  // Big checkmark
  tft.setTextColor(C_GREEN);
  tft.setTextSize(4);
  tft.setCursor(128, 52);
  tft.print("OK");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("Welcome,");

  tft.setTextColor(C_CYAN);
  tft.setTextSize(name.length() > 14 ? 1 : 2);
  tft.setCursor(8, 135);
  tft.print(name);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 178);
  tft.print("Role: "); tft.print(role);
  tft.setCursor(8, 193);
  tft.print("Door open for ");
  tft.print(UNLOCK_MS / 1000);
  tft.print(" seconds...");
}

// ── ACCESS DENIED ─────────────────────────────────────────────
void drawDenied(const String& uid) {
  tft.fillScreen(C_BG);
  drawHeader("  ACCESS DENIED ", 0x8000);

  tft.setTextColor(C_RED);
  tft.setTextSize(4);
  tft.setCursor(130, 52);
  tft.print("X");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("Card Not Registered");

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 178);
  tft.print("UID: "); tft.print(uid);
  tft.setCursor(8, 193);
  tft.print("Contact the administrator.");
}

// ── BLOCKED ───────────────────────────────────────────────────
void drawBlocked(const String& name) {
  tft.fillScreen(C_BG);
  drawHeader("   CARD BLOCKED ", C_ORANGE >> 1);

  tft.setTextColor(C_ORANGE);
  tft.setTextSize(4);
  tft.setCursor(116, 52);
  tft.print("!!");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("Card is blocked:");
  tft.setCursor(8, 135);
  tft.setTextColor(C_ORANGE);
  tft.print(name);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 193);
  tft.print("See admin to unblock.");
}

// ── ADMIN PIN ENTRY ───────────────────────────────────────────
void drawAdminPin() {
  tft.fillScreen(C_BG);
  drawHeader("  ADMIN ACCESS  ", C_PURPLE);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(80, 44);
  tft.print("Enter 4-digit PIN:");

  // PIN indicator dots
  for (int i = 0; i < 4; i++) {
    if (i < (int)adminPinEntry.length())
      tft.fillCircle(120 + i * 22, 66, 7, C_CYAN);
    else
      tft.drawCircle(120 + i * 22, 66, 7, C_GRAY);
  }

  // Numpad 3×4
  const char* keys[]  = {"1","2","3","4","5","6","7","8","9","CLR","0","OK"};
  uint16_t kcolors[]  = {C_PANEL,C_PANEL,C_PANEL,C_PANEL,C_PANEL,C_PANEL,
                          C_PANEL,C_PANEL,C_PANEL,0x6000,C_PANEL,0x0230};
  for (int i = 0; i < 12; i++) {
    int col = i % 3, row = i / 3;
    int bx = 60 + col * 72, by = 84 + row * 40;
    drawButton(bx, by, 64, 34, keys[i], kcolors[i]);
  }

  drawButton(8, 207, 60, 26, "Cancel", 0x6000, C_WHITE);
}

// ── ADMIN MENU ────────────────────────────────────────────────
void drawAdminMenu() {
  tft.fillScreen(C_BG);
  drawHeader("  ADMIN MENU    ", C_PURPLE);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 46);
  tft.print("Registered cards: "); tft.print(cardCount);

  drawButton(8,   60, 148, 40, "View Log",      0x0230, C_WHITE);
  drawButton(164, 60, 148, 40, "Add New Card",  0x3000, C_WHITE);
  drawButton(8,  110, 148, 40, "Block Card",    0x6000, C_WHITE);
  drawButton(164,110, 148, 40, "Web Register",  0x1082, C_WHITE);
  drawButton(8,  160, 148, 40, "Open Lock",     0x0460, C_WHITE);
  drawButton(164,160, 148, 40, "Card Count",    C_PANEL, C_GRAY);

  drawButton(8, 210, 60, 26, "< Back", C_DKGRAY, C_WHITE);
}

// ── VIEW LOG ──────────────────────────────────────────────────
void drawLogScreen(int page) {
  tft.fillScreen(C_BG);
  drawHeader("  ACCESS LOG    ", 0x0230);

  int perPage = 4;
  int total   = logCount;
  int end     = max(0, total - page * perPage);
  int start   = max(0, end - perPage);

  tft.setTextSize(1);
  for (int i = start; i < end; i++) {
    // Walk backwards from newest: index in circular buffer
    int realIdx = (logHead - 1 - (logCount - 1 - i) % MAX_LOG + MAX_LOG * 2) % MAX_LOG;
    LogEntry& e = logBuf[realIdx];
    int y = 44 + (i - start) * 44;
    uint16_t col = e.granted ? C_DKGREEN : 0x4000;
    tft.fillRect(4, y, 312, 40, col);
    tft.drawRect(4, y, 312, 40, e.granted ? C_GREEN : C_RED);
    tft.setTextColor(C_WHITE);
    tft.setCursor(8, y + 6);
    tft.print(e.name.length() > 18 ? e.name.substring(0, 18) : e.name);
    tft.setTextColor(C_GRAY);
    tft.setCursor(8, y + 22);
    tft.print(e.ts.length() > 19 ? e.ts.substring(0, 19) : e.ts);
    tft.setCursor(220, y + 6);
    tft.setTextColor(e.granted ? C_GREEN : C_RED);
    tft.print(e.granted ? "GRANTED" : "DENIED");
  }

  drawButton(8,   220, 70, 18, "< Newer", C_DKGRAY, C_WHITE);
  drawButton(242, 220, 70, 18, "Older >", C_DKGRAY, C_WHITE);
  drawButton(86,  220, 144, 18, "< Back",  C_PANEL,  C_WHITE);
}

// ── SCAN NEW CARD ─────────────────────────────────────────────
void drawScanNew() {
  tft.fillScreen(C_BG);
  drawHeader("  ADD NEW CARD  ", 0x3000);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 55);
  tft.print("Scan new card");
  tft.setCursor(20, 80);
  tft.print("with RFID reader");

  tft.setTextColor(C_YELLOW);
  tft.setTextSize(3);
  tft.setCursor(120, 112);
  tft.print("...");

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(20, 172);
  tft.print("New card added as 'Staff'.");
  tft.setCursor(20, 188);
  tft.print("Edit name/role via web dashboard.");

  drawButton(8, 210, 60, 26, "Cancel", 0x6000, C_WHITE);
}

// ── WEB REGISTER INFO ─────────────────────────────────────────
void drawWebInfo() {
  tft.fillScreen(C_BG);
  drawHeader("  WEB REGISTER  ", 0x1082);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 50);
  tft.print("Open on any device on WiFi:");

  tft.setTextColor(C_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 68);
  if (WiFi.status() == WL_CONNECTED)
    tft.print("http://" + WiFi.localIP().toString());
  else
    tft.print("WiFi not connected");

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 112);
  tft.print("Features:");
  tft.setCursor(8, 126); tft.print("- Full access log with timestamps");
  tft.setCursor(8, 140); tft.print("- Filter granted / denied entries");
  tft.setCursor(8, 154); tft.print("- Auto-refresh every 5 seconds");
  tft.setCursor(8, 168); tft.print("- Stats: total / granted / denied");

  drawButton(8, 210, 60, 26, "< Back", C_DKGRAY, C_WHITE);
}

// =================================================================
//  TOUCH HANDLER
// =================================================================
void handleTouch(int tx, int ty) {
  Serial.printf("Touch: (%d, %d) screen=%d\n", tx, ty, currentScreen);

  switch (currentScreen) {

    // ── IDLE ──────────────────────────────────────────────────
    case SCR_IDLE:
      if (tx > 238 && ty > 210) {
        adminPinEntry = "";
        currentScreen = SCR_ADMIN_PIN;
        drawAdminPin();
      }
      break;

    // ── PIN ENTRY ─────────────────────────────────────────────
    case SCR_ADMIN_PIN: {
      if (tx < 68 && ty > 207) {          // Cancel
        currentScreen = SCR_IDLE;
        drawIdle();
        return;
      }
      // Numpad grid cols at x=60,132,204; rows at y=84,124,164,204
      if (tx >= 60 && tx <= 252 && ty >= 84 && ty <= 244) {
        int col = (tx - 60) / 72;
        int row = (ty - 84) / 40;
        int key = row * 3 + col;
        const char* keys[] = {"1","2","3","4","5","6","7","8","9","CLR","0","OK"};
        if (key >= 0 && key < 12) {
          String k = keys[key];
          if (k == "CLR") {
            if (adminPinEntry.length()) adminPinEntry.remove(adminPinEntry.length() - 1);
          } else if (k == "OK") {
            if (adminPinEntry == ADMIN_PIN) {
              currentScreen = SCR_ADMIN_MENU;
              drawAdminMenu();
            } else {
              // Flash error
              tft.fillRect(60, 74, 200, 18, C_BG);
              tft.setTextColor(C_RED);
              tft.setTextSize(1);
              tft.setCursor(90, 76);
              tft.print("Wrong PIN! Try again.");
              adminPinEntry = "";
              delay(900);
              drawAdminPin();
            }
            return;
          } else if (adminPinEntry.length() < 4) {
            adminPinEntry += k;
          }
          drawAdminPin();
        }
      }
      break;
    }

    // ── ADMIN MENU ────────────────────────────────────────────
    case SCR_ADMIN_MENU:
      if (tx < 68 && ty > 210) {                         // Back
        currentScreen = SCR_IDLE; drawIdle(); return;
      }
      if (tx < 160 && ty > 58 && ty < 102) {            // View Log
        logPage = 0; currentScreen = SCR_VIEW_LOG; drawLogScreen(0);
      } else if (tx > 162 && ty > 58 && ty < 102) {     // Add Card
        waitingForNewCard = true; currentScreen = SCR_SCAN_NEW; drawScanNew();
      } else if (tx < 160 && ty > 108 && ty < 152) {    // Block Card (placeholder)
        tft.fillRect(0, 228, 320, 12, C_BG);
        tft.setTextColor(C_YELLOW);
        tft.setTextSize(1);
        tft.setCursor(8, 228);
        tft.print("Block via web dashboard.");
        delay(1500);
        drawAdminMenu();
      } else if (tx > 162 && ty > 108 && ty < 152) {    // Web Register
        currentScreen = SCR_LOG_WEB; drawWebInfo();
      } else if (tx < 160 && ty > 158 && ty < 202) {    // Open Lock (emergency)
        drawButton(8, 160, 148, 40, "Opening...", C_GREEN, C_WHITE);
        digitalWrite(RELAY_PIN, LOCK_OPEN);
        delay(UNLOCK_MS);
        digitalWrite(RELAY_PIN, LOCK_CLOSE);
        drawAdminMenu();
      } else if (tx > 162 && ty > 158 && ty < 202) {    // Card Count info
        tft.fillRect(0, 228, 320, 12, C_BG);
        tft.setTextColor(C_CYAN);
        tft.setTextSize(1);
        tft.setCursor(8, 228);
        tft.print("Cards: "); tft.print(cardCount);
        tft.print(" / "); tft.print(MAX_CARDS);
        delay(1500);
        drawAdminMenu();
      }
      break;

    // ── VIEW LOG ──────────────────────────────────────────────
    case SCR_VIEW_LOG:
      if (ty > 218) {
        if (tx < 80) {
          if (logPage > 0) { logPage--; drawLogScreen(logPage); }
        } else if (tx > 240) {
          logPage++; drawLogScreen(logPage);
        } else {
          currentScreen = SCR_ADMIN_MENU; drawAdminMenu();
        }
      }
      break;

    // ── SCAN NEW CARD — Cancel ────────────────────────────────
    case SCR_SCAN_NEW:
      if (tx < 68 && ty > 210) {
        waitingForNewCard = false;
        currentScreen = SCR_ADMIN_MENU;
        drawAdminMenu();
      }
      break;

    // ── WEB INFO — Back ───────────────────────────────────────
    case SCR_LOG_WEB:
      if (tx < 68 && ty > 210) { currentScreen = SCR_ADMIN_MENU; drawAdminMenu(); }
      break;

    default: break;
  }
}

// =================================================================
//  WEB SERVER — HTML dashboard + JSON API
// =================================================================
void handleRoot() {
  String html = R"RAW(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Door Lock</title>
<style>
:root{
  --bg:#0f172a;--card:#111827;--card2:#1f2937;--bd:#334155;--text:#e5e7eb;--muted:#9ca3af;
  --green:#10b981;--red:#ef4444;--blue:#3b82f6;--orange:#f59e0b
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:linear-gradient(180deg,#0b1222,#0f172a);color:var(--text);min-height:100vh;padding:16px}
.app{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:12px}
.topbar{display:flex;gap:10px}
.navBtn{flex:1;border:1px solid var(--bd);background:var(--card2);color:var(--text);border-radius:12px;padding:11px;cursor:pointer;font-weight:700}
.card{background:rgba(17,24,39,.95);border:1px solid #243042;border-radius:14px;padding:16px;box-shadow:0 8px 26px rgba(0,0,0,.25)}
.title{font-size:1.2rem;font-weight:700}
.subtitle{margin-top:6px;color:var(--muted);font-size:.94rem}
.status{display:inline-block;margin-top:10px;padding:8px 12px;border-radius:999px;font-weight:700;font-size:.9rem}
.locked{background:rgba(239,68,68,.2);color:#fecaca}
.unlocked{background:rgba(16,185,129,.2);color:#bbf7d0}
.btnGrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}
.action{border:none;border-radius:12px;padding:12px;color:#fff;cursor:pointer;font-weight:700}
.lock{background:var(--red)} .unlock{background:var(--green)}
.scan{margin-top:12px;background:var(--blue);width:100%}
.hint{margin-top:8px;color:var(--orange);font-size:.9rem}
.hidden{display:none}
.toolbar{display:flex;gap:8px;margin:8px 0 10px}
.flt{border:1px solid var(--bd);border-radius:10px;background:#202a3a;color:var(--text);padding:6px 10px;font-size:.82rem;cursor:pointer}
.flt.on{border-color:var(--blue);color:#93c5fd}
.src{margin-left:auto;width:170px;border:1px solid var(--bd);border-radius:10px;background:#202a3a;color:var(--text);padding:6px 10px;font-size:.82rem;outline:none}
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;padding:8px 6px;border-bottom:1px solid var(--bd);color:var(--muted);font-size:11px}
td{padding:8px 6px;border-bottom:1px dashed #243042}
.uid{font-family:monospace;color:#9fb0c7}
.g{color:#86efac}.d{color:#fca5a5}
.footer{text-align:center;color:var(--muted);font-size:.82rem}
</style></head><body>
<div class="app">
  <div class="topbar">
    <button class="navBtn" id="homeBtn">Home</button>
    <button class="navBtn" id="backBtn">Back</button>
  </div>

  <section class="card screen" id="homeScreen">
    <div class="title">Smart Door Lock</div>
    <div class="subtitle">Tap RFID card on the physical reader</div>
    <div style="font-size:2.2rem;margin-top:10px">📶</div>
    <button class="action scan" id="goControlBtn">Open Control Panel</button>
    <div class="hint">Hold card steady for 1-2 seconds.</div>
  </section>

  <section class="card screen hidden" id="controlScreen">
    <div class="title">Door Control</div>
    <div class="subtitle">Remote lock management</div>
    <div id="statusBadge" class="status locked">Locked</div>
    <div class="btnGrid">
      <button class="action lock" id="lockBtn">Lock</button>
      <button class="action unlock" id="unlockBtn">Unlock</button>
    </div>
  </section>

  <section class="card screen hidden" id="logsScreen">
    <div class="title">Access Logs</div>
    <div class="subtitle" id="upd">Loading...</div>
    <div class="toolbar">
      <button class="flt on" onclick="setFilter('all',this)">All</button>
      <button class="flt" onclick="setFilter('granted',this)">Granted</button>
      <button class="flt" onclick="setFilter('denied',this)">Denied</button>
      <input class="src" id="q" placeholder="Search name..." oninput="renderLogs()">
    </div>
    <table><thead><tr><th>#</th><th>Time</th><th>Name</th><th>UID</th><th>Status</th></tr></thead>
    <tbody id="tb"><tr><td colspan="5" style="text-align:center;color:var(--muted);padding:14px">Loading...</td></tr></tbody></table>
  </section>

  <div class="footer">ESP32 Smart Door Dashboard</div>
</div>
<script>
const screens={
  home:document.getElementById('homeScreen'),
  control:document.getElementById('controlScreen'),
  logs:document.getElementById('logsScreen')
};
const navHistory=[];
let allLogs=[];
let curFilter='all';

function currentScreenName(){
  for(const k in screens){if(!screens[k].classList.contains('hidden')) return k;}
  return 'home';
}
function showScreen(name,track=true){
  const cur=currentScreenName();
  if(track && cur!==name) navHistory.push(cur);
  for(const k in screens) screens[k].classList.add('hidden');
  screens[name].classList.remove('hidden');
}

function statusClass(locked){return locked?'status locked':'status unlocked';}
function statusText(locked){return locked?'Locked':'Unlocked';}

async function refreshStatus(){
  try{
    const r=await fetch('/api/status');
    const s=await r.json();
    const b=document.getElementById('statusBadge');
    b.className=statusClass(s.locked);
    b.textContent=statusText(s.locked);
  }catch(_){}
}

async function sendAction(url){
  try{
    await fetch(url,{method:'POST'});
    await refreshStatus();
    await loadLogs();
  }catch(_){}
}

function setFilter(f,b){
  curFilter=f;
  document.querySelectorAll('.flt').forEach(x=>x.classList.remove('on'));
  b.classList.add('on');
  renderLogs();
}

function renderLogs(){
  const q=document.getElementById('q').value.toLowerCase();
  const d=allLogs.filter(e=>(curFilter==='all'||(curFilter==='granted'&&e.granted)||(curFilter==='denied'&&!e.granted))
    &&(!q||e.name.toLowerCase().includes(q))).slice().reverse();
  document.getElementById('tb').innerHTML=d.map((e,i)=>`<tr>
    <td>${d.length-i}</td>
    <td style="color:var(--muted)">${e.ts}</td>
    <td>${e.name}</td>
    <td class="uid">${e.uid}</td>
    <td class="${e.granted?'g':'d'}">${e.granted?'Granted':'Denied'}</td>
  </tr>`).join('')||'<tr><td colspan="5" style="text-align:center;color:var(--muted);padding:14px">No records</td></tr>';
}

async function loadLogs(){
  try{
    const r=await fetch('/api/log');
    allLogs=await r.json();
    document.getElementById('upd').textContent='Updated '+new Date().toLocaleTimeString();
    renderLogs();
  }catch(_){
    document.getElementById('tb').innerHTML='<tr><td colspan="5" style="text-align:center;color:#fca5a5;padding:14px">Connection error</td></tr>';
  }
}

document.getElementById('goControlBtn').addEventListener('click',()=>{
  showScreen('control');
  showScreen('logs');
});
document.getElementById('homeBtn').addEventListener('click',()=>showScreen('home'));
document.getElementById('backBtn').addEventListener('click',()=>{
  if(navHistory.length){
    const prev=navHistory.pop();
    showScreen(prev,false);
  }
});
document.getElementById('lockBtn').addEventListener('click',()=>sendAction('/api/lock'));
document.getElementById('unlockBtn').addEventListener('click',()=>sendAction('/api/unlock'));

refreshStatus();
loadLogs();
setInterval(()=>{refreshStatus();loadLogs();},4000);
</script></body></html>)RAW";
  server.send(200, "text/html", html);
}

void handleLogJSON() {
  String json = "[";
  int start = (logCount < MAX_LOG) ? 0 : logHead;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG;
    if (i) json += ",";
    // Escape name in case it contains quotes
    String safeName = logBuf[idx].name;
    safeName.replace("\"", "'");
    json += "{\"uid\":\""     + logBuf[idx].uid  + "\","
            "\"name\":\""     + safeName          + "\","
            "\"role\":\""     + logBuf[idx].role  + "\","
            "\"granted\":"    + (logBuf[idx].granted ? "true" : "false") + ","
            "\"ts\":\""       + logBuf[idx].ts    + "\"}";
  }
  json += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleStatusJSON() {
  bool locked = (digitalRead(RELAY_PIN) == LOCK_CLOSE);
  String json = "{\"locked\":";
  json += locked ? "true" : "false";
  json += ",\"logCount\":";
  json += String(logCount);
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleUnlock() {
  digitalWrite(RELAY_PIN, LOCK_OPEN);
  addLog("WEB", "Web Panel", "Remote", true);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"unlock\"}");
}

void handleLock() {
  digitalWrite(RELAY_PIN, LOCK_CLOSE);
  addLog("WEB", "Web Panel", "Remote", true);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"lock\"}");
}

// =================================================================
//  SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Relay — start locked
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOCK_CLOSE);

  // TFT (HSPI)
  hSPI.begin(TFT_SCK, TOUCH_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextColor(C_CYAN);
  tft.setTextSize(2);
  tft.setCursor(20, 90);
  tft.print("Smart Door Lock");
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(20, 116);
  tft.print("Initializing...");

  // Touch (shares HSPI)
  ts.begin(hSPI);
  ts.setRotation(1);

  // RFID (VSPI)
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID ready.");
  rfid.PCD_DumpVersionToSerial();

  // WiFi
  tft.setCursor(20, 136);
  tft.setTextColor(C_YELLOW);
  tft.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 20) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    // Nepal time UTC+5:45
    configTime(5 * 3600 + 45 * 60, 0, "pool.ntp.org", "time.nist.gov");
    tft.setTextColor(C_GREEN);
    tft.setCursor(20, 156);
    tft.print("WiFi: "); tft.print(WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi offline — running standalone.");
    tft.setTextColor(C_ORANGE);
    tft.setCursor(20, 156);
    tft.print("WiFi offline — standalone mode.");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/api/log", handleLogJSON);
  server.on("/api/status", HTTP_GET, handleStatusJSON);
  server.on("/api/unlock", HTTP_POST, handleUnlock);
  server.on("/api/lock", HTTP_POST, handleLock);
  server.begin();

  delay(1200);
  drawIdle();
  currentScreen = SCR_IDLE;
  Serial.println("System ready. Scan card or touch screen.");
}

// =================================================================
//  MAIN LOOP
// =================================================================
void loop() {
  server.handleClient();

  // ── Touch input ─────────────────────────────────────────────
  int tx, ty;
  if (getTouchPoint(tx, ty)) {
    handleTouch(tx, ty);
  }

  // ── Waiting to register a new card ──────────────────────────
  if (waitingForNewCard && currentScreen == SCR_SCAN_NEW) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = getUID();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      if (findCard(uid) >= 0) {
        // Already in database
        tft.setTextColor(C_YELLOW);
        tft.setTextSize(1);
        tft.setCursor(20, 200);
        tft.print("Card already registered!");
        delay(1500);
        drawScanNew();
      } else if (cardCount < MAX_CARDS) {
        cards[cardCount] = { uid, "New User " + String(cardCount), "Staff", false };
        cardCount++;
        waitingForNewCard = false;

        tft.fillScreen(C_BG);
        drawHeader("  CARD ADDED    ", 0x0230);
        tft.setTextColor(C_GREEN);
        tft.setTextSize(2);
        tft.setCursor(20, 80);
        tft.print("Card added!");
        tft.setTextColor(C_GRAY);
        tft.setTextSize(1);
        tft.setCursor(20, 120);
        tft.print("UID: " + uid);
        tft.setCursor(20, 140);
        tft.print("Edit name/role via web dashboard.");
        delay(2500);
        currentScreen = SCR_ADMIN_MENU;
        drawAdminMenu();
      } else {
        tft.setTextColor(C_RED);
        tft.setTextSize(1);
        tft.setCursor(20, 200);
        tft.print("Card database full! Max 20.");
        delay(1500);
        drawScanNew();
      }
      return;
    }
  }

  // ── Normal RFID scan (only from idle screen) ─────────────────
  if (currentScreen != SCR_IDLE) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = getUID();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  Serial.println("Card scanned: " + uid);

  int idx = findCard(uid);

  if (idx >= 0 && cards[idx].blocked) {
    // BLOCKED
    addLog(uid, cards[idx].name, cards[idx].role, false);
    drawBlocked(cards[idx].name);
    delay(MSG_MS);
    drawIdle();

  } else if (idx >= 0) {
    // GRANTED
    addLog(uid, cards[idx].name, cards[idx].role, true);
    drawGranted(cards[idx].name, cards[idx].role);
    digitalWrite(RELAY_PIN, LOCK_OPEN);
    delay(UNLOCK_MS);
    digitalWrite(RELAY_PIN, LOCK_CLOSE);
    delay(500);
    drawIdle();

  } else {
    // DENIED — unknown card
    addLog(uid, "Unknown", "N/A", false);
    drawDenied(uid);
    delay(MSG_MS);
    drawIdle();
  }
}
