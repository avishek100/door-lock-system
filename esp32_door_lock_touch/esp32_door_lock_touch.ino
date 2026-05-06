
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

// ── WiFi ──────────────────────────────────────────────────────
const char *WIFI_SSID = "Bikash_DHInternet";
const char *WIFI_PASSWORD = "DHInternet@20992086";

// ── Pins ──────────────────────────────────────────────────────
// RFID — VSPI (default SPI)
#define RFID_SS 5
#define RFID_RST 4
#define RFID_MOSI 23
#define RFID_MISO 19
#define RFID_SCK 18

// TFT — HSPI
#define TFT_CS 15
#define TFT_RST 25
#define TFT_DC 12
#define TFT_SCK 14
#define TFT_MOSI 13

// Touch — shares HSPI SCK/MOSI, has own CS and MISO
#define TOUCH_CS 22
#define TOUCH_IRQ 21
#define TOUCH_MISO 27 // T_DO pin

// Relay
#define RELAY_PIN 2
#define LOCK_OPEN LOW // Active-LOW relay: LOW = unlock
#define LOCK_CLOSE HIGH

// ── Timing ────────────────────────────────────────────────────
#define UNLOCK_MS 3000         // Door open duration
#define MSG_MS 1500            // Result screen duration (faster)
#define TOUCH_DEBOUNCE 200     // ms between touch events (more responsive)
#define RFID_SCAN_TIMEOUT 100  // RFID polling timeout
#define DISPLAY_UPDATE_RATE 20 // ms between display updates
// ── Touch Calibration Values ──────────────────────────────
// PORTRAIT MODE - Calibrated with corner touches
// Top-left raw: x=3734, y=3915 → screen (0, 0)
// Bottom-right raw: x=592, y=500 → screen (239, 319)
#define TOUCH_X_MIN 592
#define TOUCH_X_MAX 3734
#define TOUCH_Y_MIN 500
#define TOUCH_Y_MAX 3915
#define SCREEN_W 240
#define SCREEN_H 320

// ── Navigation Bar ───────────────────────────────────────────────
#define NAV_HEIGHT 28
#define NAV_Y (SCREEN_H - NAV_HEIGHT) // 292
#define CONTENT_H (NAV_Y - 36)        // 256 (below header, above nav)

// ── Colors (RGB565) — Vibrant & Saturated ─────────────────────
#define C_BG 0x0000      // Black background
#define C_PANEL 0x2945   // Dark teal
#define C_PANEL2 0x18C3  // Deep navy panel
#define C_WHITE 0xFFFF   // Pure white
#define C_GREEN 0x07E0   // Bright lime green
#define C_DKGREEN 0x0400 // Dark green
#define C_RED 0xF800     // Bright red
#define C_BLUE 0x001F    // Bright blue
#define C_LBLUE 0x049F   // Light bright blue
#define C_YELLOW 0xFFE0  // Bright yellow
#define C_CYAN 0x07FF    // Bright cyan
#define C_ORANGE 0xFE60  // Bright orange
#define C_GRAY 0x8410    // Medium gray
#define C_DKGRAY 0x4208  // Dark gray
#define C_PURPLE 0xF81F  // Bright magenta/purple
#define C_HEADER 0x00FF  // Bright blue for header
#define C_TEXT 0xC618    // Light gray text
#define C_MUTED 0x7BEF   // Soft muted text

// ── Admin PIN ─────────────────────────────────────────────────
const String ADMIN_PIN = "1234";

// ── Card database ─────────────────────────────────────────────
#define MAX_CARDS 20
struct Card
{
  String uid;
  String name;
  String role;
  bool blocked;
};

Card cards[MAX_CARDS] = {
    // *** REPLACE UIDs with actual values from Serial Monitor ***
    {"A1B2C3D4", "Alice Johnson", "Admin", false},
    {"12345678", "Bob Smith", "Staff", false},
    {"AABBCCDD", "Carol Williams", "Visitor", false},
    {"11223344", "David Brown", "Staff", false},
    {"DEADBEEF", "Eve Davis", "Admin", false},
};
int cardCount = 5;

// ── Calibration test data ────────────────────────────────────
struct CalibPoint
{
  int screen_x, screen_y; // Target screen coordinates
  int raw_x, raw_y;       // Captured raw touch coordinates
  bool captured;
};

CalibPoint calibPoints[4] = {
    {10, 45},  // Top-left (in header area)
    {230, 45}, // Top-right
    {10, 310}, // Bottom-left
    {230, 310} // Bottom-right
};
int calibIdx = 0;

// ── Access log (circular, last 50 entries) ────────────────────
#define MAX_LOG 50
struct LogEntry
{
  String uid, name, role, ts;
  bool granted;
};
LogEntry logBuf[MAX_LOG];
int logCount = 0, logHead = 0;

// ── Screen states ─────────────────────────────────────────────
enum Screen
{
  SCR_IDLE,
  SCR_GRANTED,
  SCR_DENIED,
  SCR_ADMIN_PIN,
  SCR_ADMIN_MENU,
  SCR_VIEW_LOG,
  SCR_SCAN_NEW,
  SCR_LOG_WEB,
  SCR_CALIBRATE
};
Screen currentScreen = SCR_IDLE;

// ── Hardware objects ──────────────────────────────────────────
MFRC522 rfid(RFID_SS, RFID_RST);

