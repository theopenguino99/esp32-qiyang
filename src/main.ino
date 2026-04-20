#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <HX711.h>

#define LED_PIN  8
#define NUM_LEDS 1

// HX711 Load Cell pins
#define HX711_DT_PIN  11  // Data pin
#define HX711_SCK_PIN 10  // Clock pin

// Nordic UART Service (NUS) — what iPhone BLE serial apps expect
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone

#define MAX_WEIGHT 20000.0  // 20kg in grams

Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
HX711 scale;
NimBLECharacteristic* pTxChar = nullptr;
bool deviceConnected = false;
float calibrationFactor = 430.0;  // Adjust this based on your load cell calibration

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

void setup() {
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
  pAdv->setMinInterval(0x20);  // 32 * 0.625ms = 20ms
  pAdv->setMaxInterval(0x40);  // 64 * 0.625ms = 40ms
  pAdv->start();

  Serial.println("Advertising as 'ESP32-C6' — waiting for connection...");
}

unsigned long lastSampleTime = 0;

void updateLEDFromWeight(float weightKg) {
  // Map weight from 0-1kg to LED color gradient (Blue to Red)
  // 0kg (min) = Blue (0, 0, 255)
  // 1kg (max) = Red (255, 0, 0)
  float ratio = constrain(weightKg / 1.0, 0.0, 1.0);
  
  uint8_t red = (uint8_t)(ratio * 255);
  uint8_t blue = (uint8_t)((1.0 - ratio) * 255);
  
  led.setPixelColor(0, led.Color(red, 0, blue));
  led.show();
}

void loop() {
  if (!scale.is_ready()) {
    Serial.println("HX711 not ready");
    delay(100);
    return;
  }

  // Read weight at ~80Hz (12.5ms interval)
  if (millis() - lastSampleTime >= 12) {
    lastSampleTime = millis();

    // Get weight in kg
    float weightKg = scale.get_units(1) / 1000.0;  // Single reading for faster response
    
    if (deviceConnected) {
      // Update LED based on weight (blue to red gradient)
      updateLEDFromWeight(weightKg);

      // Send weight to webapp
      String data = String(weightKg, 2) + " kg\n";
      pTxChar->setValue(data.c_str());
      pTxChar->notify();
      Serial.print("[TX] ");
      Serial.print(data);
    } else {
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
}
