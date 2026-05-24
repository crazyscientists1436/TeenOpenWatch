


// insta- crazyscientist1436
// GitHub Repo- https://github.com/crazyscientists1436/TeenOpenWatch.git


// ============================================================
//  DIY Smartwatch — Seeed Studio XIAO ESP32-C3
//  Display : 0.96" OLED SSD1306 (I2C)
//  Sensor  : MAX30102 (Heart Rate + SpO2)
//  Buttons : D0 (Up), D1 (Select), D2 (Down)
//  Motor   : D3 via transistor (2N2222 + flyback diode)
//
//  Libraries (install via Arduino Library Manager):
//    - Adafruit SSD1306
//    - Adafruit GFX Library
//    - SparkFun MAX3010x Pulse and Proximity Sensor Library
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"

// ── OLED ──────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H    64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ── MAX30102 ───────────────────────────────────────────────
MAX30105 particleSensor;
bool sensorFound = false;

// ── PINS ──────────────────────────────────────────────────
#define BTN_UP   D0
#define BTN_SEL  D1
#define BTN_DN   D2
#define MOTOR_PIN D3

// ── BUTTON TIMING ─────────────────────────────────────────
#define DEBOUNCE_MS   50
#define LONG_PRESS_MS 600

// ── SCREENS ───────────────────────────────────────────────
enum Screen {
  SCR_HOME = 0,
  SCR_HEART,
  SCR_TIMER,
  SCR_STOPWATCH,
  SCR_ALARM,
  SCR_SET_TIME,
  SCR_COUNT
};
Screen currentScreen = SCR_HOME;
// Screens accessible via UP/DN cycling (excludes SCR_SET_TIME)
const Screen menuOrder[] = {
  SCR_HOME, SCR_HEART, SCR_TIMER, SCR_STOPWATCH, SCR_ALARM
};
const uint8_t MENU_LEN = 5;
uint8_t menuIdx = 0;

// ── CLOCK ─────────────────────────────────────────────────
struct TimeVal { uint8_t h, m, s; };
TimeVal clk        = {12, 0, 0};
unsigned long lastClkTick = 0;

// ── HEART RATE ────────────────────────────────────────────
float heartBPM = 0;
float spO2     = 0;
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot = 0;
long  lastBeat = 0;

// ── TIMER ─────────────────────────────────────────────────
TimeVal timerSet     = {0, 0, 10};
TimeVal timerLeft    = {0, 0, 0};
bool    timerRunning = false;
unsigned long timerLastTick = 0;
uint8_t timerSel = 0;   // 0=H 1=M 2=S

// ── STOPWATCH ─────────────────────────────────────────────
TimeVal swVal     = {0, 0, 0};
bool    swRunning = false;
unsigned long swLastTick = 0;

// ── ALARM ─────────────────────────────────────────────────
TimeVal alarmSet    = {0, 0, 0};
bool    alarmArmed  = false;
uint8_t alarmSel    = 0;

// ── SET TIME ──────────────────────────────────────────────
TimeVal setTimeVal  = {12, 0, 0};
uint8_t setTimeSel  = 0;

// ── MOTOR ─────────────────────────────────────────────────
bool          motorOn    = false;
unsigned long motorStart = 0;
#define MOTOR_BUZZ_MS 1000

// ── BUTTON STATE ──────────────────────────────────────────
struct BtnState {
  bool     lastRaw;
  bool     pressed;
  bool     longFired;
  unsigned long downAt;
};
BtnState btnUp  = {true, false, false, 0};
BtnState btnSel = {true, false, false, 0};
BtnState btnDn  = {true, false, false, 0};

// =============================================================
//  UTILITY
// =============================================================

void buzz() {
  digitalWrite(MOTOR_PIN, HIGH);
  motorOn    = true;
  motorStart = millis();
}

void updateMotor() {
  if (motorOn && millis() - motorStart >= MOTOR_BUZZ_MS) {
    digitalWrite(MOTOR_PIN, LOW);
    motorOn = false;
  }
}

void readBtn(BtnState &b, uint8_t pin) {
  b.pressed   = false;
  b.longFired = false;
  bool raw = digitalRead(pin);   // HIGH = released (INPUT_PULLUP)

  if (b.lastRaw == HIGH && raw == LOW)   // falling edge
    b.downAt = millis();

  if (b.lastRaw == LOW && raw == HIGH) { // rising edge
    unsigned long held = millis() - b.downAt;
    if (held >= DEBOUNCE_MS && held < LONG_PRESS_MS)
      b.pressed = true;
  }

  if (raw == LOW && !b.longFired &&
      millis() - b.downAt >= LONG_PRESS_MS)
    b.longFired = true;

  b.lastRaw = raw;
}

