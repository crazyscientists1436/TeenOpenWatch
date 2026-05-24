


// insta- crazyscientist1436
// GitHub Repo- https://github.com/crazyscientists1436/TeenOpenWatch.git


// ================================================================
//  DIY Smartwatch v2 — esp32-c3 super mini
// ----------------------------------------------------------------
//  Hardware:
//    Display  : 0.96" OLED SSD1306  (I2C: SDA=D4, SCL=D5)
//    Sensor   : MAX30102             (I2C: shared with OLED)
//    Buttons  : BT1=D0  BT2=D1  BT3=D2  BT4=D3
//    Motor    : D6 via 2N2222 transistor + flyback diode
//    Battery  : ADC on A0 (voltage divider: BAT+ → 100k → A0 → 100k → GND)
//    WiFi     : Built-in (ESP32-C3 super mini)
//
//  Button Logic:
//    BT1 → Increase selected unit         (Logic Z)
//    BT3 → Decrease selected unit         (Logic Z)
//    BT2 → Select / confirm / start-stop  (Logic Z)
//    BT4 (single) → Back to Menu          (Logic X)
//    BT4 (double) → Back to Home/Main     (Logic Y)
//    BT2 (double) → Start/Stop timer & alarm
//
//  Screens:
//    1. Home      — Clock (HH:MM) + Date (DD/MM/YY) + Battery %
//    2. Menu      — List: Heart Monitor / Timer / Stopwatch / Alarm / Torch / QR
//    3. Heart     — BPM + SpO2
//    4. Timer     — HH:MM:SS set & countdown
//    5. Stopwatch — HH:MM:SS
//    6. Alarm     — Set + arm
//    7. Torch     — Full white OLED screen
//    8. QR        — Static QR code bitmap
//    9. NTP Setup — Connect to 1 of 3 saved WiFi hotspots, sync time
//
//  Libraries (install via Arduino Library Manager):
//    • Adafruit SSD1306
//    • Adafruit GFX Library
//    • SparkFun MAX3010x Pulse and Proximity Sensor Library
//    • (NTP uses built-in ESP32 WiFi + time.h — no extra install)
// ================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <WiFi.h>
#include "time.h"

// ── OLED ──────────────────────────────────────────────────────
#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ── MAX30102 ───────────────────────────────────────────────────
MAX30105 particleSensor;
bool sensorFound = false;

// ── PINS ──────────────────────────────────────────────────────
#define BT1       D0    // Up   / Increase
#define BT2       D1    // Select / Confirm / Start-Stop
#define BT3       D2    // Down / Decrease
#define BT4       D3    // Back
#define MOTOR_PIN D6    // Vibration motor via transistor
#define BAT_PIN   A0    // Battery voltage divider

// ── WIFI / NTP ────────────────────────────────────────────────
// Edit these 3 hotspot credentials
const char* wifiSSID[3]  = { "OnePlus Nord CE4",  "Redmi 9 Power",  "dlink-9649"  };
const char* wifiPass[3]  = { "23061975", "1436shr..a", "Vihaan123" };
const char* ntpServer    = "pool.ntp.org";
const long  gmtOffset    = 19800;   // UTC+5:30 for India (seconds)
const int   dstOffset    = 0;
bool ntpSynced = false;

// ── BUTTON TIMING ─────────────────────────────────────────────
#define DEBOUNCE_MS      50
#define LONG_PRESS_MS   600
#define DOUBLE_PRESS_MS 350   // max gap between two presses for double

// ── SCREENS ───────────────────────────────────────────────────
enum Screen {
  SCR_HOME = 0,
  SCR_MENU,
  SCR_HEART,
  SCR_TIMER,
  SCR_STOPWATCH,
  SCR_ALARM,
  SCR_TORCH,
  SCR_QR,
  SCR_NTP
};
Screen currentScreen  = SCR_HOME;
Screen previousScreen = SCR_HOME;   // for BT4 logic

// ── CLOCK & DATE ──────────────────────────────────────────────
struct TimeVal { uint8_t h, m, s; };
struct DateVal { uint8_t d, mo; uint16_t yr; };
TimeVal clk       = {12, 0, 0};
DateVal dat       = {1, 1, 2025};
unsigned long lastClkTick = 0;

// ── MENU ──────────────────────────────────────────────────────
const char* menuItems[] = {
  "1 Heart Monitor",
  "2 Timer",
  "3 Stopwatch",
  "4 Alarm",
  "5 Torch",
  "6 QR Code"
};
const uint8_t MENU_COUNT = 6;
uint8_t menuCursor = 0;

