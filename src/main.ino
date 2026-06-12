#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <HX711.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <limits.h>

// Pins
#define LED_PIN       8
#define NUM_LEDS      1
#define BUZZER_PIN    9
#define HX711_DT_PIN  11
#define HX711_SCK_PIN 10
#define BUTTON1_PIN   2

// OLED — SH1106 1.3" 128×64
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_RESET  -1
#define OLED_SDA    6
#define OLED_SCL    7
#define OLED_UPDATE_MS 100

// BLE (Nordic UART Service)
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Calibration
#define CALIB_WEIGHT_KG   10.0f
#define CALIB_SAMPLES     10
#define CALIB_PROMPT_MS   5000   // boot window to press BTN1
#define CALIB_COUNTDOWN_S 30     // seconds to place known weight
#define DEBOUNCE_MS       50

Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SH1106G  display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HX711 scale;
Preferences prefs;
NimBLECharacteristic* pTxChar = nullptr;
bool deviceConnected = false;
float calOffset = 0.0f;
float calScale  = 1.0f;

// History ring-buffer for the normal-operation graph
float    history[OLED_WIDTH] = {};
uint16_t historyIndex = 0;
bool     historyFull  = false;
unsigned long lastDisplayUpdate = 0;

// ─── BLE callbacks ───────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] disconnected — re-advertising");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.empty()) return;
    String reply = "Echo: " + String(val.c_str());
    pTxChar->setValue(reply.c_str());
    pTxChar->notify();
  }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────
bool buttonPressed() {
  if (digitalRead(BUTTON1_PIN) == LOW) {
    delay(DEBOUNCE_MS);
    return digitalRead(BUTTON1_PIN) == LOW;
  }
  return false;
}

void waitButtonRelease() {
  while (digitalRead(BUTTON1_PIN) == LOW) delay(10);
  delay(DEBOUNCE_MS);
}

void beepCount(int n) {
  for (int i = 0; i < n; i++) {
    tone(BUZZER_PIN, 2000, 120);
    delay(240);
  }
}

float readRawAverage(int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    while (!scale.is_ready()) delay(10);
    sum += scale.read();
  }
  return (float)sum / samples;
}

// ─── Calibration storage ─────────────────────────────────────────────────────
bool loadCalibration() {
  prefs.begin("hx711", true);
  bool ok = prefs.isKey("offset") && prefs.isKey("scale");
  if (ok) {
    calOffset = prefs.getFloat("offset", 0.0f);
    calScale  = prefs.getFloat("scale",  1.0f);
  }
  prefs.end();
  return ok;
}

void saveCalibration() {
  prefs.begin("hx711", false);
  prefs.putFloat("offset", calOffset);
  prefs.putFloat("scale",  calScale);
  prefs.end();
}

// ─── OLED screens ────────────────────────────────────────────────────────────
void drawHRule(int y) {
  display.drawFastHLine(0, y, OLED_WIDTH, SH110X_WHITE);
}

// Centred title + horizontal rule at y=9
void drawTitleBar(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 0);
  display.print(title);
  drawHRule(9);
}

// Boot-time calibration prompt with live countdown
void showCalibPrompt(int secsLeft) {
  display.clearDisplay();
  drawTitleBar("CALIBRATE?");
  display.setTextSize(1);
  display.setCursor(4, 14);
  display.println("Press BTN1 to");
  display.println(" calibrate the");
  display.println(" load cell.");
  display.println();
  display.print(" Skip in ");
  display.print(secsLeft);
  display.print("s...");
  display.display();
}

// Generic two-line info screen
void showInfo(const char* title, const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
  display.clearDisplay();
  drawTitleBar(title);
  display.setTextSize(1);
  int y = 14;
  if (line1) { display.setCursor(4, y); display.println(line1); y += 10; }
  if (line2) { display.setCursor(4, y); display.println(line2); y += 10; }
  if (line3) { display.setCursor(4, y); display.println(line3); }
  display.display();
}

// 30-second countdown screen
void showCountdown(int secsLeft) {
  display.clearDisplay();
  drawTitleBar("PLACE WEIGHT");

  // Large centred countdown digit(s)
  display.setTextSize(3);
  char buf[8];
  snprintf(buf, sizeof(buf), "%ds", secsLeft);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 18);
  display.print(buf);

  // Hint and progress bar
  display.setTextSize(1);
  display.setCursor(4, 47);
  display.print("BTN1 = confirm now");
  int barW = map(secsLeft, 0, CALIB_COUNTDOWN_S, 0, OLED_WIDTH - 2);
  display.drawRect(0, 57, OLED_WIDTH, 7, SH110X_WHITE);
  display.fillRect(1, 58, barW, 5, SH110X_WHITE);

  display.display();
}

// Result screen after calibration
void showCalibResult(bool ok) {
  display.clearDisplay();
  drawTitleBar(ok ? "  CALIBRATED!" : " CALIB FAILED");
  display.setTextSize(1);
  if (ok) {
    display.setCursor(4, 16);
    display.println("Calibration saved.");
    display.setCursor(4, 28);
    display.print("10kg factor:");
    display.setCursor(4, 38);
    display.print(calScale, 5);
  } else {
    display.setCursor(4, 16);
    display.println("Bad reading!");
    display.setCursor(4, 28);
    display.println("Check weight is on");
    display.setCursor(4, 38);
    display.println("scale and retry.");
  }
  display.display();
  beepCount(ok ? 3 : 1);
  delay(3000);
}

