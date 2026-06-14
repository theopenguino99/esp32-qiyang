<div align="center">

<img src="assets/banner.svg" alt="crimp-ER — ESP32-C6 Firmware" width="640" />

**On-device firmware for the crimp-ER hangboard scale — ESP32-C6, HX711 load cell, OLED readout &amp; live force streaming over BLE.**

</div>

---

# ESP32-C6 Smart Load Cell

Firmware for an ESP32-C6 based digital scale. Reads an HX711 load cell, shows live
weight on a 1.3" OLED, streams readings over BLE, and supports on-device,
two-button calibration with a selectable reference weight.

## Hardware

| Peripheral        | Connection                          |
|-------------------|-------------------------------------|
| HX711 load cell   | DT = GPIO11, SCK = GPIO10           |
| SH1106 OLED (I2C) | SDA = GPIO6, SCL = GPIO7, addr 0x3C |
| WS2812B RGB LED   | GPIO8 (onboard)                     |
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

Two buttons drive the on-device UI: **Button 1 = change/skip**, **Button 2 =
OK/confirm**.

**On boot** the OLED shows a `CALIBRATE?` prompt and waits (no time limit) for a
choice:

- **Button 2** → run calibration (see below).
- **Button 1** → skip. If a calibration is saved, the device re-zeros the empty
  scale in memory (a boot tare that cancels power-on drift while keeping the saved
  scale factor); if none is saved, it warns and runs with defaults.

**Calibration**

1. *Pick the weight* — Button 1 cycles the reference-weight presets
   (1.25, 2.5, 5, 8, 10, 15, 20, 25 kg), Button 2 confirms. It starts at your
   last-used weight.
2. *Zero* — keep the scale empty; it samples the offset.
3. *Place weight* — a 30-second countdown appears (large timer + progress bar).
   Put the selected weight on the scale; Button 2 ends the countdown early.
4. The scale factor is computed and saved to flash (offset, scale, and the chosen
   weight), then a result screen shows (3 beeps on success, 1 on failure if the
   reading is invalid).

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
