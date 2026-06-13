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
#define BUTTON2_PIN   3

// OLED — SH1106 1.3" 128×64
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_RESET  -1
#define OLED_SDA    6
#define OLED_SCL    7
#define OLED_UPDATE_MS 50   // 50ms refresh (cherry-picked from oled-display)

// BLE (Nordic UART Service) — kept for the web app
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Tindeq Progressor BLE protocol — lets the official Tindeq app connect.
// All multi-byte values are little-endian (ESP32 native), so float/uint32 can
// be memcpy'd straight into the packet.
#define TINDEQ_SERVICE_UUID "7e4e1701-1ea6-40c9-9dcc-13d34ffead57"
#define TINDEQ_DATA_UUID    "7e4e1702-1ea6-40c9-9dcc-13d34ffead57" // notify: device -> app
#define TINDEQ_CTRL_UUID    "7e4e1703-1ea6-40c9-9dcc-13d34ffead57" // write:  app -> device
// Control-point command opcodes (app -> device)
#define TINDEQ_CMD_TARE        100
#define TINDEQ_CMD_START_MEAS  101
#define TINDEQ_CMD_STOP_MEAS   102
#define TINDEQ_CMD_GET_APP_VER 107
#define TINDEQ_CMD_ENTER_SLEEP 110
#define TINDEQ_CMD_GET_BATT    111
// Notification response codes (device -> app)
#define TINDEQ_RES_CMD     0x00  // command response (battery / version / ...)
#define TINDEQ_RES_WEIGHT  0x01  // weight sample: float32 kg + uint32 timestamp_us
#define TINDEQ_BATTERY_MV  3700  // placeholder — no battery monitor wired on this board

// Calibration
#define CALIB_WEIGHT_KG   10.0f  // default reference weight; user-selectable at calibration
#define CALIB_SAMPLES     10
#define CALIB_PROMPT_MS   5000   // boot window to press BTN1
#define CALIB_COUNTDOWN_S 30     // seconds to place known weight
#define DEBOUNCE_MS       50

Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SH1106G  display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HX711 scale;
Preferences prefs;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pTindeqData = nullptr;
bool deviceConnected = false;
float calOffset = 0.0f;
float calScale  = 1.0f;
float calWeight = CALIB_WEIGHT_KG;  // selected reference weight, persisted to flash

// Tindeq measurement state
bool          tindeqMeasuring   = false;  // streaming between START(101)/STOP(102)
bool          tindeqTareReq     = false;  // TARE(100) requested from app, serviced in loop()
unsigned long measureStartMicros = 0;     // micros() at START, for the sample timestamp

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

// ─── Tindeq Progressor notifications ─────────────────────────────────────────
// Packet layout: [response_code][payload_len][payload...]
// A weight sample payload is one 8-byte pair: float32 kg + uint32 timestamp_us.
void tindeqNotifyWeight(float kg, uint32_t tsMicros) {
  if (!pTindeqData) return;
  uint8_t pkt[10];
  pkt[0] = TINDEQ_RES_WEIGHT;
  pkt[1] = 8;                       // one sample
  memcpy(&pkt[2], &kg, 4);          // float32, little-endian
  memcpy(&pkt[6], &tsMicros, 4);    // uint32,  little-endian
  pTindeqData->setValue(pkt, sizeof(pkt));
  pTindeqData->notify();
}

// Command response (e.g. battery voltage, version string)
void tindeqNotifyCmdResponse(const uint8_t* payload, uint8_t len) {
  if (!pTindeqData) return;
  uint8_t pkt[24];
  if (len > sizeof(pkt) - 2) len = sizeof(pkt) - 2;
  pkt[0] = TINDEQ_RES_CMD;
  pkt[1] = len;
  memcpy(&pkt[2], payload, len);
  pTindeqData->setValue(pkt, len + 2);
  pTindeqData->notify();
}

