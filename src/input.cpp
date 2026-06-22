#include "app.h"

bool buttonPressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(DEBOUNCE_MS);
    return digitalRead(pin) == LOW;
  }
  return false;
}

void waitButtonRelease(int pin) {
  while (digitalRead(pin) == LOW) delay(10);
  delay(DEBOUNCE_MS);
}

void beepCount(int n) {
  for (int i = 0; i < n; i++) {
    tone(BUZZER_PIN, 2000, 120);
    delay(240);
  }
}
