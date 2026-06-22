#include "app.h"

Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SH1106G  display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HX711             scale;
Preferences       prefs;