// ── HEART ─────────────────────────────────────────────────────
float heartBPM = 0, spO2 = 0;
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot = 0;
long  lastBeat = 0;

// ── TIMER ─────────────────────────────────────────────────────
TimeVal timerSet     = {0, 0, 30};
TimeVal timerLeft    = {0, 0, 0};
bool    timerRunning = false;
unsigned long timerLastTick = 0;
uint8_t timerSel = 2;   // 0=H 1=M 2=S

// ── STOPWATCH ─────────────────────────────────────────────────
TimeVal swVal     = {0, 0, 0};
bool    swRunning = false;
unsigned long swLastTick = 0;

// ── ALARM ─────────────────────────────────────────────────────
TimeVal alarmSet   = {7, 0, 0};
bool    alarmArmed = false;
uint8_t alarmSel   = 0;

// ── MOTOR ─────────────────────────────────────────────────────
bool          motorOn    = false;
unsigned long motorStart = 0;
#define MOTOR_BUZZ_MS 1200

// ── BATTERY ───────────────────────────────────────────────────
uint8_t battPercent = 100;
unsigned long lastBatRead = 0;
#define BAT_READ_INTERVAL 30000

// ── NTP screen state ──────────────────────────────────────────
uint8_t  ntpStatus = 0;   // 0=idle 1=connecting 2=ok 3=fail
uint8_t  ntpTrying = 0;

// ── BUTTON STATE ──────────────────────────────────────────────
struct BtnState {
  bool  lastRaw;
  bool  pressed;       // single short press
  bool  longFired;
  bool  doubleFired;
  unsigned long downAt;
  unsigned long lastReleaseAt;
  bool  waitingDouble;
};
BtnState btn1 = {true,false,false,false,0,0,false};
BtnState btn2 = {true,false,false,false,0,0,false};
BtnState btn3 = {true,false,false,false,0,0,false};
BtnState btn4 = {true,false,false,false,0,0,false};

// =================================================================
//  BUTTON READING
// =================================================================

void readBtn(BtnState &b, uint8_t pin) {
  b.pressed     = false;
  b.longFired   = false;
  b.doubleFired = false;

  bool raw = digitalRead(pin);

  if (b.lastRaw == HIGH && raw == LOW)   // press start
    b.downAt = millis();

  if (b.lastRaw == LOW && raw == HIGH) { // release
    unsigned long held = millis() - b.downAt;
    if (held >= DEBOUNCE_MS && held < LONG_PRESS_MS) {
      if (b.waitingDouble &&
          millis() - b.lastReleaseAt <= DOUBLE_PRESS_MS) {
        b.doubleFired  = true;
        b.waitingDouble = false;
      } else {
        b.waitingDouble  = true;
        b.lastReleaseAt  = millis();
        // single press confirmed after double-window expires (see below)
      }
    }
  }

  // Confirm single press after double-window passes
  if (b.waitingDouble &&
      millis() - b.lastReleaseAt > DOUBLE_PRESS_MS) {
    b.pressed      = true;
    b.waitingDouble = false;
  }

  // Long press fires once while held
  if (raw == LOW && !b.longFired &&
      millis() - b.downAt >= LONG_PRESS_MS) {
    b.longFired = true;
  }

  b.lastRaw = raw;
}

void readAllButtons() {
  readBtn(btn1, BT1);
  readBtn(btn2, BT2);
  readBtn(btn3, BT3);
  readBtn(btn4, BT4);
}

// =================================================================
//  UTILITY
// =================================================================

void buzz() {
  digitalWrite(MOTOR_PIN, HIGH);
  motorOn = true;
  motorStart = millis();
}

void updateMotor() {
  if (motorOn && millis() - motorStart >= MOTOR_BUZZ_MS) {
    digitalWrite(MOTOR_PIN, LOW);
    motorOn = false;
  }
}

void incField(uint8_t &v, uint8_t mx) { v = (v + 1) % (mx + 1); }
void decField(uint8_t &v, uint8_t mx) { v = (v == 0) ? mx : v - 1; }

void adjustField(uint8_t sel, TimeVal &t, bool up) {
  if (sel == 0) { if (up) incField(t.h, 23); else decField(t.h, 23); }
  if (sel == 1) { if (up) incField(t.m, 59); else decField(t.m, 59); }
  if (sel == 2) { if (up) incField(t.s, 59); else decField(t.s, 59); }
}

