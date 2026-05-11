#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <HX711.h>
#include <Preferences.h>
#include <limits.h>

#define LED_PIN  8
#define NUM_LEDS 1

// Buzzer pin for calibration prompts
#define BUZZER_PIN 9

// HX711 Load Cell pins
#define HX711_DT_PIN  11  // Data pin
#define HX711_SCK_PIN 10  // Clock pin

// Nordic UART Service (NUS) — what iPhone BLE serial apps expect
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone



Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
HX711 scale;
Preferences prefs;
NimBLECharacteristic* pTxChar = nullptr;
bool deviceConnected = false;
float calOffset = 0.0f;
float calScale = 1.0f;

// Calibration settings
const float CALIB_WEIGHT_KG = 8.0f;
const int CALIB_SAMPLES = 10;
const unsigned long CALIB_STABILIZE_MS = 2000;
const unsigned long CALIB_STEP_DELAY_MS = 3000;
const bool FORCE_CALIBRATION_ON_BOOT = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected — restarting advertising");
    NimBLEDevice::startAdvertising();
  }
};

// Called when the phone sends data to the ESP32
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.empty()) return;

    Serial.print("[RX] ");
    Serial.println(val.c_str());

    // Echo back with a prefix so you can confirm round-trip
    String reply = "Echo: " + String(val.c_str());
    pTxChar->setValue(reply.c_str());
    pTxChar->notify();
  }
};

void beepCount(int count) {
  for (int i = 0; i < count; i++) {
    tone(BUZZER_PIN, 2000);
    delay(120);
    noTone(BUZZER_PIN);
    delay(120);
  }
}

float readRawAverage(int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    while (!scale.is_ready()) {
      delay(10);
    }
    sum += scale.read();
  }
  return (float)sum / (float)samples;
}

bool loadCalibration() {
  prefs.begin("hx711", true);
  bool hasOffset = prefs.isKey("offset");
  bool hasScale = prefs.isKey("scale");
  if (hasOffset && hasScale) {
    calOffset = prefs.getFloat("offset", 0.0f);
    calScale = prefs.getFloat("scale", 1.0f);
  }
  prefs.end();
  return hasOffset && hasScale;
}

void saveCalibration() {
  prefs.begin("hx711", false);
  prefs.putFloat("offset", calOffset);
  prefs.putFloat("scale", calScale);
  prefs.end();
}

void runCalibration() {
  Serial.println("Calibration start");

  // Two beeps: remove weight
  beepCount(2);
  delay(CALIB_STEP_DELAY_MS);
  calOffset = readRawAverage(CALIB_SAMPLES);
  Serial.print("Offset raw: ");
  Serial.println(calOffset, 2);

  // One beep: add known weight
  beepCount(1);
  delay(CALIB_STEP_DELAY_MS);
  float rawWithWeight = readRawAverage(CALIB_SAMPLES);
  Serial.print("Raw with weight: ");
  Serial.println(rawWithWeight, 2);

  float delta = rawWithWeight - calOffset;
  if (delta <= 0.0f) {
    Serial.println("Calibration failed: invalid delta");
    calScale = 1.0f;
    return;
  }

  calScale = CALIB_WEIGHT_KG / delta;
  saveCalibration();

  // Three beeps: calibration done
  beepCount(3);
  Serial.print("Scale factor: ");
  Serial.println(calScale, 6);
}

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);

  led.begin();
  led.setBrightness(50);
  led.setPixelColor(0, led.Color(0, 0, 0));
  led.show();

  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000);
  Serial.println("ESP32-C6 Load Cell + BLE UART Test");

  // Initialize HX711
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  Serial.println("Load cell initialized");

  bool hasCal = loadCalibration();
  if (FORCE_CALIBRATION_ON_BOOT || !hasCal) {
    runCalibration();
  }

  NimBLEDevice::init("ESP32-C6");

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);

  // TX — ESP32 notifies the phone
  pTxChar = pService->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  // RX — phone writes to ESP32
  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->enableScanResponse(true);  // Send name in scan response (main packet is full with 128-bit UUID)
  pAdv->setName("ESP32-C6");   // Explicitly include name so Chrome Web Bluetooth can filter it
  pAdv->setMinInterval(0x20);  // 32 * 0.625ms = 20ms
  pAdv->setMaxInterval(0x40);  // 64 * 0.625ms = 40ms
  pAdv->start();

  Serial.println("Advertising as 'ESP32-C6' — waiting for connection...");
}

void loop() {
  if (!scale.is_ready()) {
    Serial.println("HX711 not ready");
    delay(100);
    return;
  }

  // Read raw data and compute calibrated weight
  long raw = scale.read();
  float weightKg = (raw - calOffset) * calScale;

  // Only transmit when the raw reading changes
  static long lastRaw = LONG_MIN;
  if (deviceConnected && raw != lastRaw) {
    lastRaw = raw;

    // Send weight to webapp
    String data = String(weightKg, 2) + " kg\n";
    pTxChar->setValue(data.c_str());
    pTxChar->notify();
    Serial.print("[TX] ");
    Serial.print(data);
  }

  if (!deviceConnected) {
    // Flash red LED when disconnected
    uint8_t flash = (millis() / 25) % 2;  // Toggle every 25ms
    if (flash) {
      led.setPixelColor(0, led.Color(255, 0, 0));  // Red
    } else {
      led.setPixelColor(0, led.Color(0, 0, 0));    // Off
    }
    led.show();
  }
}
