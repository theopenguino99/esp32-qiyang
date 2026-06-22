#pragma once

// Single shared header for the whole firmware. Each .cpp includes just this
// file; it provides every constant, shared object, shared variable, and
// function prototype the modules need to see each other.

#include <Arduino.h>
#include <Wire.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SH110X.h>
#include <HX711.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// ─── Pins ────────────────────────────────────────────────────────────────────
#define LED_PIN       8
#define NUM_LEDS      1
#define BUZZER_PIN    9
#define HX711_DT_PIN  11
#define HX711_SCK_PIN 10
#define BUTTON1_PIN   2
#define BUTTON2_PIN   3
#define BATT_ADC_PIN  1    // GPIO1 / ADC1_CH1 — battery sense (100k/100k divider, Vbatt = 2*Vadc)

// ─── OLED — SH1106 1.3" 128×64 ───────────────────────────────────────────────
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_RESET  -1
#define OLED_SDA    6
#define OLED_SCL    7
#define OLED_UPDATE_MS 50   // 50ms refresh (cherry-picked from oled-display)

// ─── BLE (Nordic UART Service) — kept for the web app ────────────────────────
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─── Tindeq Progressor BLE protocol — lets the official Tindeq app connect ────
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

// ─── Calibration ─────────────────────────────────────────────────────────────
#define CALIB_WEIGHT_KG   10.0f  // default reference weight; user-selectable at calibration
#define CALIB_SAMPLES     10
#define CALIB_COUNTDOWN_S 30     // seconds to place known weight
#define DEBOUNCE_MS       50

// ─── Battery monitor — GPIO1/ADC1_CH1 via a 100k/100k divider (Vbatt = 2*Vadc) ─
#define BATT_FULL_MV   4200   // 1S LiPo fully charged (CC/CV terminates at 4.2V) → 100%
#define BATT_EMPTY_MV  3500   // AP2112K-3.3 brown-out floor (3.3V + ~200mV dropout) → 0%
#define BATT_READ_MS   2000   // sampling cadence
#define BATT_SAMPLES   8      // ADC reads averaged per update

// ─── Shared peripheral objects (hardware.cpp) ────────────────────────────────
extern Adafruit_NeoPixel led;
extern Adafruit_SH1106G  display;
extern HX711             scale;
extern Preferences       prefs;

// ─── Input (input.cpp) ───────────────────────────────────────────────────────
bool buttonPressed(int pin = BUTTON1_PIN);   // debounced active-low read
void waitButtonRelease(int pin = BUTTON1_PIN);
void beepCount(int n);

// ─── Battery (battery.cpp) ───────────────────────────────────────────────────
extern uint32_t batteryMv;   // measured battery voltage (mV)
extern int      batteryPct;  // 0..100, derived from batteryMv
void updateBattery();

// ─── Calibration (calibration.cpp) ───────────────────────────────────────────
// Model: weightKg = (raw - calOffset) * calScale. Persisted to flash
// (Preferences namespace "hx711", keys "offset"/"scale"/"calwt").
extern float calOffset;
extern float calScale;
extern float calWeight;      // selected reference weight, persisted to flash
bool  loadCalibration();     // true if offset+scale were found in flash
void  saveCalibration();
float readRawAverage(int samples);
void  selectCalibWeight();   // weight-preset chooser (blocking, boot-only)
void  runCalibration();      // full boot calibration flow

// ─── OLED screens (ui.cpp) ───────────────────────────────────────────────────
void showCalibPrompt();
void showInfo(const char* title, const char* line1,
              const char* line2 = nullptr, const char* line3 = nullptr);
void showCountdown(int secsLeft);
void showCalibResult(bool ok);
void showCalWeightSelect(float w);
void updateDisplay(float weightKg);

// ─── BLE services (ble_service.cpp) ──────────────────────────────────────────
extern bool deviceConnected;            // a central is connected (NUS gate)
extern NimBLECharacteristic* pTxChar;   // NUS TX notify characteristic
extern bool          tindeqMeasuring;   // streaming between START(101)/STOP(102)
extern bool          tindeqTareReq;     // TARE(100) requested, serviced in loop()
extern unsigned long measureStartMicros;// micros() at START, for sample timestamps
void tindeqNotifyWeight(float kg, uint32_t tsMicros);
void bleSetup();