bool decrementTimer(TimeVal &t) {
  if (t.h == 0 && t.m == 0 && t.s == 0) return true;
  if (t.s-- == 0) { t.s = 59; if (t.m-- == 0) { t.m = 59; t.h--; } }
  return (t.h == 0 && t.m == 0 && t.s == 0);
}

void incrementSW(TimeVal &t) {
  if (++t.s >= 60) { t.s = 0; if (++t.m >= 60) { t.m = 0; if (++t.h >= 24) t.h = 0; } }
}

void advanceClock() {
  incrementSW(clk);
  if (clk.h == 0 && clk.m == 0 && clk.s == 0) {
    // Advance date
    const uint8_t daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (dat.yr % 4 == 0 && (dat.yr % 100 != 0 || dat.yr % 400 == 0));
    uint8_t maxDay = daysInMonth[dat.mo];
    if (dat.mo == 2 && leap) maxDay = 29;
    dat.d++;
    if (dat.d > maxDay) { dat.d = 1; dat.mo++; }
    if (dat.mo > 12)   { dat.mo = 1; dat.yr++; }
  }
}

// Battery % from ADC (voltage divider: 100k + 100k, 3.7V LiPo max ~4.2V)
// XIAO ADC ref = 3.3V, 12-bit = 4095
// Divider halves voltage: V_adc = V_bat / 2
// V_bat = V_adc * 2; map 3.0V–4.2V → 0–100%
void updateBattery() {
  if (millis() - lastBatRead < BAT_READ_INTERVAL) return;
  lastBatRead = millis();
  int raw = analogRead(BAT_PIN);
  float vAdc = raw * 3.3f / 4095.0f;
  float vBat = vAdc * 2.0f;
  battPercent = (uint8_t)constrain(
    map((long)(vBat * 100), 300, 420, 0, 100), 0, 100);
}

// Draw highlighted field
void drawField(uint8_t val, uint8_t idx, uint8_t selIdx) {
  char buf[3]; sprintf(buf, "%02d", val);
  if (selIdx == idx)
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  else
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.print(buf);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
}

void drawTimeFields(const TimeVal &t, uint8_t sel) {
  drawField(t.h, 0, sel); display.print(":");
  drawField(t.m, 1, sel); display.print(":");
  drawField(t.s, 2, sel);
}

// Battery icon (top-right, 20×10)
void drawBatIcon(uint8_t pct, int16_t x, int16_t y) {
  display.drawRect(x, y, 18, 9, SSD1306_WHITE);
  display.fillRect(x+18, y+3, 2, 3, SSD1306_WHITE); // terminal nub
  uint8_t fill = (uint8_t)(pct * 16 / 100);
  if (fill > 0)
    display.fillRect(x+1, y+1, fill, 7, SSD1306_WHITE);
}

void goTo(Screen s) {
  previousScreen = currentScreen;
  currentScreen  = s;
}

// =================================================================
//  QR CODE  (tiny 21×21 QR — "Hello" as placeholder)
//  Replace this bitmap with your own QR (use online QR bitmap gen)
//  Each row = 21 bits packed into 3 bytes MSB first, zero-padded
//  Scale factor 3 → rendered at 63×63 px
// =================================================================
// QR for text "SMARTWATCH" — Version 1, 21x21 modules
// Generated as raw bit rows (1=dark, 0=light)
const uint8_t qrData[21][3] = {
  {0b11111110,0b00101111,0b11000000},
  {0b10000010,0b11001000,0b01000000},
  {0b10111010,0b01001011,0b01000000},
  {0b10111010,0b10001011,0b01000000},
  {0b10111010,0b11001011,0b01000000},
  {0b10000010,0b01001000,0b01000000},
  {0b11111110,0b10101111,0b11000000},
  {0b00000000,0b11010000,0b00000000},
  {0b11010110,0b00111010,0b11000000},
  {0b01001000,0b11001001,0b00000000},
  {0b11110110,0b01011101,0b10000000},
  {0b00101000,0b10100010,0b10000000},
  {0b11011110,0b11011010,0b00000000},
  {0b00000001,0b01000001,0b10000000},
  {0b11111110,0b10110010,0b10000000},  // padding rows below
  {0b10000010,0b00011101,0b00000000},
  {0b10111010,0b11001010,0b10000000},
  {0b10111010,0b01110001,0b00000000},
  {0b10111010,0b10001011,0b10000000},
  {0b10000010,0b11010100,0b00000000},
  {0b11111110,0b01101111,0b00000000},
};
#define QR_SCALE 3  // each module = 3×3 px → 63px total

