#include "app.h"

float calOffset = 0.0f;
float calScale  = 1.0f;
float calWeight = CALIB_WEIGHT_KG;  // selected reference weight, persisted to flash

// ─── Storage ─────────────────────────────────────────────────────────────────
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

float readRawAverage(int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    while (!scale.is_ready()) delay(10);
    sum += scale.read();
  }
  return (float)sum / samples;
}

// Running median of the last FILTER_MEDIAN_WINDOW raw counts, then converted to
// kg with the current calibration. Median rejects impulsive spikes with almost
// no lag (no smoothing of genuine transients), so it's safe on the RFD stream.
// Operates on raw counts (offset/scale-independent), so a tare mid-stream is fine.
float medianFilteredKg(long raw) {
  static long win[FILTER_MEDIAN_WINDOW];
  static int  count = 0;
  static int  head  = 0;

  win[head] = raw;
  head = (head + 1) % FILTER_MEDIAN_WINDOW;
  if (count < FILTER_MEDIAN_WINDOW) count++;

  long sorted[FILTER_MEDIAN_WINDOW];
  for (int i = 0; i < count; i++) sorted[i] = win[i];
  for (int i = 1; i < count; i++) {        // insertion sort (window is tiny)
    long key = sorted[i];
    int  j = i - 1;
    while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = key;
  }
  long med = sorted[count / 2];
  return (med - calOffset) * calScale;
}

// ─── Calibration-weight chooser ──────────────────────────────────────────────
// Standard kg gym-plate presets. BTN1 cycles, BTN2 confirms.
void selectCalibWeight() {
  static const float presets[] = {1.25f, 2.5f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f, 25.0f};
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
