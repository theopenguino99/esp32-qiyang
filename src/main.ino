#include "app.h"

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  // Battery ADC: 12-bit, 11dB attenuation (full ~0–3.1V range; divided Vbatt ≤ 2.1V)
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);
  updateBattery();   // prime batteryPct before the first display

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

  // ── Calibration prompt — wait (no time limit) for the user to choose ──
  showCalibPrompt();
  bool doCalib = false;
  while (true) {
    if (buttonPressed(BUTTON2_PIN)) {     // BTN2 = calibrate
      waitButtonRelease(BUTTON2_PIN);
      doCalib = true;
      break;
    }
    if (buttonPressed(BUTTON1_PIN)) {     // BTN1 = skip
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

  bleSetup();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!scale.is_ready()) { delay(10); return; }

  long  raw      = scale.read();
  float weightKg = medianFilteredKg(raw);   // spike-filtered, calibrated (low-lag)

  updateBattery();
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