void drawQR(int16_t ox, int16_t oy) {
  for (uint8_t row = 0; row < 21; row++) {
    for (uint8_t col = 0; col < 21; col++) {
      uint8_t byteIdx = col / 8;
      uint8_t bitIdx  = 7 - (col % 8);
      bool dark = (qrData[row][byteIdx] >> bitIdx) & 1;
      if (dark)
        display.fillRect(ox + col * QR_SCALE, oy + row * QR_SCALE,
                         QR_SCALE, QR_SCALE, SSD1306_WHITE);
    }
  }
}

// =================================================================
//  SCREEN DRAWS
// =================================================================

void drawHome() {
  display.clearDisplay();

  // Battery icon top-right
  drawBatIcon(battPercent, 106, 0);
  // Battery % text
  char batBuf[5]; sprintf(batBuf, "%3d%%", battPercent);
  display.setTextSize(1); display.setCursor(82, 1); display.print(batBuf);

  // NTP synced dot
  if (ntpSynced) { display.fillCircle(4, 4, 3, SSD1306_WHITE); }

  // Big clock HH:MM
  display.setTextSize(3);
  char tbuf[6]; sprintf(tbuf, "%02d:%02d", clk.h, clk.m);
  display.setCursor(14, 16);
  display.print(tbuf);

  // Seconds small
  display.setTextSize(1);
  char sbuf[3]; sprintf(sbuf, "%02d", clk.s);
  display.setCursor(110, 28);
  display.print(sbuf);

  // Date
  display.setTextSize(1);
  char dbuf[12]; sprintf(dbuf, "%02d/%02d/%04d", dat.d, dat.mo, dat.yr);
  display.setCursor(24, 52);
  display.print(dbuf);

  display.display();
}

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(40, 0);
  display.print("-- MENU --");

  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    int16_t y = 10 + i * 9;
    if (y > 63) break;
    if (i == menuCursor) {
      display.fillRect(0, y, SCREEN_W, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y + 1);
    display.print(menuItems[i]);
  }
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.display();
}

void drawHeart() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(22, 0); display.print("HEART MONITOR");

  display.setTextSize(2); display.setCursor(4, 14);
  if (heartBPM > 0) {
    char buf[12]; sprintf(buf, "%3.0f BPM", heartBPM); display.print(buf);
  } else { display.print("--- BPM"); }

  display.setTextSize(1); display.setCursor(4, 40);
  display.print("SpO2: ");
  if (spO2 > 0) { char b2[8]; sprintf(b2, "%.0f%%", spO2); display.print(b2); }
  else display.print("---%");

  if (!sensorFound) { display.setCursor(4,52); display.print("[sensor not found]"); }
  else { display.setCursor(0,54); display.print("Place finger on sensor"); }
  display.display();
}

void drawTimer() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(36,0); display.print("-- TIMER --");

  display.setTextSize(2); display.setCursor(4, 14);
  drawTimeFields(timerSet, timerRunning ? 99 : timerSel);

  display.setTextSize(1); display.setCursor(4, 38);
  display.print("Left: ");
  char buf[9]; sprintf(buf, "%02d:%02d:%02d", timerLeft.h, timerLeft.m, timerLeft.s);
  display.print(buf);

  display.setCursor(0, 54);
  if (!timerRunning) display.print("1/3:adj 2:sel 2x2:start");
  else               display.print("2x2:stop  BT4:back");
  display.display();
}

void drawStopwatch() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(18,0); display.print("-- STOPWATCH --");

  display.setTextSize(2);
  char buf[9]; sprintf(buf, "%02d:%02d:%02d", swVal.h, swVal.m, swVal.s);
  display.setCursor(4, 22); display.print(buf);

  display.setTextSize(1); display.setCursor(0, 54);
  display.print("BT2:go/stop  BT3:reset");
  display.display();
}

void drawAlarm() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(34,0); display.print("-- ALARM --");

  display.setTextSize(2); display.setCursor(4, 14);
  drawTimeFields(alarmSet, alarmSel);

  display.setTextSize(1); display.setCursor(4,40);
  display.print("Armed: "); display.print(alarmArmed ? "YES" : " NO");

  display.setCursor(0, 54);
  display.print("1/3:adj 2:sel 2x2:arm");
  display.display();
}