void readAllButtons() {
  readBtn(btnUp,  BTN_UP);
  readBtn(btnSel, BTN_SEL);
  readBtn(btnDn,  BTN_DN);
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

// Draw 2-digit field; highlighted if sel matches
void drawField(uint8_t val, uint8_t idx, uint8_t selIdx, bool editing) {
  char buf[3];
  sprintf(buf, "%02d", val);
  if (editing && selIdx == idx)
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  else
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.print(buf);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
}

void drawTimeFields(const TimeVal &t, uint8_t sel, bool editing) {
  drawField(t.h, 0, sel, editing);
  display.print(":");
  drawField(t.m, 1, sel, editing);
  display.print(":");
  drawField(t.s, 2, sel, editing);
}

// =============================================================
//  SCREEN DRAWS
// =============================================================

void drawHome() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(40, 0);
  display.print("-- CLOCK --");

  display.setTextSize(2);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", clk.h, clk.m, clk.s);
  int16_t x = (SCREEN_W - strlen(buf) * 12) / 2;
  display.setCursor(x, 22);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("UP/DN:screen  SEL:set");
  display.display();
}

void drawHeart() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(24, 0);
  display.print("HEART MONITOR");

  display.setTextSize(2);
  display.setCursor(8, 14);
  if (heartBPM > 0) {
    char buf[10];
    sprintf(buf, "%3.0f  BPM", heartBPM);
    display.print(buf);
  } else {
    display.print("--- BPM");
  }

  display.setTextSize(1);
  display.setCursor(8, 38);
  display.print("SpO2: ");
  if (spO2 > 0) {
    char buf2[8];
    sprintf(buf2, "%.0f%%", spO2);
    display.print(buf2);
  } else {
    display.print("---%");
  }

  if (!sensorFound) {
    display.setCursor(8, 50);
    display.print("[sensor not found]");
  } else {
    display.setCursor(4, 54);
    display.print("Place finger on sensor");
  }
  display.display();
}

void drawTimer() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(40, 0);
  display.print("-- TIMER --");

  // Set row
  display.setTextSize(2);
  display.setCursor(4, 14);
  drawTimeFields(timerSet, timerSel, !timerRunning);

  // Countdown row
  display.setTextSize(1);
  display.setCursor(4, 38);
  display.print("Left: ");
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", timerLeft.h, timerLeft.m, timerLeft.s);
  display.print(buf);

  display.setCursor(0, 54);
  if (!timerRunning)
    display.print("SEL:sel  LSEL:start  LDN:bk");
  else
    display.print("LSEL:stop  LDN:cancel");
  display.display();
}

void drawStopwatch() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(22, 0);
  display.print("-- STOPWATCH --");

  display.setTextSize(2);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", swVal.h, swVal.m, swVal.s);
  int16_t x = (SCREEN_W - strlen(buf) * 12) / 2;
  display.setCursor(x, 22);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("SEL:go/stop  DN:reset  UP:bk");
  display.display();
}

void drawAlarm() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(36, 0);
  display.print("-- ALARM --");

  display.setTextSize(2);
  display.setCursor(4, 14);
  drawTimeFields(alarmSet, alarmSel, true);

  display.setTextSize(1);
  display.setCursor(4, 40);
  display.print("Armed: ");
  display.print(alarmArmed ? "YES" : " NO");

  display.setCursor(0, 54);
  display.print("SEL:sel  LSEL:arm  LDN:back");
  display.display();
}

void drawSetTime() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(22, 0);
  display.print("-- SET CLOCK --");

  display.setTextSize(2);
  display.setCursor(4, 18);
  drawTimeFields(setTimeVal, setTimeSel, true);

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("UP/DN:adj  SEL:next  LSEL:ok");
  display.display();
}

// =============================================================
//  INPUT HANDLERS
// =============================================================

void handleHome() {
  if (btnUp.pressed) {
    menuIdx = (menuIdx + MENU_LEN - 1) % MENU_LEN;
    currentScreen = menuOrder[menuIdx];
  }
  if (btnDn.pressed) {
    menuIdx = (menuIdx + 1) % MENU_LEN;
    currentScreen = menuOrder[menuIdx];
  }
  if (btnSel.pressed) {
    setTimeVal = clk;
    setTimeSel = 0;
    currentScreen = SCR_SET_TIME;
  }
}