SPIClass hSPI(HSPI);
Adafruit_ILI9341 tft(&hSPI, TFT_DC, TFT_CS, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

WebServer server(80);

// ── State ─────────────────────────────────────────────────────
// ── State for UI messaging ───────────────────────────────────
unsigned long lastTouch = 0;
unsigned long lastRFIDScan = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long messageTimeout = 0;
String adminPinEntry = "";
bool waitingForNewCard = false;
int logPage = 0;
bool needsRedraw = true;
bool showingMessage = false;
String messageText = "";

// =================================================================
//  HELPERS
// =================================================================
String getUID()
{
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

int findCard(const String &uid)
{
  for (int i = 0; i < cardCount; i++)
    if (cards[i].uid.equalsIgnoreCase(uid))
      return i;
  return -1;
}

void addLog(const String &uid, const String &name,
            const String &role, bool granted)
{
  logBuf[logHead] = {uid, name, role, "", granted};
  logHead = (logHead + 1) % MAX_LOG;
  if (logCount < MAX_LOG)
    logCount++;
}

// Calculate calibration values from captured points
void calculateCalibration()
{
  // Find min/max raw values
  int min_x = calibPoints[0].raw_x, max_x = calibPoints[0].raw_x;
  int min_y = calibPoints[0].raw_y, max_y = calibPoints[0].raw_y;

  for (int i = 1; i < 4; i++)
  {
    min_x = min(min_x, calibPoints[i].raw_x);
    max_x = max(max_x, calibPoints[i].raw_x);
    min_y = min(min_y, calibPoints[i].raw_y);
    max_y = max(max_y, calibPoints[i].raw_y);
  }

  // Calculate touch range (add small margin)
  int range_x = max_x - min_x;
  int range_y = max_y - min_y;

  Serial.println("\n=== CALIBRATION RESULTS ===");
  Serial.printf("X range: %d to %d (range: %d)\n", min_x, max_x, range_x);
  Serial.printf("Y range: %d to %d (range: %d)\n", min_y, max_y, range_y);
  Serial.println("Captured points:");
  for (int i = 0; i < 4; i++)
  {
    Serial.printf("  Point %d: Screen(%d,%d) -> Raw(%d,%d)\n",
                  i, calibPoints[i].screen_x, calibPoints[i].screen_y,
                  calibPoints[i].raw_x, calibPoints[i].raw_y);
  }

  // Print suggested calibration #defines
  Serial.println("\nSuggested calibration values:");
  Serial.printf("#define TOUCH_X_MIN %d\n", min_x);
  Serial.printf("#define TOUCH_X_MAX %d\n", max_x);
  Serial.printf("#define TOUCH_Y_MIN %d\n", min_y);
  Serial.printf("#define TOUCH_Y_MAX %d\n", max_y);
  Serial.println("=== Copy these values to the #define section ===\n");
}

// Map raw touch → screen pixels (landscape) with timeout
bool getTouchPoint(int &sx, int &sy)
{
  if (!ts.tirqTouched() || !ts.touched())
    return false;
  unsigned long now = millis();
  if (now - lastTouch < TOUCH_DEBOUNCE)
    return false;
  lastTouch = now;
  // Average multiple reads for stability
  int x_sum = 0, y_sum = 0;
  for (int i = 0; i < 3; i++)
  {
    if (!ts.touched())
      return false;
    TS_Point p = ts.getPoint();
    x_sum += p.x;
    y_sum += p.y;
  }
  int p_x = x_sum / 3;
  int p_y = y_sum / 3;

  // Portrait mode: coordinates are inverted, no swap needed
  sx = map(p_x, TOUCH_X_MAX, TOUCH_X_MIN, 0, SCREEN_W);
  sy = map(p_y, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, SCREEN_H);
  sx = constrain(sx, 0, SCREEN_W - 1);
  sy = constrain(sy, 0, SCREEN_H - 1);

  // DETAILED DEBUG: Show raw coords, calibration range, and mapped result
  Serial.printf("TOUCH DEBUG | RAW: x=%4d y=%4d | RANGE: x[%d-%d] y[%d-%d] | MAPPED: x=%3d y=%3d\n",
                p_x, p_y,
                TOUCH_X_MIN, TOUCH_X_MAX, TOUCH_Y_MIN, TOUCH_Y_MAX,
                sx, sy);
  return true;
}

// Get raw touch coordinates (for calibration)
bool getRawTouchPoint(int &raw_x, int &raw_y)
{
  if (!ts.tirqTouched() || !ts.touched())
    return false;
  unsigned long now = millis();
  if (now - lastTouch < TOUCH_DEBOUNCE)
    return false;
  lastTouch = now;
  // Average multiple reads for stability
  int x_sum = 0, y_sum = 0;
  for (int i = 0; i < 3; i++)
  {
    if (!ts.touched())
      return false;
    TS_Point p = ts.getPoint();
    x_sum += p.x;
    y_sum += p.y;
  }
  raw_x = x_sum / 3;
  raw_y = y_sum / 3;
  return true;
}

// =================================================================
//  DRAW HELPERS
// =================================================================

// ── Common header bar with time ───────────────────────────────
void drawHeader(const char *title, uint16_t color)
{
  // Header background
  tft.fillRect(0, 0, SCREEN_W, 36, C_HEADER);

  // Draw title centered — choose size2 when it fits, otherwise size1
  int tlen = strlen(title);
  int titleWidth = tlen * 12; // approx width for textSize=2 (6*2)
  int titleX;
  if (titleWidth <= SCREEN_W - 40)
  {
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    titleX = max(4, (SCREEN_W - titleWidth) / 2);
  }
  else
  {
    tft.setTextSize(1);
    tft.setTextColor(C_WHITE);
    titleX = max(4, (SCREEN_W - tlen * 6) / 2);
  }
  tft.setCursor(titleX, 6);
  tft.print(title);

  // IP display on second line
  String ipStr;
  if (WiFi.status() == WL_CONNECTED)
    ipStr = WiFi.localIP().toString();
  else
    ipStr = "WiFi offline";

  int ipLen = ipStr.length();
  int ipX = SCREEN_W - 8 - ipLen * 6;

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(max(8, ipX), 22);
  tft.print(ipStr);
}

// ── Rounded button with optional press effect ────────────────────
void drawButton(int x, int y, int w, int h,
                const char *label, uint16_t bg, uint16_t fg = C_WHITE, bool pressed = false)
{
  // Pressed state: slightly darker and offset
  if (pressed)
  {
    tft.fillRoundRect(x + 1, y + 1, w, h, 6, bg >> 1); // darker
    tft.drawRoundRect(x + 1, y + 1, w, h, 6, fg);
    int tx = x + 2 + (w - strlen(label) * 6) / 2;
    int ty = y + 2 + (h - 8) / 2;
    tft.setTextColor(fg);
    tft.setTextSize(1);
    tft.setCursor(tx, ty);
    tft.print(label);
  }
  else
  {
    tft.fillRoundRect(x, y, w, h, 6, bg);
    tft.drawRoundRect(x, y, w, h, 6, fg);
    int tx = x + (w - strlen(label) * 6) / 2;
    int ty = y + (h - 8) / 2;
    tft.setTextColor(fg);
    tft.setTextSize(1);
    tft.setCursor(tx, ty);
    tft.print(label);
  }
}

// ── Padlock shackle drawn without drawArc ─────────────────────
// Approximates a U-shape using pixel-plotted semicircle
void drawShackle(int cx, int topY, int outerR, int innerR, uint16_t color)
{
  for (int angle = 0; angle <= 180; angle += 3)
  {
    float rad = angle * PI / 180.0;
    // Outer arc
    int ox = cx + (int)(outerR * cos(rad));
    int oy = topY - (int)(outerR * sin(rad));
    tft.drawPixel(ox, oy, color);
    tft.drawPixel(ox + 1, oy, color); // 2px thick
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

// ── Fixed Navigation Bar (Bottom) ─────────────────────────────
void drawNavBar()
{
  // Background
  tft.fillRect(0, NAV_Y, SCREEN_W, NAV_HEIGHT, C_DKGRAY);
  tft.drawLine(0, NAV_Y, SCREEN_W, NAV_Y, C_GRAY);

  // Back button (left) — text centered inside button
  tft.fillRoundRect(8, NAV_Y + 2, 110, 24, 4, C_RED);
  tft.drawRoundRect(8, NAV_Y + 2, 110, 24, 4, C_WHITE);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  int backTextX = 8 + (110 - strlen("< Back") * 6) / 2;
  int backTextY = NAV_Y + 2 + (24 - 8) / 2;
  tft.setCursor(backTextX, backTextY);
  tft.print("< Back");

  // Home button (right) — text centered inside button
  tft.fillRoundRect(122, NAV_Y + 2, 110, 24, 4, C_LBLUE);
  tft.drawRoundRect(122, NAV_Y + 2, 110, 24, 4, C_WHITE);
  tft.setTextColor(C_WHITE);
  int homeTextX = 122 + (110 - strlen("Home") * 6) / 2;
  int homeTextY = NAV_Y + 2 + (24 - 8) / 2;
  tft.setCursor(homeTextX, homeTextY);
  tft.print("Home");
}

// =================================================================
//  SCREEN DRAW FUNCTIONS
// =================================================================

// ── IDLE ──────────────────────────────────────────────────────
void drawIdle()
{
  tft.fillScreen(C_BG);
  drawHeader("SMART DOOR LOCK", 0x0210);

  // Main status card
  tft.fillRoundRect(10, 44, 220, 170, 12, C_PANEL2);
  tft.drawRoundRect(10, 44, 220, 170, 12, C_CYAN);

  tft.setTextColor(C_CYAN);
  tft.setTextSize(1);
  tft.setCursor(18, 56);
  tft.print("SMART DOOR LOCK");

  tft.fillCircle(120, 105, 42, C_DKGRAY);
  drawShackle(120, 84, 26, 20, C_WHITE);
  tft.fillRect(100, 114, 40, 38, C_WHITE);
  tft.fillCircle(120, 123, 10, C_DKGRAY);
  tft.fillRect(118, 133, 4, 10, C_DKGRAY);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(28, 170);
  tft.print("Tap RFID Card");

  tft.setTextColor(C_MUTED);
  tft.setTextSize(1);
  tft.setCursor(28, 190);
  tft.print("or use PIN login from below");

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(18, 215);
  tft.print("Logs: ");
  tft.print(logCount);
  tft.print(" entries");

  drawButton(70, 240, 100, 30, "PIN LOGIN", C_PURPLE, C_WHITE);
  drawNavBar();
}

// ── ACCESS GRANTED ────────────────────────────────────────────
void drawGranted(const String &name, const String &role)
{
  tft.fillScreen(C_BG);
  drawHeader("ACCESS GRANTED", C_DKGREEN);

  tft.fillRoundRect(12, 46, 216, 160, 14, C_PANEL2);
  tft.drawRoundRect(12, 46, 216, 160, 14, C_GREEN);

  tft.setTextColor(C_GREEN);
  tft.setTextSize(5);
  tft.setCursor(92, 60);
  tft.print("✓");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 132);
  tft.print("ACCESS GRANTED");

  tft.setTextColor(C_CYAN);
  tft.setTextSize(1);
  tft.setCursor(20, 158);
  tft.print(name);

  tft.setTextColor(C_MUTED);
  tft.setCursor(20, 174);
  tft.print(role);

  tft.setTextColor(C_GRAY);
  tft.setCursor(20, 192);
  tft.print("Door unlocked");
}

// ── ACCESS DENIED ─────────────────────────────────────────────
void drawDenied(const String &uid)
{
  tft.fillScreen(C_BG);
  drawHeader("ACCESS DENIED", C_RED);

  tft.fillRoundRect(12, 46, 216, 160, 14, C_PANEL2);
  tft.drawRoundRect(12, 46, 216, 160, 14, C_RED);

  tft.setTextColor(C_RED);
  tft.setTextSize(5);
  tft.setCursor(92, 60);
  tft.print("✗");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 132);
  tft.print("ACCESS DENIED");

  tft.setTextColor(C_MUTED);
  tft.setTextSize(1);
  tft.setCursor(20, 158);
  tft.print("Unauthorized card");

  tft.setTextColor(C_GRAY);
  tft.setCursor(20, 176);
  tft.print("UID: " + uid);
  tft.setCursor(20, 190);
  tft.print("Please try again.");
}

// ── BLOCKED ───────────────────────────────────────────────────
void drawBlocked(const String &name)
{
  tft.fillScreen(C_BG);
  drawHeader("   CARD BLOCKED ", C_ORANGE >> 1);

  tft.setTextColor(C_ORANGE);
  tft.setTextSize(3);
  tft.setCursor(124, 50);
  tft.print("!");

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("Card blocked:");
  tft.setCursor(8, 135);
  tft.setTextColor(C_ORANGE);
  tft.print(name);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 193);
  tft.print("Contact manager.");
}

// ── LOGIN WITH PIN ENTRY ─────────────────────────────
void drawAdminPin()
{
  tft.fillScreen(C_BG);
  drawHeader("PIN LOGIN", C_PURPLE);

  tft.fillRoundRect(10, 44, 220, 76, 12, C_PANEL2);
  tft.drawRoundRect(10, 44, 220, 76, 12, C_CYAN);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(18, 56);
  tft.print("Enter PIN");

  int centerX = SCREEN_W / 2;
  int dotSpan = 90;
  int dotStartX = centerX - dotSpan / 2;
  for (int i = 0; i < 4; i++)
  {
    int dotX = dotStartX + i * 30;
    int dotY = 80;
    if (i < (int)adminPinEntry.length())
    {
      tft.fillCircle(dotX, dotY, 7, C_GREEN);
    }
    else
    {
      tft.drawCircle(dotX, dotY, 7, C_GRAY);
    }
  }

  const char *keys[] = {"1", "2", "3",
                        "4", "5", "6",
                        "7", "8", "9",
                        "CLR", "0", "OK"};
  uint16_t kcolors[] = {C_BLUE, C_BLUE, C_BLUE,
                        C_BLUE, C_BLUE, C_BLUE,
                        C_BLUE, C_BLUE, C_BLUE,
                        C_ORANGE, C_BLUE, C_GREEN};

  for (int i = 0; i < 12; i++)
  {
    int col = i % 3, row = i / 3;
    int bx = 8 + col * 76;
    int by = 108 + row * 46;
    drawButton(bx, by, 68, 42, keys[i], kcolors[i], C_WHITE);
  }
  drawNavBar();
}

// ── ADMIN MENU ────────────────────────────────────────────────
void drawAdminMenu()
{
  tft.fillScreen(C_BG);
  drawHeader("MANAGEMENT", C_PURPLE);

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  const char *cardText = "Registered cards: ";
  int cardWidth = strlen(cardText) * 6;
  int cardX = (SCREEN_W - cardWidth - 12) / 2;
  tft.setCursor(cardX, 44);
  tft.print(cardText);
  tft.setTextColor(C_CYAN);
  tft.print(cardCount);

  drawButton(8, 68, 224, 28, "View Log", C_CYAN, C_BG);
  drawButton(8, 100, 224, 28, "Add New Card", C_GREEN, C_BG);
  drawButton(8, 132, 224, 28, "Block Card", C_RED, C_WHITE);
  drawButton(8, 164, 224, 28, "Web Register", C_LBLUE, C_BG);
  drawButton(8, 196, 224, 28, "Open Lock", C_YELLOW, C_BG);
  drawNavBar();
}

// ── VIEW LOG ──────────────────────────────────────────────────
void drawLogScreen(int page)
{
  tft.fillScreen(C_BG);
  drawHeader("  ACCESS LOG    ", 0x0230);

  int perPage = 3;
  int total = logCount;
  int end = max(0, total - page * perPage);
  int start = max(0, end - perPage);

  tft.setTextSize(1);
  for (int i = start; i < end; i++)
  {
    // Walk backwards from newest: index in circular buffer
    int realIdx = (logHead - 1 - (logCount - 1 - i) % MAX_LOG + MAX_LOG * 2) % MAX_LOG;
    LogEntry &e = logBuf[realIdx];
    int y = 44 + (i - start) * 60;
    uint16_t col = e.granted ? C_DKGREEN : 0x4000;
    tft.fillRect(4, y, 232, 50, col);
    tft.drawRect(4, y, 232, 50, e.granted ? C_GREEN : C_RED);
    tft.setTextColor(C_WHITE);
    tft.setCursor(8, y + 6);
    tft.print(e.name.length() > 18 ? e.name.substring(0, 18) : e.name);
    tft.setTextColor(C_GRAY);
    tft.setCursor(8, y + 22);
    tft.print("Role: " + e.role);
    tft.setCursor(152, y + 6);
    tft.setTextColor(e.granted ? C_GREEN : C_RED);
    tft.print(e.granted ? "GRANTED" : "DENIED");
  }
  drawNavBar();
}

// ── SCAN NEW CARD ─────────────────────────────────────────────
void drawScanNew()
{
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
  drawNavBar();
}

// ── WEB REGISTER INFO ─────────────────────────────────────────
void drawWebInfo()
{
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
  tft.setCursor(8, 126);
  tft.print("- Full access log with timestamps");
  tft.setCursor(8, 140);
  tft.print("- Filter granted / denied entries");
  tft.setCursor(8, 154);
  tft.print("- Auto-refresh every 5 seconds");
  tft.setCursor(8, 168);
  tft.print("- Stats: total / granted / denied");

  drawNavBar();
}

// ── CALIBRATION TEST SCREEN ──────────────────────────────────
void drawCalibrationScreen()
{
  tft.fillScreen(C_BG);
  drawHeader("CALIBRATE", C_HEADER);

  tft.setTextColor(C_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(8, 50);
  tft.printf("Point %d/4 - Touch the target:", calibIdx + 1);

  // Draw target circles at each calibration point
  for (int i = 0; i < 4; i++)
  {
    uint16_t color = (i == calibIdx) ? C_GREEN : C_DKGRAY;
    tft.drawCircle(calibPoints[i].screen_x, calibPoints[i].screen_y, 12, color);
    tft.drawCircle(calibPoints[i].screen_x, calibPoints[i].screen_y, 10, color);

    // Fill center of active target
    if (i == calibIdx)
    {
      tft.fillCircle(calibPoints[i].screen_x, calibPoints[i].screen_y, 8, C_GREEN);
    }
    else if (calibPoints[i].captured)
    {
      tft.fillCircle(calibPoints[i].screen_x, calibPoints[i].screen_y, 6, C_DKGREEN);
    }
  }

  // Show coordinates of current target
  tft.setTextColor(C_CYAN);
  tft.setCursor(8, 100);
  tft.printf("Target: (%d, %d)", calibPoints[calibIdx].screen_x, calibPoints[calibIdx].screen_y);

  // Show captured coordinates
  tft.setTextColor(C_GRAY);
  tft.setCursor(8, 130);
  if (calibPoints[calibIdx].captured)
  {
    tft.printf("Raw: (%d, %d)", calibPoints[calibIdx].raw_x, calibPoints[calibIdx].raw_y);
  }
  else
  {
    tft.print("Waiting for touch...");
  }

  // Progress indicator
  tft.setTextColor(C_YELLOW);
  tft.setCursor(8, 160);
  for (int i = 0; i < 4; i++)
  {
    tft.print(calibPoints[i].captured ? "[X] " : "[ ] ");
  }

  // Instructions
  tft.setTextColor(C_ORANGE);
  tft.setTextSize(1);
  tft.setCursor(8, 200);
  tft.print("Press firmly on green circle");
  tft.setCursor(8, 214);
  tft.print("Button: Submit calibration");

  drawButton(8, 250, 100, 28, "Submit", C_GREEN, C_BG);
  drawButton(132, 250, 100, 28, "Cancel", C_RED, C_WHITE);
}

// =================================================================
//  TOUCH HANDLER
// =================================================================
void handleTouch(int tx, int ty)
{
  Serial.printf("Touch: (%d, %d) screen=%d\n", tx, ty, currentScreen);

  // Global nav-bar handling: Back (left) and Home (right)
  if (ty >= NAV_Y)
  {
    // Back button: x=8..118
    if (tx >= 8 && tx <= 118)
    {
      switch (currentScreen)
      {
      case SCR_VIEW_LOG:
        currentScreen = SCR_ADMIN_MENU;
        drawAdminMenu();
        break;
      case SCR_SCAN_NEW:
      case SCR_LOG_WEB:
        currentScreen = SCR_ADMIN_MENU;
        drawAdminMenu();
        break;
      case SCR_ADMIN_PIN:
      case SCR_ADMIN_MENU:
      case SCR_CALIBRATE:
      default:
        currentScreen = SCR_IDLE;
        drawIdle();
        break;
      }
      return;
    }

    // Home button: x=122..232
    if (tx >= 122 && tx <= 232)
    {
      currentScreen = SCR_IDLE;
      drawIdle();
      return;
    }
  }

  switch (currentScreen)
  {

  // ── CALIBRATION ───────────────────────────────────────────
  case SCR_CALIBRATE:
  {
    // Submit button: x=8..108, y=250..278
    if (tx >= 8 && tx <= 108 && ty >= 250 && ty <= 278)
    {
      // Check if all points captured
      bool allCaptured = true;
      for (int i = 0; i < 4; i++)
      {
        if (!calibPoints[i].captured)
        {
          allCaptured = false;
          break;
        }
      }

      if (allCaptured)
      {
        calculateCalibration();
        currentScreen = SCR_IDLE;
        drawIdle();
      }
      else
      {
        tft.fillRect(0, 180, 240, 20, C_RED);
        tft.setTextColor(C_WHITE);
        tft.setTextSize(1);
        tft.setCursor(20, 185);
        tft.print("Capture all 4 points first!");
        delay(1500);
        drawCalibrationScreen();
      }
      return;
    }

    // Cancel button: x=132..232, y=250..278
    if (tx >= 132 && tx <= 232 && ty >= 250 && ty <= 278)
    {
      // Reset calibration data
      for (int i = 0; i < 4; i++)
        calibPoints[i].captured = false;
      calibIdx = 0;

      currentScreen = SCR_IDLE;
      drawIdle();
      return;
    }

    // Capture touch on target
    if (!calibPoints[calibIdx].captured && tx >= 40 && ty >= 40)
    {
      // Accept touch if near target (within 30 pixels)
      int dx = abs(tx - calibPoints[calibIdx].screen_x);
      int dy = abs(ty - calibPoints[calibIdx].screen_y);

      if (dx <= 30 && dy <= 30)
      {
        // Get raw coordinates for this touch
        int raw_x, raw_y;
        if (getRawTouchPoint(raw_x, raw_y))
        {
          calibPoints[calibIdx].raw_x = raw_x;
          calibPoints[calibIdx].raw_y = raw_y;
          calibPoints[calibIdx].captured = true;

          Serial.printf("Calibration point %d captured: Screen(%d,%d) -> Raw(%d,%d)\n",
                        calibIdx, calibPoints[calibIdx].screen_x, calibPoints[calibIdx].screen_y,
                        raw_x, raw_y);

          // Move to next point
          if (calibIdx < 3)
          {
            calibIdx++;
            delay(300);
          }
          drawCalibrationScreen();
        }
      }
      else
      {
        tft.fillRect(0, 90, 240, 20, C_ORANGE);
        tft.setTextColor(C_BG);
        tft.setTextSize(1);
        tft.setCursor(20, 95);
        tft.print("Touch the green circle exactly!");
        delay(800);
        drawCalibrationScreen();
      }
    }
    break;
  }

  // ── PIN ENTRY ─────────────────────────────────────────────
  case SCR_ADMIN_PIN:
  {
    // Numpad grid: 3 cols × 4 rows (matches drawAdminPin layout)
    if (tx >= 8 && tx <= 228 && ty >= 108 && ty < NAV_Y)
    {
      int col = (tx - 8) / 76;   // horiz step used in draw
      int row = (ty - 108) / 46; // vertical step used in draw
      int key = row * 3 + col;
      const char *keys[] = {"1", "2", "3",
                            "4", "5", "6",
                            "7", "8", "9",
                            "CLR", "0", "OK"};
      if (key >= 0 && key < 12)
      {
        // Show button press animation
        int bx = 8 + col * 76;
        int by = 108 + row * 46;
        drawButton(bx, by, 68, 42, keys[key],
                   (key == 9 ? C_ORANGE : (key == 11 ? C_GREEN : C_LBLUE)),
                   C_WHITE, true); // pressed=true
        delay(80);                 // brief haptic feedback delay

        String k = keys[key];
        if (k == "CLR")
        {
          if (adminPinEntry.length())
            adminPinEntry.remove(adminPinEntry.length() - 1);
        }
        else if (k == "OK")
        {
          if (adminPinEntry == ADMIN_PIN)
          {
            currentScreen = SCR_ADMIN_MENU;
            drawAdminMenu();
          }
          else
          {
            // Non-blocking error feedback
            showingMessage = true;
            messageText = "Wrong PIN! Try again.";
            messageTimeout = millis() + 1000;
            adminPinEntry = "";
            // Flash error on screen
            tft.fillRect(30, 74, 180, 18, C_RED);
            tft.setTextColor(C_WHITE);
            tft.setTextSize(1);
            tft.setCursor(55, 76);
            tft.print(messageText);
            delay(600);
          }
          return;
        }
        else if (adminPinEntry.length() < 4)
        {
          adminPinEntry += k;
        }
        drawAdminPin();
      }
    }
    break;
  }

  // ── ADMIN MENU ────────────────────────────────────────────
  case SCR_ADMIN_MENU:
    if (tx > 8 && tx < 232 && ty > 58 && ty < NAV_Y)
    {
      if (ty >= 60 && ty <= 92)
      { // View Log
        drawButton(8, 68, 224, 28, "View Log", C_CYAN, C_BG, true);
        delay(80);
        logPage = 0;
        currentScreen = SCR_VIEW_LOG;
        drawLogScreen(0);
      }
      else if (ty >= 100 && ty <= 132)
      { // Add New Card
        drawButton(8, 100, 224, 28, "Add New Card", C_GREEN, C_BG, true);
        delay(80);
        waitingForNewCard = true;
        currentScreen = SCR_SCAN_NEW;
        drawScanNew();
      }
      else if (ty >= 140 && ty <= 172)
      { // Block Card
        drawButton(8, 132, 224, 28, "Block Card", C_RED, C_WHITE, true);
        delay(80);
        showingMessage = true;
        messageText = "Block via web dashboard.";
        messageTimeout = millis() + 1200;
        drawAdminMenu();
      }
      else if (ty >= 180 && ty <= 212)
      { // Web Register
        drawButton(8, 164, 224, 28, "Web Register", C_LBLUE, C_BG, true);
        delay(80);
        currentScreen = SCR_LOG_WEB;
        drawWebInfo();
      }
      else if (ty >= 220 && ty <= 252)
      { // Open Lock (emergency)
        drawButton(8, 196, 224, 28, "Open Lock", C_YELLOW, C_BG, true);
        delay(80);
        tft.fillRoundRect(8, 196, 224, 28, 6, C_YELLOW);
        tft.setTextColor(C_BG);
        tft.setTextSize(1);
        tft.setCursor(85, 206);
        tft.print("Opening...");
        digitalWrite(RELAY_PIN, LOCK_OPEN);
        messageTimeout = millis() + UNLOCK_MS;
        showingMessage = true;
        messageText = "Door unlocking...";
      }
    }
    break;

  // ── VIEW LOG ──────────────────────────────────────────────
  case SCR_VIEW_LOG:
    // Page controls: tap area just above nav bar (left=Newer, right=Older)
    if (ty >= NAV_Y - 36 && ty < NAV_Y)
    {
      if (tx >= 8 && tx <= 116)
      { // Newer
        if (logPage > 0)
        {
          logPage--;
          drawLogScreen(logPage);
        }
      }
      else if (tx >= 124 && tx <= 232)
      { // Older
        logPage++;
        drawLogScreen(logPage);
      }
    }
    break;

  // ── SCAN NEW CARD — Cancel ────────────────────────────────
  case SCR_SCAN_NEW:
    // Nav bar handles cancel/back
    break;
    break;

  // ── WEB INFO — Back ───────────────────────────────────────
  case SCR_LOG_WEB:
    // Nav bar handles back
    break;
    break;

  // ── IDLE ──────────────────────────────────────────────────
  case SCR_IDLE:
    if (tx >= 70 && tx <= 170 && ty >= 240 && ty <= 270)
    {
      drawButton(70, 240, 100, 30, "PIN LOGIN", C_PURPLE, C_WHITE, true);
      delay(80);
      adminPinEntry = "";
      currentScreen = SCR_ADMIN_PIN;
      drawAdminPin();
      return;
    }
    // CALIBRATION MODE: Tap header 3 times rapidly to enter calibration
    else if (ty < 40 && tx > 80 && tx < 160)
    {
      static unsigned long lastHeaderTap = 0;
      static int headerTaps = 0;
      unsigned long now = millis();

      if (now - lastHeaderTap < 600)
      {
        headerTaps++;
        if (headerTaps >= 3)
        {
          headerTaps = 0;
          // Enter calibration mode
          calibIdx = 0;
          for (int i = 0; i < 4; i++)
            calibPoints[i].captured = false;
          currentScreen = SCR_CALIBRATE;
          drawCalibrationScreen();
          return;
        }
      }
      else
      {
        headerTaps = 1;
      }
      lastHeaderTap = now;
    }
    break;

  default:
    break;
  }
}

// =================================================================
//  WEB SERVER — HTML dashboard + JSON API
// =================================================================
void handleRoot()
{
  String html = R"RAW(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Door Register</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--bd:#30363d;--green:#3fb950;
  --red:#f85149;--blue:#58a6ff;--muted:#8b949e;--text:#e6edf3}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:system-ui,sans-serif}
.hd{background:var(--card);border-bottom:1px solid var(--bd);
    padding:14px 20px;display:flex;align-items:center;gap:10px}
.hd h1{font-size:16px;font-weight:600}
.dot{width:7px;height:7px;border-radius:50%;background:var(--green);animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.25}}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;padding:16px 20px}
.stat{background:var(--card);border:1px solid var(--bd);border-radius:8px;padding:14px}
.sn{font-size:26px;font-weight:700}.sl{font-size:11px;color:var(--muted);margin-top:3px}
.wrap{padding:0 20px 20px}
.bar{display:flex;gap:8px;margin-bottom:10px}
.btn{background:#21262d;border:1px solid var(--bd);color:var(--text);
     border-radius:6px;padding:4px 12px;font-size:12px;cursor:pointer}
.btn.on{border-color:var(--blue);color:var(--blue)}
.src{background:#21262d;border:1px solid var(--bd);color:var(--text);
     border-radius:6px;padding:4px 12px;font-size:12px;margin-left:auto;width:160px;outline:none}
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;padding:7px 10px;border-bottom:1px solid var(--bd);
   color:var(--muted);font-weight:500;font-size:11px;text-transform:uppercase}