void drawTorch() {
  // Full white screen = flashlight
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
  display.display();
}

void drawQRScreen() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(32, 0); display.print("-- QR CODE --");
  drawQR(32, 1);   // center 63px QR in 128px width
  display.display();
}

void drawNTP() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(24,0); display.print("-- NTP SYNC --");

  const char* statusMsg[] = {
    "Press BT2 to sync",
    "Connecting WiFi...",
    "Synced OK!",
    "Failed - check WiFi"
  };
  display.setCursor(0, 24); display.print(statusMsg[ntpStatus]);

  if (ntpStatus == 2) {
    char tbuf[9]; sprintf(tbuf, "%02d:%02d:%02d", clk.h, clk.m, clk.s);
    display.setCursor(24, 40); display.print(tbuf);
  }
  display.setCursor(0, 54); display.print("BT2:sync  BT4:back");
  display.display();
}

// =================================================================
//  INPUT HANDLERS
// =================================================================

void handleHome() {
  if (btn2.pressed)  goTo(SCR_MENU);
  if (btn4.pressed)  goTo(SCR_NTP);   // shortcut to NTP from home via BT4
}

void handleMenu() {
  if (btn1.pressed)  menuCursor = (menuCursor + MENU_COUNT - 1) % MENU_COUNT;
  if (btn3.pressed)  menuCursor = (menuCursor + 1) % MENU_COUNT;
  if (btn2.pressed) {
    const Screen targets[] = {
      SCR_HEART, SCR_TIMER, SCR_STOPWATCH, SCR_ALARM, SCR_TORCH, SCR_QR
    };
    goTo(targets[menuCursor]);
  }
  if (btn4.pressed)  goTo(SCR_HOME);
  if (btn4.doubleFired) goTo(SCR_HOME);
}

void handleHeart() {
  if (btn4.pressed)    goTo(SCR_MENU);
  if (btn4.doubleFired) goTo(SCR_HOME);
}

void handleTimer() {
  if (!timerRunning) {
    if (btn1.pressed)    adjustField(timerSel, timerSet, true);
    if (btn3.pressed)    adjustField(timerSel, timerSet, false);
    if (btn2.pressed)    timerSel = (timerSel + 1) % 3;
    if (btn2.doubleFired) {
      timerLeft    = timerSet;
      timerRunning = true;
      timerLastTick = millis();
    }
  } else {
    if (btn2.doubleFired) timerRunning = false;
  }
  if (btn4.pressed)    { timerRunning = false; goTo(SCR_MENU); }
  if (btn4.doubleFired) { timerRunning = false; goTo(SCR_HOME); }
}

void handleStopwatch() {
  if (btn2.pressed)  {
    swRunning = !swRunning;
    if (swRunning) swLastTick = millis();
  }
  if (btn3.pressed && !swRunning) swVal = {0,0,0};
  if (btn4.pressed)    { swRunning = false; goTo(SCR_MENU); }
  if (btn4.doubleFired) { swRunning = false; goTo(SCR_HOME); }
}

void handleAlarm() {
  if (btn1.pressed)    adjustField(alarmSel, alarmSet, true);
  if (btn3.pressed)    adjustField(alarmSel, alarmSet, false);
  if (btn2.pressed)    alarmSel = (alarmSel + 1) % 3;
  if (btn2.doubleFired) alarmArmed = !alarmArmed;
  if (btn4.pressed)    goTo(SCR_MENU);
  if (btn4.doubleFired) goTo(SCR_HOME);
}

void handleTorch() {
  // BT2 or BT4 exits torch
  if (btn2.pressed || btn4.pressed) goTo(SCR_MENU);
  if (btn4.doubleFired) goTo(SCR_HOME);
}

void handleQR() {
  if (btn4.pressed)    goTo(SCR_MENU);
  if (btn4.doubleFired) goTo(SCR_HOME);
}

