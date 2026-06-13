# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

ESP32-C6 (DevKitC-1) firmware for a smart digital scale: HX711 load cell + SH1106
OLED + BLE UART + on-device button calibration. Single-file Arduino sketch built
with PlatformIO (pioarduino platform for C6 support).

All firmware lives in **`src/main.ino`**. There is no `.cpp`/`.h` split — keep new
code in that file unless asked otherwise.

## Commands

```bash
pio run                # build only (use this to verify compiles)
pio run -t upload      # build + flash over USB
pio device monitor     # serial @ 115200
```

There is no host-side test suite; "verifying" means it compiles (`pio run`) and,
ideally, behaves on hardware. The board may not be connected — don't assume upload.

## Pin map (single source of truth: the `#define`s at top of main.ino)

| Function | GPIO | | Function | GPIO |
|----------|------|-|----------|------|
| HX711 DT | 11   | | OLED SDA | 6    |
| HX711 SCK| 10   | | OLED SCL | 7    |
| RGB LED  | 8    | | Button 1 | 2    |
| Buzzer   | 9    | | Button 2 | (reserved, unused) |

Button 2 exists in hardware (see schematic) but is not wired up in firmware yet —
it's reserved for future use.

## Code structure in main.ino (top → bottom)

1. `#define`s — pins, OLED, BLE UUIDs, calibration tunables (`CALIB_*`).
2. Globals — `led`, `display`, `scale`, `prefs`, BLE char, `calOffset`/`calScale`,
   history ring buffer.
3. BLE callbacks (`ServerCallbacks`, `RxCallbacks`).
4. Helpers — button debounce, beeps, `readRawAverage`, calibration load/save.
5. OLED screens — `drawTitleBar`, `showCalibPrompt`, `showInfo`, `showCountdown`,
   `showCalibResult`.
6. `runCalibration()` — the boot calibration flow.
7. `updateDisplay()` + history helpers — normal-operation screen.
8. `setup()` / `loop()`.

## Key conventions

- **Calibration model:** `weightKg = (raw - calOffset) * calScale`. Offset and
  scale are persisted to flash via `Preferences` namespace `"hx711"`, keys
  `"offset"` / `"scale"`. `CALIB_WEIGHT_KG` (10 kg) is the reference mass.
- **Button is active-low** (`INPUT_PULLUP`); pressed == `LOW`. Always debounce via
  the existing `buttonPressed()` / `waitButtonRelease()` helpers.
- **OLED:** 128×64 SH1106 via `Adafruit_SH110X`. Colours are `SH110X_WHITE`.
  Title screens use `drawTitleBar()`; keep new screens consistent with it.
- **BLE TX format:** plain text `"<weight> kg\n"`, sent only when the raw reading
  changes. The web app at `../esp32-qiyang-webapp` parses this.
- Serial logs are tagged: `[BOOT]`, `[CAL]`, `[BLE]`, `[TX]`.

## Critical build flags (platformio.ini — do not remove)

`-DARDUINO_USB_CDC_ON_BOOT=1` and `-DARDUINO_USB_MODE=1` are required for the C6
to enumerate over USB. Library deps: Adafruit NeoPixel, Adafruit GFX, Adafruit
SH110X, NimBLE-Arduino, HX711.

## Branches

Feature branches per area (`oled-display`, `inbuilt-calibration`, `bluetooth`,
`mvp1_scale`). `main` is the integration target. Check the current branch before
committing; only commit/push when asked.

## Gotchas

- `runCalibration()` uses blocking `delay()`/busy loops — that's intentional, it
  runs only at boot before BLE starts. Don't add blocking delays to `loop()`.
- The history graph buffer is `OLED_WIDTH` (128) floats; index wraps via modulo.
- HX711 raw reads block on `is_ready()`; guard with `delay()` not tight spins.
