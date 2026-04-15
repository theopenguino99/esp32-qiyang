#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// GPIO8 is the WS2812B RGB LED on the ESP32-C6 DevKitC-1
#define LED_PIN   8
#define NUM_LEDS  1

Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  led.begin();
  led.setBrightness(50);
  led.show();

  Serial.begin(115200);
  // Wait up to 3s for USB CDC serial to connect (needed on C6 with USB CDC)
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000);
  Serial.println("ESP32-C6 RGB Blink Start!");
}

void loop() {
  // Red
  led.setPixelColor(0, led.Color(255, 0, 0));
  led.show();
  Serial.println("RED");
  delay(1000);

  // Green
  led.setPixelColor(0, led.Color(0, 255, 0));
  led.show();
  Serial.println("GREEN");
  delay(1000);

  // Blue
  led.setPixelColor(0, led.Color(0, 0, 255));
  led.show();
  Serial.println("BLUE");
  delay(1000);

  // Pause (off)
  led.setPixelColor(0, led.Color(0, 0, 0));
  led.show();
  Serial.println("PAUSE");
  delay(1000);
}