td{padding:9px 10px;border-bottom:1px solid #21262d}
tr:hover td{background:#1c2128}
.g{background:#1a3c28;color:var(--green);border-radius:10px;padding:2px 9px;font-size:11px;font-weight:600}
.d{background:#3c1a1a;color:var(--red);border-radius:10px;padding:2px 9px;font-size:11px;font-weight:600}
.role{background:#21262d;color:var(--blue);border-radius:10px;padding:2px 8px;font-size:11px}
.uid{font-family:monospace;color:var(--muted);font-size:11px}
</style></head><body>
<div class="hd"><div class="dot"></div><h1>Smart Door Register</h1>
<span style="margin-left:auto;font-size:11px;color:var(--muted)" id="upd">Live</span></div>
<div class="stats">
<div class="stat"><div class="sn" id="s1">-</div><div class="sl">Total scans</div></div>
<div class="stat"><div class="sn" style="color:var(--green)" id="s2">-</div><div class="sl">Granted</div></div>
<div class="stat"><div class="sn" style="color:var(--red)" id="s3">-</div><div class="sl">Denied</div></div>
</div>
<div class="wrap">
<div class="bar">
<button class="btn on" onclick="filt('all',this)">All</button>
<button class="btn" onclick="filt('granted',this)">Granted</button>
<button class="btn" onclick="filt('denied',this)">Denied</button>
<input class="src" id="q" placeholder="Search name..." oninput="render()">
</div>
<table><thead><tr><th>#</th><th>Name</th><th>UID</th><th>Role</th><th>Status</th></tr></thead>
<tbody id="tb"><tr><td colspan="5" style="text-align:center;padding:20px;color:var(--muted)">Loading...</td></tr></tbody>
</table></div>
<script>
let all=[],cur='all';
async function load(){
  try{
    const r=await fetch('/api/log');
    all=await r.json();
    const g=all.filter(e=>e.granted).length;
    document.getElementById('s1').textContent=all.length;
    document.getElementById('s2').textContent=g;
    document.getElementById('s3').textContent=all.length-g;
    document.getElementById('upd').textContent='Updated '+new Date().toLocaleTimeString();
    render();
  }catch(e){
    document.getElementById('tb').innerHTML='<tr><td colspan="5" style="text-align:center;color:var(--red);padding:20px">Connection error</td></tr>';
  }
}
function filt(f,b){cur=f;document.querySelectorAll('.btn').forEach(x=>x.classList.remove('on'));b.classList.add('on');render();}
function render(){
  const q=document.getElementById('q').value.toLowerCase();
  const d=all.filter(e=>(cur==='all'||(cur==='granted'&&e.granted)||(cur==='denied'&&!e.granted))&&(!q||e.name.toLowerCase().includes(q))).slice().reverse();
  document.getElementById('tb').innerHTML=d.map((e,i)=>`<tr>
    <td style="color:var(--muted)">${d.length-i}</td>
    <td style="font-weight:500">${e.name}</td>
    <td class="uid">${e.uid}</td>
    <td><span class="role">${e.role}</span></td>
    <td><span class="${e.granted?'g':'d'}">${e.granted?'&#10003; Granted':'&#10007; Denied'}</span></td>
  </tr>`).join('')||`<tr><td colspan="5" style="text-align:center;color:var(--muted);padding:20px">No records</td></tr>`;
}
load();setInterval(load,5000);
</script></body></html>)RAW";
  server.send(200, "text/html", html);
}

void handleLogJSON()
{
  String json = "[";
  int start = (logCount < MAX_LOG) ? 0 : logHead;
  for (int i = 0; i < logCount; i++)
  {
    int idx = (start + i) % MAX_LOG;
    if (i)
      json += ",";
    // Escape name in case it contains quotes
    String safeName = logBuf[idx].name;
    safeName.replace("\"", "'");
    json += "{\"uid\":\"" + logBuf[idx].uid + "\","
                                              "\"name\":\"" +
            safeName + "\","
                       "\"role\":\"" +
            logBuf[idx].role + "\","
                               "\"granted\":" +
            (logBuf[idx].granted ? "true" : "false") + ","
                                                       "\"ts\":\"" +
            logBuf[idx].ts + "\"}";
  }
  json += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// =================================================================
//  SETUP
// =================================================================
void setup()
{
  Serial.begin(115200);
  delay(200);

  // Relay — start locked
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOCK_CLOSE);

  // TFT (HSPI)
  hSPI.begin(TFT_SCK, TOUCH_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(0); // Portrait mode
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
  ts.setRotation(0); // Portrait mode

  // RFID (VSPI) — init
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS); // VSPI
  tft.setTextColor(C_YELLOW);
  tft.setCursor(20, 136);
  tft.print("Initializing RFID...");

  bool rfidOK = false;
  for (int i = 0; i < 5; i++)
  {
    rfid.PCD_Init();
    delay(100);
    // Check if PCD version is valid (0x91-0x92 = good)
    byte v = rfid.PCD_ReadRegister(rfid.VersionReg);
    if (v == 0x91 || v == 0x92)
    {
      rfidOK = true;
      Serial.println("RFID version OK: 0x" + String(v, HEX));
      rfid.PCD_DumpVersionToSerial();
      break;
    }
    Serial.println("RFID init retry... (v=" + String(v, HEX) + ")");
  }

  if (!rfidOK)
  {
    Serial.println("WARNING: RFID failed to initialize!");
  }
  else
  {
    Serial.println("RFID ready.");
  }

  // WiFi
  tft.setCursor(20, 156);
  tft.setTextColor(C_YELLOW);
  tft.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 20)
  {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    // Clear old WiFi message and show success
    tft.fillRect(20, 156, 200, 16, C_BG);
    tft.setTextColor(C_GREEN);
    tft.setCursor(20, 156);
    tft.print("WiFi: ");
    tft.print(WiFi.localIP().toString());
  }
  else
  {
    Serial.println("\nWiFi offline — running standalone.");
    // Clear old WiFi message and show offline status
    tft.fillRect(20, 156, 200, 16, C_BG);
    tft.setTextColor(C_ORANGE);
    tft.setCursor(20, 156);
    tft.print("WiFi offline — standalone mode.");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/api/log", handleLogJSON);
  server.begin();

  delay(1200);
  drawIdle();
  currentScreen = SCR_IDLE;
  Serial.println("System ready. Scan card or touch screen.");
}

// =================================================================
//  MAIN LOOP
// =================================================================
void loop()
{
  server.handleClient();

  // ── Handle message timeouts (non-blocking UI feedback) ───────
  if (showingMessage && millis() >= messageTimeout)
  {
    showingMessage = false;
    // Special handling for emergency unlock
    if (messageText == "Door unlocking...")
    {
      digitalWrite(RELAY_PIN, LOCK_CLOSE);
      drawAdminMenu();
    }
    else if (currentScreen == SCR_ADMIN_PIN && messageText.indexOf("Wrong PIN") >= 0)
    {
      drawAdminPin(); // Redraw pin entry
    }
    else if (currentScreen == SCR_ADMIN_MENU)
    {
      drawAdminMenu(); // Redraw menu
    }
  }
  else if (showingMessage && currentScreen == SCR_ADMIN_MENU)
  {
    // Show message bar at bottom
    tft.fillRect(0, 228, 320, 12, C_BG);
    tft.setTextColor(C_CYAN);
    tft.setTextSize(1);
    tft.setCursor(8, 228);
    tft.print(messageText);
  }

  // ── Touch input ─────────────────────────────────────────────
  int tx, ty;
  if (getTouchPoint(tx, ty))
  {
    handleTouch(tx, ty);
  }

  // ── Waiting to register a new card ──────────────────────────
  if (waitingForNewCard && currentScreen == SCR_SCAN_NEW)
  {
    unsigned long now = millis();
    if (now - lastRFIDScan > RFID_SCAN_TIMEOUT && rfid.PICC_IsNewCardPresent())
    {
      lastRFIDScan = now;
      if (rfid.PICC_ReadCardSerial())
      {
        String uid = getUID();
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        if (findCard(uid) >= 0)
        {
          // Already in database - show error with non-blocking timeout
          tft.setTextColor(C_YELLOW);
          tft.setTextSize(1);
          tft.setCursor(20, 200);
          tft.print("Card already registered!");
          showingMessage = true;
          messageTimeout = millis() + 1200;
        }
        else if (cardCount < MAX_CARDS)
        {
          cards[cardCount] = {uid, "New User " + String(cardCount), "Staff", false};
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

          // Store this screen state and show for 2.5 seconds
          showingMessage = true;
          messageTimeout = millis() + 2500;
          lastDisplayUpdate = millis();
        }
        else
        {
          tft.setTextColor(C_RED);
          tft.setTextSize(1);
          tft.setCursor(20, 200);
          tft.print("Card database full! Max 20.");
          showingMessage = true;
          messageTimeout = millis() + 1200;
        }
        return;
      }
    }

    // ── Handle card added screen timeout ────────────────────────
    if (showingMessage && currentScreen == SCR_SCAN_NEW && messageTimeout <= millis())
    {
      currentScreen = SCR_ADMIN_MENU;
      drawAdminMenu();
      showingMessage = false;
    }

    // ── Normal RFID scan (only from idle screen) ─────────────────
    if (currentScreen != SCR_IDLE)
      return;

    unsigned long now2 = millis();
    if (now2 - lastRFIDScan < RFID_SCAN_TIMEOUT)
      return;
    lastRFIDScan = now2;

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
      return;

    String uid = getUID();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    Serial.println("Card scanned: " + uid);

    int idx = findCard(uid);

    if (idx >= 0 && cards[idx].blocked)
    {
      // BLOCKED
      addLog(uid, cards[idx].name, cards[idx].role, false);
      drawBlocked(cards[idx].name);
      lastDisplayUpdate = millis();
      while (millis() - lastDisplayUpdate < MSG_MS)
      {
        server.handleClient();
        delay(10);
      }
      drawIdle();
    }
    else if (idx >= 0)
    {
      // GRANTED
      addLog(uid, cards[idx].name, cards[idx].role, true);
      drawGranted(cards[idx].name, cards[idx].role);
      digitalWrite(RELAY_PIN, LOCK_OPEN);
      // Non-blocking unlock duration
      lastDisplayUpdate = millis();
      while (millis() - lastDisplayUpdate < UNLOCK_MS)
      {
        server.handleClient();
        delay(10);
      }
      digitalWrite(RELAY_PIN, LOCK_CLOSE);
      drawIdle();
    }
    else
    {
      // DENIED — unknown card
      addLog(uid, "Unknown", "N/A", false);
      drawDenied(uid);
      lastDisplayUpdate = millis();
      while (millis() - lastDisplayUpdate < MSG_MS)
      {
        server.handleClient();
        delay(10);
      }
      drawIdle();
    }
  }
}