// ─── Calibration flow ────────────────────────────────────────────────────────
void runCalibration() {
  Serial.println("[CAL] start");

  // Step 1 — zero offset (scale must be empty)
  showInfo("CALIBRATING", "Reading zero...", "Keep scale empty!", nullptr);
  beepCount(1);
  delay(1500);
  calOffset = readRawAverage(CALIB_SAMPLES);
  Serial.print("[CAL] offset="); Serial.println(calOffset, 2);

  // Step 2 — countdown for user to place known weight
  showInfo("PLACE WEIGHT", "Put 10 kg on scale.", "30s countdown starts.", "BTN1 = confirm early");
  beepCount(2);
  delay(2000);
  waitButtonRelease(); // clear any residual press from the trigger

  unsigned long countStart  = millis();
  const unsigned long total = (unsigned long)CALIB_COUNTDOWN_S * 1000UL;

  while (millis() - countStart < total) {
    int secsLeft = CALIB_COUNTDOWN_S - (int)((millis() - countStart) / 1000UL);
    showCountdown(max(secsLeft, 0));
    if (buttonPressed()) {
      waitButtonRelease();
      Serial.println("[CAL] countdown skipped");
      break;
    }
    delay(50);
  }

  // Step 3 — read weight and compute scale factor
  showInfo("CALIBRATING", "Reading weight...", nullptr, nullptr);
  float rawWeight = readRawAverage(CALIB_SAMPLES);
  Serial.print("[CAL] raw_weight="); Serial.println(rawWeight, 2);

  float delta = rawWeight - calOffset;
  if (delta <= 0.0f) {
    Serial.println("[CAL] failed: delta <= 0");
    calScale = 1.0f;
    showCalibResult(false);
    return;
  }

  calScale = CALIB_WEIGHT_KG / delta;
  saveCalibration();
  Serial.print("[CAL] scale="); Serial.println(calScale, 6);
  showCalibResult(true);
}

// ─── Normal-operation display ────────────────────────────────────────────────
void pushHistory(float v) {
  history[historyIndex] = v;
  historyIndex = (historyIndex + 1) % OLED_WIDTH;
  if (historyIndex == 0) historyFull = true;
}

float getHistoryAt(int i) {
  if (!historyFull) return history[i];
  return history[(historyIndex + i) % OLED_WIDTH];
}

void updateDisplay(float weightKg) {
  if (millis() - lastDisplayUpdate < OLED_UPDATE_MS) return;
  lastDisplayUpdate = millis();
  pushHistory(weightKg);

  int count = historyFull ? OLED_WIDTH : (int)historyIndex;
  float minV = weightKg, maxV = weightKg;
  for (int i = 0; i < count; i++) {
    float v = getHistoryAt(i);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  if (maxV - minV < 0.05f) maxV = minV + 0.05f;

  display.clearDisplay();
  display.setTextSize(3);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(weightKg, 2);
  display.print("kg");

  const int graphTop = 26, graphBottom = OLED_HEIGHT - 1;
  const int graphH   = graphBottom - graphTop;
  for (int i = 1; i < count; i++) {
    float v0 = getHistoryAt(i - 1), v1 = getHistoryAt(i);
    int y0 = graphBottom - (int)(((v0 - minV) / (maxV - minV)) * graphH);
    int y1 = graphBottom - (int)(((v1 - minV) / (maxV - minV)) * graphH);
    display.drawLine(i - 1, y0, i, y1, SH110X_WHITE);
  }
  display.display();
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  led.begin();
  led.setBrightness(50);
  led.setPixelColor(0, led.Color(0, 0, 0));
  led.show();

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000);
  Serial.println("[BOOT] ESP32-C6 Load Cell + BLE");

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("[BOOT] OLED init failed");
  }
  display.clearDisplay();
  display.display();

  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  Serial.println("[BOOT] HX711 ready");

  bool hasCal = loadCalibration();

  // ── Calibration prompt window ──
  unsigned long promptStart = millis();
  bool doCalib = false;
  while (millis() - promptStart < CALIB_PROMPT_MS) {
    int secsLeft = CALIB_PROMPT_MS / 1000 - (int)((millis() - promptStart) / 1000UL);
    showCalibPrompt(max(secsLeft, 1));
    if (buttonPressed()) {
      waitButtonRelease();
      doCalib = true;
      break;
    }
    delay(50);
  }

  if (doCalib) {
    runCalibration();
  } else if (!hasCal) {
    Serial.println("[BOOT] no calibration saved — using defaults");
    calOffset = 0.0f;
    calScale  = 1.0f;
    showInfo("NO CALIBRATION", "Using defaults.", "Reboot & press BTN1", "to calibrate.");
    delay(3000);
  }

  // ── BLE setup ──
  NimBLEDevice::init("ESP32-C6");
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);
  pTxChar = pService->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());
  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setName("ESP32-C6");
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x40);
  pAdv->start();
  Serial.println("[BOOT] advertising as 'ESP32-C6'");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!scale.is_ready()) { delay(10); return; }

  long  raw      = scale.read();
  float weightKg = (raw - calOffset) * calScale;

  updateDisplay(weightKg);

  static long lastRaw = LONG_MIN;
  if (deviceConnected && raw != lastRaw) {
    lastRaw = raw;
    String data = String(weightKg, 2) + " kg\n";
    pTxChar->setValue(data.c_str());
    pTxChar->notify();
    Serial.print("[TX] "); Serial.print(data);
  }

  if (!deviceConnected) {
    uint8_t flash = (millis() / 25) % 2;
    led.setPixelColor(0, flash ? led.Color(255, 0, 0) : led.Color(0, 0, 0));
    led.show();
  }
}
