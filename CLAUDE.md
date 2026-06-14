# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

ESP32-C6 (DevKitC-1) firmware for a smart digital scale: HX711 load cell + SH1106
OLED + BLE + on-device button calibration. It exposes two BLE services in
parallel: a **Tindeq Progressor** emulation (for the official Tindeq app) and the
**Nordic UART Service** (for the web app). Single-file Arduino sketch built with
PlatformIO (pioarduino platform for C6 support).

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
| Buzzer   | 9    | | Button 2 | 3    |

Both buttons are wired (active-low, `INPUT_PULLUP`): BTN1 (GPIO2) = change/skip,
BTN2 (GPIO3) = OK/confirm in the boot calibration UI.

## Hardware assets

Board and enclosure design files live in **`hardware/`** (not built by PlatformIO):

- `hardware/pcb_Crimp-ER.kicad_{pro,sch,pcb}` — KiCad project.
- `hardware/fab/` — gerbers, drill files, pick-and-place, fab zip.
- `hardware/crimp-ER covers/` — 3D-printed enclosure (`*.stl` + `*.step`).
- `hardware/renders/` — PNG previews embedded in the README (PCB front/back +
  cover renders). Regenerate the cover previews from the STLs if the models change.

## Code structure in main.ino (top → bottom)

1. `#define`s — pins, OLED, BLE UUIDs (NUS + `TINDEQ_*`), calibration tunables
   (`CALIB_*`).
2. Globals — `led`, `display`, `scale`, `prefs`, BLE chars,
   `calOffset`/`calScale`/`calWeight`, Tindeq state (`tindeqMeasuring`/`tindeqTareReq`/`measureStartMicros`), history
   ring buffer.
3. BLE callbacks (`ServerCallbacks`, `RxCallbacks`).
4. Tindeq protocol — `tindeqNotifyWeight`, `tindeqNotifyCmdResponse`,
   `TindeqCtrlCallbacks` (parses control-point opcodes).
5. Helpers — button debounce (`buttonPressed(pin)`), beeps, `readRawAverage`,
   calibration load/save.
6. OLED screens — `drawTitleBar`, `showCalibPrompt`, `showInfo`, `showCountdown`,
   `showCalibResult`, `showCalWeightSelect`.
7. `selectCalibWeight()` (weight-preset chooser) + `runCalibration()` — the boot
   calibration flow.
8. `updateDisplay()` + history helpers — normal-operation screen.
9. `setup()` / `loop()`.

## Key conventions

- **Calibration model:** `weightKg = (raw - calOffset) * calScale`. Offset, scale,
  and the chosen reference weight are persisted to flash via `Preferences`
  namespace `"hx711"`, keys `"offset"` / `"scale"` / `"calwt"`. The reference
  weight is user-selectable at calibration from gym-plate presets (`selectCalibWeight`);
  `CALIB_WEIGHT_KG` (10 kg) is only the default. `calScale = calWeight / delta`.
- **Boot flow:** the `CALIBRATE?` prompt waits with no timeout — BTN2 calibrates,
  BTN1 skips. On skip with a saved calibration, the device does an in-memory boot
  tare (re-zeros `calOffset` only, not persisted) to cancel power-on drift.
- **Buttons are active-low** (`INPUT_PULLUP`); pressed == `LOW`. Debounce via
  `buttonPressed(pin)` / `waitButtonRelease(pin)` (default `BUTTON1_PIN`). UI roles:
  BTN1 = change/skip, BTN2 = OK/confirm.
- **OLED:** 128×64 SH1106 via `Adafruit_SH110X`. Colours are `SH110X_WHITE`.
  Title screens use `drawTitleBar()`; keep new screens consistent with it.
- **NUS TX format:** plain text `"<weight> kg\n"`, sent only when the raw reading
  changes. The web app at `../esp32-qiyang-webapp` parses this.
- **Tindeq protocol:** packets are `[response_code][len][payload]`, little-endian
  (ESP32-native, so `memcpy` floats/uint32 straight in). Weight sample payload =
  `float32 kg` + `uint32 timestamp_µs`. Weight streams only between START (101) and
  STOP (102); see the `TINDEQ_*` opcode defines. Keep `TindeqCtrlCallbacks::onWrite`
  non-blocking — defer heavy work to `loop()` via a flag (as `tindeqTareReq` does).
- **Device advertises as `Progressor_XXXX`** (MAC-derived) and advertises the Tindeq
  service UUID; the name rides in the scan response. The web app is unaffected — it
  picks devices with `acceptAllDevices` and accesses NUS via `optionalServices`, so
  it works regardless of the name or which UUID is advertised (NUS still runs).
- Serial logs are tagged: `[BOOT]`, `[CAL]`, `[BLE]`, `[TX]`, `[TQ]` (Tindeq).

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
- **Tindeq tare** (`TINDEQ_CMD_TARE`) re-zeros `calOffset` in memory only — it is
  deliberately *not* saved, so the persisted 10 kg scale factor survives.
- **Tindeq sample rate = HX711 rate** (~10 Hz on the devboard but 80 Hz on the production board);
  RFD-style metrics will be coarser than a real Progressor.
- **Battery voltage is a placeholder** (`TINDEQ_BATTERY_MV`) — no battery monitor is
  wired on the DevKit.