void handleNTP() {
  if (btn4.pressed || btn4.doubleFired) { ntpStatus = 0; goTo(SCR_HOME); }
  if (btn2.pressed) {
    ntpStatus = 1;
    drawNTP();  // show "Connecting..." immediately

    bool connected = false;
    for (uint8_t i = 0; i < 3 && !connected; i++) {
      WiFi.begin(wifiSSID[i], wifiPass[i]);
      uint8_t tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500); tries++;
      }
      if (WiFi.status() == WL_CONNECTED) connected = true;
    }

    if (connected) {
      configTime(gmtOffset, dstOffset, ntpServer);
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 5000)) {
        clk.h  = timeinfo.tm_hour;
        clk.m  = timeinfo.tm_min;
        clk.s  = timeinfo.tm_sec;
        dat.d  = timeinfo.tm_mday;
        dat.mo = timeinfo.tm_mon + 1;
        dat.yr = timeinfo.tm_year + 1900;
        lastClkTick = millis();
        ntpSynced   = true;
        ntpStatus   = 2;
      } else {
        ntpStatus = 3;
      }
      WiFi.disconnect(true);
    } else {
      ntpStatus = 3;
    }
  }
}

// =================================================================
//  BACKGROUND TASKS
// =================================================================

void updateClock() {
  if (millis() - lastClkTick >= 1000) {
    lastClkTick = millis();
    advanceClock();
  }
}

void updateTimer() {
  if (!timerRunning) return;
  if (millis() - timerLastTick >= 1000) {
    timerLastTick = millis();
    if (decrementTimer(timerLeft)) {
      timerRunning = false;
      buzz();
    }
  }
}

void updateStopwatch() {
  if (!swRunning) return;
  if (millis() - swLastTick >= 1000) {
    swLastTick = millis();
    incrementSW(swVal);
  }
}

void updateAlarm() {
  if (!alarmArmed) return;
  if (clk.h == alarmSet.h && clk.m == alarmSet.m && clk.s == alarmSet.s) {
    buzz(); alarmArmed = false;
  }
}

void updateHeartRate() {
  if (!sensorFound) return;
  long irV  = particleSensor.getIR();
  long redV = particleSensor.getRed();
  if (checkForBeat(irV)) {
    long delta = millis() - lastBeat; lastBeat = millis();
    float bpm = 60.0f / (delta / 1000.0f);
    if (bpm > 20 && bpm < 255) {
      rates[rateSpot++] = (byte)bpm; rateSpot %= RATE_SIZE;
      float avg = 0; for (byte i = 0; i < RATE_SIZE; i++) avg += rates[i];
      heartBPM = avg / RATE_SIZE;
    }
  }
  if (irV > 50000 && redV > 50000) {
    spO2 = constrain(110.0f - 25.0f * ((float)redV / irV), 90.0f, 100.0f);
  } else { spO2 = 0; heartBPM = 0; }
}

// =================================================================
//  SETUP
// =================================================================

void setup() {
  Serial.begin(115200);

  pinMode(BT1, INPUT_PULLUP);
  pinMode(BT2, INPUT_PULLUP);
  pinMode(BT3, INPUT_PULLUP);
  pinMode(BT4, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  Wire.begin(D4, D5);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed")); for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(14, 20); display.print("Smartwatch v2.0");
  display.setCursor(28, 34); display.print("Initialising...");
  display.display();
  delay(1000);

  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    sensorFound = true;
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeGreen(0);
    Serial.println(F("MAX30102 OK"));
  } else {
    Serial.println(F("MAX30102 not found"));
  }

  lastClkTick  = millis();
  lastBatRead  = millis() - BAT_READ_INTERVAL; // force first read
}

// =================================================================
//  MAIN LOOP
// =================================================================

void loop() {
  readAllButtons();

  // Always-on background tasks
  updateClock();
  updateTimer();
  updateStopwatch();
  updateAlarm();
  updateMotor();
  updateBattery();
  if (currentScreen == SCR_HEART) updateHeartRate();

  // Screen dispatch
  switch (currentScreen) {
    case SCR_HOME:      handleHome();      drawHome();      break;
    case SCR_MENU:      handleMenu();      drawMenu();      break;
    case SCR_HEART:     handleHeart();     drawHeart();     break;
    case SCR_TIMER:     handleTimer();     drawTimer();     break;
    case SCR_STOPWATCH: handleStopwatch(); drawStopwatch(); break;
    case SCR_ALARM:     handleAlarm();     drawAlarm();     break;
    case SCR_TORCH:     handleTorch();     drawTorch();     break;
    case SCR_QR:        handleQR();        drawQRScreen();  break;
    case SCR_NTP:       handleNTP();       drawNTP();       break;
    default: break;
  }

  delay(20);
}
