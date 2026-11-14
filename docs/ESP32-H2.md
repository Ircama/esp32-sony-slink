# ESP32-H2 support

The ESP32-H2 (RISC-V, 96 MHz, BLE 5 + IEEE 802.15.4) has **no WiFi radio**. The firmware therefore builds for it without the WiFi, web
portal, TCP console and MQTT layers: only the **USB Serial/JTAG port** and **Bluetooth LE** (Nordic UART service) are available, with the same
text protocol, decoder and S-Link engine as the other chips. The `WIFI`, `WEB`, `APP`, `MQTT` and `HOSTNAME` commands answer
"not available"; BLE is **on by default** (it is the only wireless interface).

| | ESP32-C3 / S3 / C6 | ESP32-H2 |
|---|---|---|
| USB serial, BLE, S-Link engine, decoder | yes | yes |
| Web portal, TCP console, MQTT / Home Assistant | yes | no (no WiFi) |
| Default S-Link GPIO | 8 | **10** (GPIO 2, 3, 8, 9, 25 are strapping pins, 26/27 are USB) |
| Firmware size (this build) | 1.58 MB (C3) | about 0.6 MB (text + data of the ELF) |
| CPU, normal / ECO | 160 MHz max / 40-80 MHz | 96 MHz / **32-48 MHz** |
| BLE advertising, normal / ECO | stack default | stack default / **500-700 ms** |

## Power: what was checked and what was not

Checked in this repository (build and source):
- No WiFi, HTTP, MQTT or lwIP code is linked into the H2 image (symbol table checked: 0 `esp_wifi`, `esp_netif`, `httpd_`, `mqtt_client`
  symbols). On the WiFi chips the radio is the dominant consumer even in modem sleep, so removing it is the largest saving of this chip.
- The ECO profile scales the CPU between **32 and 48 MHz** (dynamic frequency scaling): both values are valid dividers of the H2 clock tree
  (XTAL 32 MHz / 1, PLL 96 MHz / 2, checked in ESP-IDF's `rtc_clk.c`), against 40-80 MHz on the C3.
- In ECO the BLE advertising interval is 500-700 ms instead of the stack default (tens of ms): about 10 times fewer advertising events.
  This also applies to the other chips.
- `NET` / `STATUS` on the H2 prints the CPU frequency in use, the limits and whether power management was configured.
- **No automatic light sleep**, as on the other chips: the S-Link receiver time-stamps the bus edges in a GPIO interrupt, and a light-sleep
  wake-up (hundreds of microseconds) would corrupt the 600/1200/2400 µs bit timing. Turning BLE off (`BLE OFF`) leaves USB only and
  removes the second radio activity.

**Not measured:** no ESP32-H2 board was available, so the actual current drawn has not been measured. Measure it with a USB power meter or
a series shunt (the same way as on the other chips) before quoting figures; the points above are the reasons to expect a lower consumption,
not measurements.

## Build

PlatformIO's `espressif32` platform (8.0.0) does not know the H2 (its builder hard-codes the C3 and C6 for RISC-V), so `pio run -e esp32h2`
fails at the compiler name. The environment and the board file (`boards/esp32-h2-devkitm-1.json`) are in the repository for the day the
platform supports it. Meanwhile build with ESP-IDF directly (6.1):

```
idf.py set-target esp32h2
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build flash
```

`sdkconfig.defaults.esp32h2` is read automatically. This repository's build was done with CMake + Ninja on the toolchain installed by
PlatformIO: the application (`esp32c3_slink.elf`) compiles and links; the final `.bin` packaging step was not run there (the `esptool` module
was missing from that Python environment), so **the image has not been flashed**.

## Wiring

Same as the other chips (see [WIRING.md](WIRING.md)): GPIO 10 as the MOSFET source (or direct, see there), jack tip on the drain, GND to the sleeve.
