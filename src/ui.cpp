#include "app.h"

// History ring-buffer for the normal-operation graph.
static float    history[OLED_WIDTH] = {};
static uint16_t historyIndex = 0;
static bool     historyFull  = false;
static unsigned long lastDisplayUpdate = 0;

// ─── Title bar ───────────────────────────────────────────────────────────────
static void drawHRule(int y) {
  display.drawFastHLine(0, y, OLED_WIDTH, SH110X_WHITE);
}

// Centred title + horizontal rule at y=9
static void drawTitleBar(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 0);
  display.print(title);
  drawHRule(9);
}

// ─── Screens ─────────────────────────────────────────────────────────────────
void showCalibPrompt() {
  display.clearDisplay();
  drawTitleBar("CALIBRATE?");
  display.setTextSize(1);
  display.setCursor(4, 20);
  display.println("BTN2: calibrate");
  display.setCursor(4, 36);
  display.println("BTN1: skip");
  display.display();
}

void showInfo(const char* title, const char* line1, const char* line2, const char* line3) {
  display.clearDisplay();
  drawTitleBar(title);
  display.setTextSize(1);
  int y = 14;
  if (line1) { display.setCursor(4, y); display.println(line1); y += 10; }
  if (line2) { display.setCursor(4, y); display.println(line2); y += 10; }
  if (line3) { display.setCursor(4, y); display.println(line3); }
  display.display();
}

void showCountdown(int secsLeft) {
  display.clearDisplay();
  drawTitleBar("PLACE WEIGHT");

  // Large centred countdown digit(s)
  display.setTextSize(3);
  char buf[8];
  snprintf(buf, sizeof(buf), "%ds", secsLeft);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 18);
  display.print(buf);

  // Hint and progress bar
  display.setTextSize(1);
  display.setCursor(4, 47);
  display.print("BTN2 = confirm now");
  int barW = map(secsLeft, 0, CALIB_COUNTDOWN_S, 0, OLED_WIDTH - 2);
  display.drawRect(0, 57, OLED_WIDTH, 7, SH110X_WHITE);
  display.fillRect(1, 58, barW, 5, SH110X_WHITE);

  display.display();
}

void showCalibResult(bool ok) {
  display.clearDisplay();
  drawTitleBar(ok ? "  CALIBRATED!" : " CALIB FAILED");
  display.setTextSize(1);
  if (ok) {
    display.setCursor(4, 16);
    display.println("Calibration saved.");
    display.setCursor(4, 28);
    char wbuf[20];
    snprintf(wbuf, sizeof(wbuf), "%g kg factor:", calWeight);
    display.print(wbuf);
    display.setCursor(4, 38);
    display.print(calScale, 5);
  } else {
    display.setCursor(4, 16);
    display.println("Bad reading!");
    display.setCursor(4, 28);
    display.println("Check weight is on");
    display.setCursor(4, 38);
    display.println("scale and retry.");
  }
  display.display();
  beepCount(ok ? 3 : 1);
  delay(3000);
}

void showCalWeightSelect(float w) {
  display.clearDisplay();
  drawTitleBar("SET CAL WEIGHT");
  char buf[16];
  snprintf(buf, sizeof(buf), "%g kg", w);
  display.setTextSize(2);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((OLED_WIDTH - (int)bw) / 2, 24);
  display.print(buf);
  display.setTextSize(1);
  display.setCursor(4, 54);
  display.print("BTN1:change  BTN2:OK");
  display.display();
}

// ─── Normal-operation display ────────────────────────────────────────────────
static void pushHistory(float v) {
  history[historyIndex] = v;
  historyIndex = (historyIndex + 1) % OLED_WIDTH;
  if (historyIndex == 0) historyFull = true;
}

static float getHistoryAt(int i) {
  if (!historyFull) return history[i];
  return history[(historyIndex + i) % OLED_WIDTH];
}

// Small battery glyph at the top-right corner; inner fill ∝ percent, with the
// percentage printed as text right-aligned just to the left of the glyph.
static void drawBatteryIcon(int pct) {
  const int w = 16, h = 9;
  const int x = OLED_WIDTH - w - 3;   // 3px keeps the +terminal nub on-screen
  const int y = 0;
  display.drawRect(x, y, w, h, SH110X_WHITE);             // body outline
  display.fillRect(x + w, y + 3, 2, h - 6, SH110X_WHITE); // + terminal nub
  int fw = ((w - 2) * pct + 50) / 100;                    // rounded fill width
  if (fw > 0) display.fillRect(x + 1, y + 1, fw, h - 2, SH110X_WHITE);

  // Percentage label to the left of the glyph, right-aligned against it.
  char pbuf[6];
  snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(pbuf, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(x - 3 - (int)bw, y + 1);             // 3px gap before the body
  display.print(pbuf);
}

void updateDisplay(float weightKg) {
  if (millis() - lastDisplayUpdate < OLED_UPDATE_MS) return;
  lastDisplayUpdate = millis();

  // Extra smoothing for the OLED readout + graph only (the BLE streams already
  // got the low-lag median in loop() and are not affected by this EMA).
  static float ema = 0.0f;
  static bool  emaInit = false;
  if (!emaInit) { ema = weightKg; emaInit = true; }
  else          { ema += DISPLAY_EMA_ALPHA * (weightKg - ema); }
  weightKg = ema;

  pushHistory(weightKg);

  int count = historyFull ? OLED_WIDTH : (int)historyIndex;
  float minV = weightKg, maxV = weightKg;
  for (int i = 0; i < count; i++) {
    float v = getHistoryAt(i);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  if (maxV - minV < 0.05f) maxV = minV + 0.05f;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Battery indicator — top-right, always on.
  drawBatteryIcon(batteryPct);

  // Weight: integer + point at size 3; the 2 decimals and "kg" at size 2 and
  // dropped below the battery row, so they fit alongside the icon.
  char wbuf[12];
  snprintf(wbuf, sizeof(wbuf), "%.2f", weightKg);   // e.g. "12.34" / "-1.20"
  char* dotp = strchr(wbuf, '.');
  int   dot  = dotp ? (int)(dotp - wbuf) : (int)strlen(wbuf);
  char  big[10];
  int   bn = (dot + 1 < (int)sizeof(big)) ? dot + 1 : (int)sizeof(big) - 1;
  memcpy(big, wbuf, bn); big[bn] = '\0';            // integer part incl. the '.'

  display.setTextSize(3);
  display.setCursor(0, 2);
  display.print(big);
  int sx = display.getCursorX();
  display.setTextSize(2);
  display.setCursor(sx, 10);                         // y=10 keeps it clear of the icon
  display.print(dotp ? dotp + 1 : "");               // the 2 decimal digits
  display.print("kg");

  const int graphTop = 26, graphBottom = OLED_HEIGHT - 1;
  const int graphH   = graphBottom - graphTop;
  for (int i = 1; i < count; i++) {
    float v0 = getHistoryAt(i - 1), v1 = getHistoryAt(i);
    int y0 = graphBottom - (int)(((v0 - minV) / (maxV - minV)) * graphH);
    int y1 = graphBottom - (int)(((v1 - minV) / (maxV - minV)) * graphH);
    display.drawLine(i - 1, y0, i, y1, SH110X_WHITE);
  }
  display.display();
}