// Control-point writes from the Tindeq app. Keep this non-blocking: heavy work
// (tare averaging) is deferred to loop() via a flag.
class TindeqCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string v = pChar->getValue();
    if (v.empty()) return;
    uint8_t op = (uint8_t)v[0];     // opcode is the first byte (TLV; args ignored)
    switch (op) {
      case TINDEQ_CMD_TARE:
        tindeqTareReq = true;
        Serial.println("[TQ] tare");
        break;
      case TINDEQ_CMD_START_MEAS:
        measureStartMicros = micros();
        tindeqMeasuring = true;
        Serial.println("[TQ] start measurement");
        break;
      case TINDEQ_CMD_STOP_MEAS:
        tindeqMeasuring = false;
        Serial.println("[TQ] stop measurement");
        break;
      case TINDEQ_CMD_GET_BATT: {
        uint32_t mv = TINDEQ_BATTERY_MV;
        tindeqNotifyCmdResponse((uint8_t*)&mv, 4);
        Serial.println("[TQ] battery query");
        break;
      }
      case TINDEQ_CMD_GET_APP_VER: {
        const char* ver = "1.0.0";
        tindeqNotifyCmdResponse((const uint8_t*)ver, strlen(ver));
        Serial.println("[TQ] version query");
        break;
      }
      case TINDEQ_CMD_ENTER_SLEEP:
        tindeqMeasuring = false;
        Serial.println("[TQ] sleep (ignored)");
        break;
      default:
        Serial.printf("[TQ] unhandled opcode %u\n", op);
        break;
    }
  }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────
bool buttonPressed(int pin = BUTTON1_PIN) {
  if (digitalRead(pin) == LOW) {
    delay(DEBOUNCE_MS);
    return digitalRead(pin) == LOW;
  }
  return false;
}

void waitButtonRelease(int pin = BUTTON1_PIN) {
  while (digitalRead(pin) == LOW) delay(10);
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
  calWeight = prefs.getFloat("calwt", CALIB_WEIGHT_KG);  // last-used weight (default 10 kg)
  prefs.end();
  return ok;
}

void saveCalibration() {
  prefs.begin("hx711", false);
  prefs.putFloat("offset", calOffset);
  prefs.putFloat("scale",  calScale);
  prefs.putFloat("calwt",  calWeight);
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
  display.setCursor(4, 16);
  display.println("BTN2: calibrate");
  display.setCursor(4, 28);
  display.println("BTN1: skip");
  display.setCursor(4, 48);
  display.print("Auto-skip in ");
  display.print(secsLeft);
  display.print("s");
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
  display.print("BTN2 = confirm now");
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
    char wbuf[20];
    snprintf(wbuf, sizeof(wbuf), "%g kg factor:", calWeight);
    display.print(wbuf);
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

// ─── Calibration-weight chooser ──────────────────────────────────────────────
// Standard kg gym-plate presets. BTN1 cycles, BTN2 confirms.
void showCalWeightSelect(float w) {
  display.clearDisplay();
  drawTitleBar("SET CAL WEIGHT");
  char buf[16];
  snprintf(buf, sizeof(buf), "%g kg", w);
  display.setTextSize(2);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 24);
  display.print(buf);
  display.setTextSize(1);
  display.setCursor(4, 54);
  display.print("BTN1:change  BTN2:OK");
  display.display();
}

