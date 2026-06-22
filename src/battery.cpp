#include "app.h"

uint32_t batteryMv  = BATT_FULL_MV;  // measured battery voltage (mV)
int      batteryPct = 100;           // 0..100, derived from batteryMv

static unsigned long lastBattRead = 0;

void updateBattery() {
  if (lastBattRead != 0 && millis() - lastBattRead < BATT_READ_MS) return;
  lastBattRead = millis();
  uint32_t sum = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) sum += analogReadMilliVolts(BATT_ADC_PIN);
  batteryMv = (sum / BATT_SAMPLES) * 2;   // undo the 100k/100k divider
  long pct = ((long)batteryMv - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV);
  batteryPct = constrain((int)pct, 0, 100);
}