void handleSetTime() {
  if (btnUp.pressed)     adjustField(setTimeSel, setTimeVal, true);
  if (btnDn.pressed)     adjustField(setTimeSel, setTimeVal, false);
  if (btnSel.pressed)    setTimeSel = (setTimeSel + 1) % 3;
  if (btnSel.longFired)  { clk = setTimeVal; lastClkTick = millis(); currentScreen = SCR_HOME; menuIdx = 0; }
  if (btnDn.longFired)   { currentScreen = SCR_HOME; menuIdx = 0; }
}

void handleHeart() {
  if (btnUp.pressed || btnDn.pressed) {
    menuIdx = 0;
    currentScreen = SCR_HOME;
  }
}

void handleTimer() {
  if (!timerRunning) {
    if (btnUp.pressed)    adjustField(timerSel, timerSet, true);
    if (btnDn.pressed)    adjustField(timerSel, timerSet, false);
    if (btnSel.pressed)   timerSel = (timerSel + 1) % 3;
    if (btnSel.longFired) {
      timerLeft    = timerSet;
      timerRunning = true;
      timerLastTick = millis();
    }
    if (btnDn.longFired)  { currentScreen = SCR_HOME; menuIdx = 0; }
  } else {
    if (btnSel.longFired) timerRunning = false;
    if (btnDn.longFired)  { timerRunning = false; timerLeft = {0,0,0}; currentScreen = SCR_HOME; menuIdx = 0; }
  }
}

void handleStopwatch() {
  if (btnSel.pressed) {
    swRunning = !swRunning;
    if (swRunning) swLastTick = millis();
  }
  if (btnDn.pressed && !swRunning) swVal = {0, 0, 0};
  if (btnUp.pressed)  { menuIdx = 0; currentScreen = SCR_HOME; }
}

void handleAlarm() {
  if (btnUp.pressed)    adjustField(alarmSel, alarmSet, true);
  if (btnDn.pressed)    adjustField(alarmSel, alarmSet, false);
  if (btnSel.pressed)   alarmSel = (alarmSel + 1) % 3;
  if (btnSel.longFired) alarmArmed = !alarmArmed;
  if (btnDn.longFired)  { currentScreen = SCR_HOME; menuIdx = 0; }
}

// =============================================================
//  BACKGROUND UPDATES
// =============================================================

void updateClock() {
  if (millis() - lastClkTick >= 1000) {
    lastClkTick = millis();
    incrementSW(clk);
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
    buzz();
    alarmArmed = false;  // one-shot
  }
}

void updateHeartRate() {
  if (!sensorFound) return;
  long irValue  = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    float bpm = 60.0f / (delta / 1000.0f);
    if (bpm > 20 && bpm < 255) {
      rates[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_SIZE;
      float avg = 0;
      for (byte i = 0; i < RATE_SIZE; i++) avg += rates[i];
      heartBPM = avg / RATE_SIZE;
    }
  }

  if (irValue > 50000 && redValue > 50000) {
    float ratio = (float)redValue / (float)irValue;
    spO2 = constrain(110.0f - 25.0f * ratio, 90.0f, 100.0f);
  } else {
    spO2     = 0;
    heartBPM = 0;
  }
}

// =============================================================
//  SETUP
// =============================================================

void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_SEL,   INPUT_PULLUP);
  pinMode(BTN_DN,    INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  Wire.begin(D4, D5);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init failed"));
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(18, 24);
  display.print("Smartwatch v1.0");
  display.display();
  delay(1000);

  // MAX30102 init
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    sensorFound = true;
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeGreen(0);
    Serial.println(F("MAX30102 OK"));
  } else {
    Serial.println(F("MAX30102 not found — heart screen will show ---"));
  }

  lastClkTick = millis();
}

// =============================================================
//  MAIN LOOP
// =============================================================

void loop() {
  readAllButtons();

  // Always running background tasks
  updateClock();
  updateTimer();
  updateStopwatch();
  updateAlarm();
  updateMotor();
  if (currentScreen == SCR_HEART) updateHeartRate();

  // Screen dispatch
  switch (currentScreen) {
    case SCR_HOME:       handleHome();      drawHome();      break;
    case SCR_HEART:      handleHeart();     drawHeart();     break;
    case SCR_TIMER:      handleTimer();     drawTimer();     break;
    case SCR_STOPWATCH:  handleStopwatch(); drawStopwatch(); break;
    case SCR_ALARM:      handleAlarm();     drawAlarm();     break;
    case SCR_SET_TIME:   handleSetTime();   drawSetTime();   break;
    default: break;
  }

  delay(20);  // ~50 fps; keeps button debounce stable
}
