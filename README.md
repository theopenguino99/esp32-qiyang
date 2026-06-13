<div align="center">

<img src="assets/banner.svg" alt="crimp-ER — ESP32-C6 Firmware" width="640" />

**On-device firmware for the crimp-ER hangboard scale — ESP32-C6, HX711 load cell, OLED readout &amp; live force streaming over BLE.**

</div>

---

# ESP32-C6 Smart Load Cell

Firmware for an ESP32-C6 based digital scale. Reads an HX711 load cell, shows live
weight on a 1.3" OLED, streams readings over BLE, and supports on-device
calibration with a push button. It speaks the **Tindeq Progressor** BLE protocol,
so it works with the official Tindeq app as well as the companion web app.

## Hardware

| Peripheral        | Connection                          |
|-------------------|-------------------------------------|
| HX711 load cell   | DT = GPIO11, SCK = GPIO10           |
| SH1106 OLED (I2C) | SDA = GPIO6, SCL = GPIO7, addr 0x3C |
| WS2812B RGB LED   | GPIO8 (onboard)                     |
| Buzzer            | GPIO9                               |
| Button 1          | GPIO2 (active-low, internal pullup) |

## Behaviour

**On boot** the OLED shows a `CALIBRATE?` prompt with a 5-second countdown:

- **Press Button 1** → calibration starts (see below).
- **Do nothing** → boots normally using the last saved calibration (stored in
  flash via `Preferences`). If none exists, it warns and runs with defaults.

**Calibration**

1. *Zero* — samples the empty scale to get the offset (keep the scale clear).
2. *Place weight* — a 30-second countdown appears (large timer + progress bar).
   Put a **10 kg** reference weight on the scale.
3. Pressing Button 1 again ends the countdown early and reads immediately.
4. The scale factor is computed, saved to flash, and a result screen is shown
   (3 beeps on success, 1 on failure if the reading is invalid).

**Normal operation**

- OLED shows the current weight (large) plus a rolling history graph.
- The RGB LED flashes red while no BLE client is connected.
- Each changed reading is streamed over BLE as `"<weight> kg\n"`.

## BLE

The device advertises as **`Progressor_XXXX`** (`XXXX` derived from the chip MAC)
and exposes two services in parallel.

### Tindeq Progressor (works with the Tindeq app)

Emulates the Tindeq Progressor so the official **Tindeq app** can connect.

- Service: `7e4e1701-1ea6-40c9-9dcc-13d34ffead57`
- Data — notify, device → app: `7e4e1702-…`
- Control — write, app → device: `7e4e1703-…`
- **Weight stream** (little-endian): `[0x01][8][float32 kg][uint32 timestamp_µs]`,
  sent only between *start* and *stop*.
- **Commands:** tare `100`, start `101`, stop `102`, get app version `107`,
  get battery `111`. Tare is a soft, non-persisted re-zero (keeps the saved
  calibration factor).

### Nordic UART Service (web app)

- Service: `6E400001-…`, RX `6E400002-…`, TX `6E400003-…`
- Plain-text TX `"<weight> kg\n"`. Still running, so the companion web app works
  with no changes — it picks devices with `acceptAllDevices`, so the device simply
  shows up as `Progressor_XXXX` (instead of `ESP32-C6`) in the Bluetooth chooser.

A companion web app lives at `../esp32-qiyang-webapp`.

## Build & flash

```bash
pio run -t upload      # build + flash
pio device monitor     # serial @ 115200
pio run                # build only
```