void selectCalibWeight() {
  static const float presets[] = {1.25f, 2.5f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
  const int n = sizeof(presets) / sizeof(presets[0]);
  // Start at the preset closest to the last-used weight.
  int idx = 0;
  for (int i = 1; i < n; i++) {
    if (fabsf(presets[i] - calWeight) < fabsf(presets[idx] - calWeight)) idx = i;
  }
  waitButtonRelease(BUTTON2_PIN);   // clear the press that entered calibration
  showCalWeightSelect(presets[idx]);
  while (true) {
    if (buttonPressed(BUTTON1_PIN)) {        // cycle to next preset
      idx = (idx + 1) % n;
      showCalWeightSelect(presets[idx]);
      waitButtonRelease(BUTTON1_PIN);
    }
    if (buttonPressed(BUTTON2_PIN)) {        // confirm
      waitButtonRelease(BUTTON2_PIN);
      break;
    }
    delay(20);
  }
  calWeight = presets[idx];
  Serial.print("[CAL] weight selected="); Serial.println(calWeight, 2);
}

// ─── Calibration flow ────────────────────────────────────────────────────────
void runCalibration() {
  Serial.println("[CAL] start");

  // Step 0 — user picks the reference weight (BTN1 cycles, BTN2 confirms)
  selectCalibWeight();

  // Step 1 — zero offset (scale must be empty)
  showInfo("CALIBRATING", "Reading zero...", "Keep scale empty!", nullptr);
  beepCount(1);
  delay(1500);
  calOffset = readRawAverage(CALIB_SAMPLES);
  Serial.print("[CAL] offset="); Serial.println(calOffset, 2);

  // Step 2 — countdown for user to place the selected weight
  char placeLine[24];
  snprintf(placeLine, sizeof(placeLine), "Put %g kg on scale.", calWeight);
  showInfo("PLACE WEIGHT", placeLine, "30s countdown starts.", "BTN2 = confirm early");
  beepCount(2);
  delay(2000);
  waitButtonRelease(BUTTON2_PIN); // clear any residual press from the trigger

  unsigned long countStart  = millis();
  const unsigned long total = (unsigned long)CALIB_COUNTDOWN_S * 1000UL;

  while (millis() - countStart < total) {
    int secsLeft = CALIB_COUNTDOWN_S - (int)((millis() - countStart) / 1000UL);
    showCountdown(max(secsLeft, 0));
    if (buttonPressed(BUTTON2_PIN)) {
      waitButtonRelease(BUTTON2_PIN);
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

  calScale = calWeight / delta;
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
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
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
    if (buttonPressed(BUTTON2_PIN)) {     // BTN2 = calibrate
      waitButtonRelease(BUTTON2_PIN);
      doCalib = true;
      break;
    }
    if (buttonPressed(BUTTON1_PIN)) {     // BTN1 = skip now
      waitButtonRelease(BUTTON1_PIN);
      break;
    }
    delay(50);
  }

  if (doCalib) {
    runCalibration();
  } else if (hasCal) {
    // Boot tare — refresh only the zero offset from the (empty) scale, in memory
    // only. The saved scale factor and offset in flash are left untouched, so the
    // previous calibration is preserved; this just cancels power-on zero drift.
    // Assumes the scale is empty at boot.
    showInfo("TARING", "Zeroing scale...", "Keep scale empty!", nullptr);
    delay(800);  // let the user clear the scale / readings settle
    calOffset = readRawAverage(CALIB_SAMPLES);
    Serial.print("[CAL] boot tare offset="); Serial.println(calOffset, 2);
  } else {
    Serial.println("[BOOT] no calibration saved — using defaults");
    calOffset = 0.0f;
    calScale  = 1.0f;
    showInfo("NO CALIBRATION", "Using defaults.", "Reboot & press BTN2", "to calibrate.");
    delay(3000);
  }

  // ── BLE setup ──
  // Advertise as a Tindeq Progressor ("Progressor_XXXX") so the Tindeq app
  // discovers it. XXXX is derived from the chip MAC for a stable unique name.
  char devName[20];
  uint16_t devId = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
  snprintf(devName, sizeof(devName), "Progressor_%04X", devId);
  NimBLEDevice::init(devName);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);
  pTxChar = pService->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());
  pService->start();

  // Tindeq Progressor service (runs in parallel with NUS)
  NimBLEService* pTindeqSvc = pServer->createService(TINDEQ_SERVICE_UUID);
  pTindeqData = pTindeqSvc->createCharacteristic(TINDEQ_DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pTindeqCtrl = pTindeqSvc->createCharacteristic(
    TINDEQ_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pTindeqCtrl->setCallbacks(new TindeqCtrlCallbacks());
  pTindeqSvc->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  // Advertise the Tindeq service UUID so the app filters us in (one 128-bit
  // UUID fills the adv packet; the name rides in the scan response).
  pAdv->addServiceUUID(TINDEQ_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setName(devName);
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x40);
  pAdv->start();
  Serial.printf("[BOOT] advertising as '%s' (Tindeq Progressor + NUS)\n", devName);
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!scale.is_ready()) { delay(10); return; }

  long  raw      = scale.read();
  float weightKg = (raw - calOffset) * calScale;

  updateDisplay(weightKg);

  // ── Tindeq Progressor ──
  // Soft tare (re-zero) requested by the app — in-memory only, not persisted to
  // flash, so the saved 10 kg calibration scale factor is preserved.
  if (tindeqTareReq) {
    calOffset = readRawAverage(CALIB_SAMPLES);
    tindeqTareReq = false;
    Serial.println("[TQ] tared");
  }
  // Stream weight samples while a measurement is active.
  if (tindeqMeasuring) {
    tindeqNotifyWeight(weightKg, (uint32_t)(micros() - measureStartMicros));
  }

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
