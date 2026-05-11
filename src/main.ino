#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <HX711.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#define LED_PIN  8
#define NUM_LEDS 1

// SH1106 OLED (1.3")
#define OLED_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET -1
#define OLED_UPDATE_MS 50

// HX711 Load Cell pins
#define HX711_DT_PIN  11  // Data pin
#define HX711_SCK_PIN 10  // Clock pin

// Nordic UART Service (NUS) — what iPhone BLE serial apps expect
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone



Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HX711 scale;
NimBLECharacteristic* pTxChar = nullptr;
bool deviceConnected = false;
float calibrationFactor = 430.0;  // Adjust this based on your load cell calibration

float history[OLED_WIDTH];
uint16_t historyIndex = 0;
bool historyFull = false;
unsigned long lastDisplayUpdate = 0;

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

void pushHistory(float value) {
  history[historyIndex] = value;
  historyIndex = (historyIndex + 1) % OLED_WIDTH;
  if (historyIndex == 0) historyFull = true;
}

float getHistoryValue(int i, int count) {
  if (!historyFull) return history[i];
  int idx = (historyIndex + i) % OLED_WIDTH;
  return history[idx];
}

void updateDisplay(float weightKg) {
  static float lastWeight = NAN;
  if (weightKg == lastWeight) return; // update only when the reading changes
  if (millis() - lastDisplayUpdate < OLED_UPDATE_MS) return;
  lastDisplayUpdate = millis();
  lastWeight = weightKg;

  pushHistory(weightKg);

  int count = historyFull ? OLED_WIDTH : historyIndex;
  float minVal = weightKg;
  float maxVal = weightKg;
  for (int i = 0; i < count; i++) {
    float v = getHistoryValue(i, count);
    if (v < minVal) minVal = v;
    if (v > maxVal) maxVal = v;
  }
  if (maxVal - minVal < 0.05f) {
    maxVal = minVal + 0.05f;
  }

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(weightKg, 1);
  display.print("kg");

  const int textHeight = 16;
  const int graphTop = textHeight + 2;
  const int graphBottom = OLED_HEIGHT - 1;
  const int graphHeight = graphBottom - graphTop;

  for (int i = 1; i < count; i++) {
    float v0 = getHistoryValue(i - 1, count);
    float v1 = getHistoryValue(i, count);
    int x0 = i - 1;
    int x1 = i;
    int y0 = graphBottom - (int)(((v0 - minVal) / (maxVal - minVal)) * graphHeight);
    int y1 = graphBottom - (int)(((v1 - minVal) / (maxVal - minVal)) * graphHeight);
    display.drawLine(x0, y0, x1, y1, SH110X_WHITE);
  }

  display.display();
}

void setup() {
  led.begin();
  led.setBrightness(50);
  led.setPixelColor(0, led.Color(0, 0, 0));
  led.show();

  Wire.begin();
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED init failed");
  }
  display.clearDisplay();
  display.display();

  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000);
  Serial.println("ESP32-C6 Load Cell + BLE UART Test");

  // Initialize HX711
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  scale.set_scale(calibrationFactor);
  scale.tare();  // Reset the scale to 0
  Serial.println("Load cell initialized and tared");

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

  // Get weight in kg
  float weightKg = scale.get_units(1) / 1000.0;

  updateDisplay(weightKg);

  // Only transmit when the reading changes
  static float lastSentWeight = NAN;
  bool readingChanged = isnan(lastSentWeight) || weightKg != lastSentWeight;

  if (deviceConnected && readingChanged) {
    lastSentWeight = weightKg;

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
