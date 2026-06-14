<div align="center">

<img src="assets/banner.svg" alt="crimp-ER — ESP32-C6 Firmware" width="640" />

**On-device firmware for the crimp-ER hangboard scale — ESP32-C6, HX711 load cell, OLED readout &amp; live force streaming over BLE.**

</div>

---

# ESP32-C6 Smart Load Cell

Firmware for an ESP32-C6 based digital scale. Reads an HX711 load cell, shows live
weight on a 1.3" OLED, streams readings over BLE, and supports on-device
calibration with a push button.

## Hardware

| Peripheral        | Connection                          |
|-------------------|-------------------------------------|
| HX711 load cell   | DT = GPIO11, SCK = GPIO10           |
| SH1106 OLED (I2C) | SDA = GPIO6, SCL = GPIO7, addr 0x3C |
| WS2812B RGB LED   | GPIO8 (onboard)                     |
| Buzzer            | GPIO9                               |
| Button 1          | GPIO2 (active-low, internal pullup) |
| Button 2          | GPIO3 (active-low, internal pullup) |

All hardware design files live in [`hardware/`](hardware/) — KiCad project,
gerbers (`hardware/fab/`), and the 3D-printable enclosure.

### PCB

Custom 2-layer board designed in KiCad ([`hardware/pcb_Crimp-ER.kicad_pcb`](hardware/pcb_Crimp-ER.kicad_pcb)).

| Front | Back | Actual|
|:-----:|:----:|:-----:|
| <img src="hardware/renders/pcb_front.png" alt="PCB front" width="340"> | <img src="hardware/renders/pcb_back.png" alt="PCB back" width="340"> | <img src="hardware/renders/jlcpcb.jpeg" alt="PCB actual" width="340"> |

### Enclosure

Two-part 3D-printed case. Models (STL + STEP) live in
[`hardware/crimp-ER covers/`](hardware/crimp-ER%20covers/).

|Full Assembly| Top cover | Bottom cover |
|:---------:|:------------:|:-:|
|<img src="hardware/renders/assembly v16.png" alt="Full Assembly" width="340">| <img src="hardware/renders/top cover_fusion.png" alt="Top cover" width="340"> | <img src="hardware/renders/bottom cover_fusion.png" alt="Bottom cover" width="340"> |

> Print-ready downloads:
> [top STL](hardware/crimp-ER%20covers/top_cover.stl) ·
> [bottom STL](hardware/crimp-ER%20covers/bottom_cover.stl).

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

## BLE (Nordic UART Service)

- Device name: `ESP32-C6`
- Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX (phone → ESP32): `6E400002-…`
- TX (ESP32 → phone): `6E400003-…`

A companion web app lives at `../esp32-qiyang-webapp`.

## Build & flash

```bash
pio run -t upload      # build + flash
pio device monitor     # serial @ 115200
pio run                # build only
```
