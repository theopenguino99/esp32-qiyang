<div align="center">

<img src="assets/banner.svg" alt="crimp-ER — ESP32-C6 Firmware" width="640" />

**On-device firmware for the crimp-ER hangboard scale — ESP32-C6, HX711 load cell, OLED readout &amp; live force streaming over BLE.**

</div>

---

ESP32-C6 (DevKitC-1) firmware for a smart hangboard scale, built with PlatformIO
on the Arduino framework. Pairs with the
[crimp-ER web app](https://github.com/theopenguino99/esp32-qiyang-webapp) for
training, testing and rehab protocols.

> **Active development.** `main` currently holds a minimal bring-up sketch.
> Feature work lives on dedicated branches and is documented there:
>
> - `inbuilt-calibration` — HX711 load cell, OLED weight readout, button-driven
>   calibration, and BLE force streaming.
> - `oled-display` — SH1106 OLED display and reading history.
> - `bluetooth` — BLE UART (Nordic UART Service) bring-up.

## Build & flash

```bash
pio run                # build only
pio run -t upload      # build + flash over USB
pio device monitor     # serial @ 115200
```
