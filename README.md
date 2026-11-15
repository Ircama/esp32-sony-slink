# ESP32 Sony S-Link / Control-A1 Controller

A modern web application for comfortable, intuitive control of a **Sony STR-DA50ES** amplifier, designed to take the place of the **RM-TP501** remote. The whole web portal runs on an **ESP32-S3** that connects to the amplifier's **S-Link / Control-A1** jack with a single wire and one small N-channel MOSFET (see [`docs/WIRING.md`](docs/WIRING.md)).

Beyond the amplifier, the firmware (written in C with ESP-IDF) implements the S-Link / Control-A1 protocol broadly: it decodes bus traffic in real time and handles the other device types on the bus, such as the tuner, CD players and changers and MiniDisc recorders (the Jukebox tab; those are implemented from published documentation and have not been tried on real units). Everything is exposed through several interfaces:

- **Web Portal:** A modern, embedded web app for full device control, inspired by the Sony RM-TP501 remote (reference remotes: RM-TP501, RM-TP501E, RM-TP502, RM-TP503, RM-TP504 and RM-TP1).
- **REST & WebSocket API:** For custom integrations and real-time state updates.
- **MQTT:** Native integration with automatic Home Assistant discovery.
- **Bluetooth LE:** Wireless control via the Nordic UART Service.
- **Raw TCP Console:** Direct text-based command interface over the network.
- **USB Serial:** Direct local connection for logging and commands.

**Supported hardware:** ESP32-C3, ESP32-S3, ESP32-H2, ESP32-C6, ESP32-C5.

## Features

**S-Link / Control-A1 bus**
- Sends and receives S-Link frames on a single GPIO (open-drain; one N-channel MOSFET or direct wiring, see [`docs/WIRING.md`](docs/WIRING.md)): RMT-based transmitter, edge-timing receiver, line-free check, frame repeats and gap, LED that mirrors the traffic, loopback and line-level tests.
- **Real-time decoding** of every frame (amplifier, tuner, surround processor, CD players and changers, MiniDisc recorders), with a text comment on received *and* sent lines (`DECODE ON|OFF`).
- Catalogue of **225 documented commands** with parameter widgets, live frame preview and confirmation for destructive ones; every command and reply is mapped in [`docs/WEB_COVERAGE.md`](docs/WEB_COVERAGE.md).
- Amplifier: power, source, volume and mute, 5.1 input, recording source, sound fields, 3-band front/rear equaliser, surround levels, speaker configuration. Volume with a motorised-knob model (three scales, calibration, configurable safety limit).
- Tuner: FM/AM tuning, seek, RDS name, signal level, FM scan with station table, up to 30 presets kept by the bridge (shared by every client), read-only view of the amplifier's own presets.
- Jukebox: CD changers and MD recorders are detected on the bus and get their own tab (transport, disc/track lists, memos, CD Text, MD titles). Written from published documentation, **not tried on real units**.

**Network**
- **Web portal** embedded in the firmware: one page, light/dark theme, phone and desktop layouts, five languages (it/en/fr/de/es), installable as a home-screen app, with a demo mode that needs no amplifier ([GitHub Pages demo](https://ircama.github.io/esp32-sony-slink/)).
- **mDNS (Bonjour / Avahi)**: the device announces itself as `<hostname>.local` (default `slink`, set with `HOSTNAME <name>`), so the portal opens at `http://slink.local/` as well as at its IP address. It also advertises the services `_http._tcp` (port 80, while the portal is on) and `_slink._tcp` (port 2323, while the TCP console is on), which service browsers and Home Assistant can discover. mDNS is multicast on the local network and does not need the router to do anything, as long as the router or access point does not block multicast or isolate WiFi clients; it works out of the box on Windows 10/11, macOS, iOS and Linux with Avahi, while some Android versions do not resolve `.local` names in the browser. The host name is also sent to the router with the DHCP request, so many routers resolve it by themselves (for example `slink.fritz.box` on a FRITZ!Box); when neither works, use the IP address shown by `NET`.
- **HTTPS (optional)** with a private certificate authority made by the device itself (no certificate to buy or to copy around): install its certificate once on the phone and the portal becomes a trusted secure origin, installable as a real app.
- **REST and WebSocket API** (`/ws`, `/api/state`, `/api/cmd`, ...) for integrations and real-time state.
- **Shared state across clients**: the firmware keeps the one state of the system (rebuilt from the frames on the bus) and pushes every `RX`/`TX` line and every state change over the WebSocket to **all** connected browsers, so several pages, phones, MQTT and the consoles always show the same thing, even when a command comes from another client or the remote. Presets, stations and interface preferences live in the device, not in the browser. Up to 6 simultaneous connections.
- **MQTT** client with automatic **Home Assistant** discovery (power, mute, input, sound field, volume, FM frequency, station name, signal level).
- **Raw TCP console** (port 2323), same text protocol as USB.
- **Slink-e protocol on TCP** (port 2325) so programs written for the Nirvis Slink-e interface can use the bridge; read-only by default.
- **Bluetooth LE** UART (Nordic UART Service), **USB serial** (USB Serial/JTAG) always available for logging and commands.
- WiFi client (WPA2/WPA3) configured over USB or BLE; **power saving** profile (modem sleep, 20 MHz channel, reduced TX power, Target Wake Time on WiFi 6 chips) on by default.

**Maintenance**
- **Over-the-air update** with two firmware slots, automatic rollback, optional upload password, pull from GitHub releases or push from a computer; both off or user-triggered by default (see [Firmware update over WiFi](#firmware-update-over-wifi-ota)).
- Settings kept in flash (NVS); **backup and restore** of the amplifier configuration, the bridge's data and the browser preferences in one JSON file.
- Statistics page: uptime, reset reason, chip temperature, memory, WiFi quality, clients, S-Link counters.
- Every interface is **opt-in**: web, TCP console, MQTT, Slink-e, BLE, push update and sending from Slink-e are off until enabled; the network interfaces have no authentication, so use them only on a trusted network (see *Security* below).
- Builds for ESP32-C3, S3, C6, C5 and H2 (the H2 has no WiFi: USB and BLE only); GitHub Actions builds every chip and publishes releases.

## Build and flash

You do not need to build anything to try the firmware: every [release](https://github.com/Ircama/esp32-sony-slink/releases) has a ready `slink-<chip>-factory.bin` (bootloader, partition table and application in one file) that you flash at address `0x0` with `esptool` (`pip install esptool`, then `esptool --chip esp32s3 write_flash 0x0 slink-esp32s3-factory.bin`). To build it yourself, choose one of the two toolchains below; both need **git**, **Python 3.10 or newer** and an internet connection for the first build (toolchains and the managed components `mdns` and `mqtt` are downloaded).

| Chip | Board used / typical | PlatformIO environment | Status |
|---|---|---|---|
| ESP32-S3 | S3 Super Mini or DevKitC, 4 MB flash | `esp32s3` | built and run on the real amplifier |
| ESP32-C3 | C3 Super Mini | `esp32c3` | builds |
| ESP32-C6 | WiFi 6 | `esp32c6` | builds, not tried on a board yet |
| ESP32-C5 | WiFi 6, 2.4 + 5 GHz | – (ESP-IDF only) | builds in CI, not tried |
| ESP32-H2 | no WiFi, USB + BLE only | – (ESP-IDF only) | builds in CI, see [`docs/ESP32-H2.md`](docs/ESP32-H2.md) |

**Option A: PlatformIO** (simplest). Install either the *PlatformIO IDE* extension for VS Code, or the command line tool with `pip install platformio` (the `pio` command; on Windows it may be in `%USERPROFILE%\.platformio\penv\Scripts`). The `espressif32` platform, the ESP-IDF framework and the compilers are fetched automatically on the first build (several GB, about 15 minutes; later builds take about a minute). Then, from the project folder:

```
pio run -e esp32s3                  # build
pio run -e esp32s3 -t upload        # build and flash over USB (add --upload-port COM3 or /dev/ttyACM0 if several boards are connected)
pio device monitor                  # serial console (115200 is only nominal: USB Serial/JTAG ignores the baud rate)
```

Replace `esp32s3` with the environment of your chip (`esp32c3`, `esp32c6`; they are defined in `platformio.ini`).

**Option B: ESP-IDF 6.x** (needed for ESP32-C5 and ESP32-H2, which PlatformIO's platform does not support yet; also fine for the other chips). Install ESP-IDF with the [Espressif installer or VS Code extension](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (on Linux/macOS: clone ESP-IDF, run `./install.sh`, then `. ./export.sh`; the `espressif/idf` Docker image works too), open an ESP-IDF shell and run:

```
python tools/gen_web.py                                   # embeds main/web/ into the firmware
idf.py set-target esp32s3                                 # or esp32c3 / esp32c5 / esp32c6 / esp32h2
idf.py menuconfig                                         # optional: menu "S-Link interface" (GPIO, default device ID, LED GPIO)
idf.py -p COM3 flash monitor                              # /dev/ttyACM0 on Linux
```

`tools/ci_build.sh <chip>` does the same without the menu and is what the GitHub workflow runs (`.github/workflows/firmware.yml`).

**First flash over USB.** The ESP32-S3 / C3 / C6 / H2 boards used here have a native USB port: no driver is needed on current Windows, macOS and Linux. If the board does not show up as a serial port, hold the **BOOT** button while plugging it in (download mode). The first installation, and any change of partition table, must go through USB; after that the firmware can be updated over WiFi (see [OTA](#firmware-update-over-wifi-ota)).

Status: built and flashed on an **ESP32-S3** board (S3FH4R2); CLI, TX, BLINK/LEVEL
verified on the board. **Tried with a real amplifier: Sony STR-DA50ES** (it powers on with `C0 2E` and answers `C8 ..`; ID 0xC0).

Open the port (any baud rate) with PuTTY / `screen /dev/ttyACM0`; the terminal must send
CR or LF at the end of a line. Note: opening the port may reset the board (DTR/RTS): wait ~1 s.

## Text protocol

Host → ESP (one line per command, case-insensitive):

| Command | Effect |
|---|---|
| `70 14` | raw frame (hex, max 24 bytes): device, command, parameters |
| `VOLUP` / `VOLDN` | volume ± (cmd 14/15) |
| `MUTE` / `UNMUTE` | cmd 06/07 |
| `PWRON` / `PWROFF` | cmd 2E/2F |
| `ALLOFF` | power off every Sony unit on the bus (CD 1–3, MD, tuner, then the amplifier): the RM-TP501's SYSTEM/STANDBY “ALL OFF” |
| `VOL UP`/`DOWN [n]`, `VOL MIN`, `VOL MAX`, `VOL SET <0-100>` (decimals allowed: `VOL SET 2.4`), `VOL POS [steps]` | motorised volume knob (travel 420 steps up / 445 down, no read-back): the firmware keeps an **estimate** of the position and, to reach an end, sends only the steps needed + 10 (it never spins idly). `VOL SET 50` = half travel; 1 % is ~4 steps, which is why decimals are accepted (the pot is logarithmic: the first steps from the minimum are already audible, and in the portal the dial is non-linear so they can be adjusted). After a reboot the position is unknown (`VOL POS <steps>` declares it without moving). `GAP <ms>` = pace (30 ms/step) |
| `VOL QMIN [limit%]` | quick calibration: assumes the volume is within the safety limit and only goes down as far as needed (`VOL MIN` is the full one) |
| `VOLCFG [up down margin]` | steps of the knob travel (default 420 / 445 / 10), saved; editable in Settings › Volume knob |
| `PAD [ODPU\|OD\|PP]` | electrical mode of the bus pin: open-drain with the internal pull-up (default), open-drain without it, or push-pull (3.3 V buses only); saved. Also in Settings › S-Link diagnostics |
| `CFG [PASS]` / `CFG SET <key> <value>` / `CFG APPLY` | the settings kept in the ESP32 flash (network, MQTT, interfaces, knob travel, titles, pin mode): list (passwords only with `PASS`), change one, apply. Used by the web backup/restore |
| `STATION CLEAR\|ADD <10 kHz> ["name"]\|SAVE`, `FAV CLEAR\|PUT FM\|AM <f> ["name"]` | rebuild the FM station table / the favourites (configuration restore) |
| `UITITLE [APP\|PAGE "text"]` | header title / page title (tabs, bookmarks) of the web interface, saved |
| `INPUT <name\|hex>` | cmd 50 (TUNER PHONO CD DAT MD TAPE DIGITAL VIDEO1-4 LD SAT TV DVD) |
| `AMPNAME` | cmd 6A |
| `TUNER FM\|AM\|SCAN+\|SCAN-\|PRESET+\|PRESET-\|MONO\|STEREO` | tuner (dev C1). The tuner has no band command: `TUNER FM`/`AM` tunes the last frequency used in that band |
| `TUNE FM 101.3` / `TUNE AM 810` | direct tuning |
| `PRESET <bank 1-3> <n 0-9>` | recall a preset station of the amplifier |
| `FAV` / `FAV ADD [FM <MHz>\|AM <kHz>]` / `FAV DEL <n>` / `FAV <n>` | **bridge presets** (up to 30, kept in flash, shared by every client): list, store the tuned station, remove, recall. The tuner's own 30 presets cannot be written over S-Link (front panel only), so the bridge has its own; also `GET /api/favs` |
| `DEV <hex>` | default device ID for the mnemonics (default C0, verified on the STR-DA50ES; 70 for the DA1000ES class, not verified); without an argument it prints the current one |
| `REPEAT <n>` | send each frame n times (1–5) |
| `BLINK HIGH 500 LOW 500 60` | tester check: pin high for 500 ms, then low for 500 ms, for 60 cycles (without a number: until `BLINK OFF`); also `BLINK LOW 200 HIGH 800 10` |
| `BLINK OFF` | stop blinking at once and release the line |
| `LEVEL HIGH` / `LEVEL LOW` / `LEVEL OFF` | hold the pin high / low; `OFF` releases it |
| `LED 8` / `LED 8 INV` / `LED OFF` | mirror the S-Link traffic (in and out) on the LED of the given GPIO (on = line low); `INV` for an active-low LED. Default from menuconfig (GPIO48 on the S3, none on the other chips) |
| `SLINK 5` | choose the GPIO wired to S-Link (single-pin builds only); `SLINK` alone shows the one in use. Valid until reboot (menuconfig default: GPIO8). Rejects invalid pins or the LED's pin |
| `ECHO ON` / `ECHO OFF` | echo typed characters (backspace handled). Default OFF |
| `STATUS` | ask for the state of amplifier, tuner and surround (decoded answers) |
| `EQCURVE` | read the equaliser (front and rear) and draw its curve, frequency (log axis) against dB, on the terminal |
| `NAME` | RDS name of the tuned station (`C1 48 BB FF`): it has to be requested, the amplifier does not send it by itself |
| `PRESETS [bank]` | read frequency (`C1 4A BB NN` → `C9 4A BB NN 00 HH LL`) and name (`C1 48 BB NN`) of the 10 presets of one bank, or of all 3; 87.50 MHz = preset never set |
| `MODEL` | read model/info of amplifier, tuner and surround (`22`) plus name (`6A`) and info (`6B`) |
| `SETUP` | replay the remote's download: name, info, input labels and tuner presets |
| `STATIONS` | list the FM stations stored by `FMSCAN` (frequency and RDS name); the table is saved in flash and survives reboots |
| `TUNE <name>` / `TUNE #n` / `TUNE 101.5` | tune by name (exact or part of the name, case-insensitive), by table number or by frequency in MHz. The S-Link protocol has no "by name" command: the firmware searches the `FMSCAN` table. If several stations match (e.g. VIRGIN) it tunes the first and lists them |
| `FMSCAN` | automatic FM scan with a station list (needs the TUNER input) |
| `EQ` / `EQ F\|R BASS\|MID\|TREBLE GAIN <dB>` / `... FREQ <Hz>` | read or set the 3-band front/rear equaliser |
| `SF ON\|OFF\|<1-29>` | sound field; `REAR`, `SUB`, `BAL`, `DRC`, `LFE <value>` surround levels |
| `INPUTS` | ask the amplifier for the label of every input (answers decoded like `RX`). Measured list and formats of all frames (inputs, radio, EQ, sound field): [`docs/INPUTS_AND_FRAMES.md`](docs/INPUTS_AND_FRAMES.md) |
| `DECODE ON` / `DECODE OFF` | annotate `RX` lines as AMP / TUNER / SURROUND messages (e.g. `RX C8 2E ; AMP reply: power ON`). Default ON |
| `LOOPBACK ON` / `LOOPBACK OFF` | also show the transmitted frames as `RX`: checks that the line is healthy |
| `LINE` | line level: 1 = free/high, 0 = low |
| `BLE ON\|OFF` | enable/disable **UART over BLE** (Nordic UART Service). **Saved in flash**: it persists after a reboot. Without an argument it shows the state |
| `WEB ON\|OFF` | enable/disable the **web portal** (saved) |
| `APP ON\|OFF` | enable/disable the **WiFi application interface**: TCP console (port 2323) and MQTT bridge with Home Assistant discovery (saved) |
| `SLINKE ON\|OFF` | enable/disable the **Slink-e serial protocol on TCP** (port 2325, see [Slink-e protocol over TCP](#slink-e-protocol-over-tcp)); **off** by default (saved) |
| `HTTPS ON\|OFF\|RESET` | also serve the web portal on **https** (port 443) with a certificate from the device's own CA, see [HTTPS and installing the app](#https-and-installing-the-app); `RESET` deletes the CA and makes a new one (saved) |
| `SLINKETX ON\|OFF` | let a connected Slink-e program **send** to the bus; **off** by default: the connection is then a read-only monitor (saved) |
| `WIFI <ssid> [password]` / `WIFI CLEAR` | WiFi network (saved). With blanks use quotes: `WIFI "My home" "the password"`. WiFi turns on only if `WEB`, `APP` or `SLINKE` is ON |
| `MQTT <mqtt://host[:port]> [user] [password]` / `MQTT CLEAR` | MQTT broker (saved; plain TCP only, no TLS) |
| `HOSTNAME <name>` | host name and mDNS (`<name>.local`, default `slink`) |
| `ECO ON\|OFF` | low-power profile (default **ON**, saved): WiFi in *max modem sleep* (wakes every 3 beacons, ~0.3 s), 20 MHz channel, 11 dBm TX power, CPU 40–80 MHz with automatic scaling. With OFF: minimum modem sleep, 20 dBm, CPU 160/240 MHz. No light sleep: the S-Link receiver needs interrupt latency in the microseconds. On ESP32-C6 (WiFi 6) an individual Target Wake Time agreement (500 ms interval) is also requested when the router supports it; not tried on a real board yet |
| `NET` | state of WiFi, BLE, web and the application interface |
| `HELP` | command list |

ESP → host:

- `RX 70 14 ...` every frame detected on the bus (also those of other devices/answers);
  suffixes `+Nbit` (leftover bits) and `!timing` (durations out of tolerance).
- `TX ...` + `OK` after sending; `ERR ...` on error (`busy` = line never free,
  `line test active` = BLINK/LEVEL running).

## Interfaces: USB, BLE, WiFi, web portal

A single command core (`cli.c`) and several transports: every interface sends text lines identical to those of the
USB port and receives all the output (`RX ..`, `TX ..`, `OK`, `ERR ..`). The settings (`BLE`, `WEB`, `APP`, `SLINKE`, `SLINKETX`, `ECO`, WiFi, MQTT,
host name) are stored in NVS and **persist**.

| Interface | How to use it |
|---|---|
| USB Serial/JTAG | always on, as before |
| **BLE UART** (`BLE ON`) | Nordic UART Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`; the device advertises as `S-Link-XXXX`. Apps: nRF Toolbox, Serial Bluetooth Terminal, Web Bluetooth. It is not authenticated: enable it only when needed |
| **Web portal** (`WEB ON`) | `http://slink.local/` (or the IP: `NET`). Single page, light/dark theme, suited to phone and desktop; WebSocket `/ws` for commands and real-time state. Also REST: `GET /api/state`, `/api/presets`, `/api/stations`, `/api/favs`, `/api/stats` and `POST /api/cmd` (body = one command; the output comes over the WebSocket). A WebSocket client is registered when it sends its first frame |
| **HTTPS** (`HTTPS ON`) | the same portal on `https://slink.local/` (port 443), trusted once the device's CA is installed on the phone / PC: needed to install the portal as an app on Android. See [below](#https-and-installing-the-app) |
| **Jukebox tab** | appears only while a CD changer (`90`/`91`/`92`) or an MD recorder (`B0`) answers the probe `90 22` / `B0 22`: transport, disc and track lists, memos / CD Text / MD titles, groups and artists. Reading and transport are free; every write (memo, MD titles, artist names, erase track, power off) asks for confirmation. Untested on real units: none is attached to the author's bus. Open `?demo&jb` to see it with an emulated changer and recorder (the GitHub Pages demo always shows it) |
| **Backup / restore** | Settings › Backup and restore saves one JSON file with the amplifier's configuration (input names, sound field, EQ, surround levels, speaker set-up, 2nd audio, 5.1; its tuner presets are stored for reference only, S-Link cannot write them), what the ESP32 keeps (settings, FM stations, favourites; passwords only if asked) and this browser's interface preferences; the file can be restored selectively. Volume, input and power state are never touched. Only writes verified on the STR-DA50ES are used (sound field, EQ, surround levels, 5.1); input names (`C0 88`), speaker sizes (`C3 53`) and the second audio source (`C0 52`) do not work on it and stay in the file for reference |
| **Raw TCP** (`APP ON`) | `nc slink.local 2323`: same text protocol as USB (max 2 clients) |
| **Slink-e protocol on TCP** (`SLINKE ON`) | the binary protocol of the Nirvis Slink-e interface on port 2325, for programs written for that device: see [below](#slink-e-protocol-over-tcp) |
| **MQTT / Home Assistant** (`APP ON` + `MQTT ...`) | automatic discovery under `homeassistant/`; JSON state on `slink/<id>/state`, commands (any CLI line) on `slink/<id>/cmd`, availability on `slink/<id>/avail`. Entities: power, mute, input, sound field, volume (estimate), volume ±, FM frequency, station name, signal level |

First start (WiFi is configured over USB or BLE):

```
WIFI "NetworkName" "password"
WEB ON
NET            # shows the address
```

**Security.** The web portal is **HTTP** on port 80 (HTTPS on port 443 is optional, see [HTTPS and installing the app](#https-and-installing-the-app); it encrypts but does not authenticate users), and neither the portal nor the TCP console (2323) nor the Slink-e port (2325) nor BLE has any authentication: use them only on a trusted network and do not forward ports from the router. The only configurable credentials are the WPA2/WPA3 WiFi password and the user/password
of the MQTT broker (`mqtt://` only, no TLS).

**Tab bar on a phone** (Settings › Interface, kept in the device). When the tabs do not fit in portrait: *Compression* (checkbox, on by default) narrows the buttons first; then, according to the two switches (they exclude each other), the bar either **scrolls** sideways, or the last tabs move into an **Altro** (More) menu and are no longer in the bar (e.g. Settings and Help). With neither switch on, the buttons are always compressed. On wide screens the side column is unchanged.

WiFi works only as a client (STA): with BLE active the access point is not stable. WiFi and BLE can be
on together (ESP-IDF software coexistence).

**Web portal.** A single embedded page (light/dark theme, phone and desktop, five languages: it/en/fr/de/es) with these tabs:

- *Listen*: power, source (MD/DAT and SAT/TV are one button each; labels read from the amplifier), badges for what the amplifier reports (tape
  loop, 5.1 input, auto function off, input type, 2nd audio input), volume and mute. The volume knob has **three scales** (linear, logarithmic,
  linear up to the safety limit), 0.1 % resolution, and a mini volume control in the header; the **safety limit** is configurable (default 7.0 of 10,
  down to 1.0) and the fixed-volume buttons are derived from it (never above 75 % of it). The volume is an **estimate** (the motorised knob does not
  report its position): while it is unknown the dial is greyed out and offers the calibration. A second card has the **5.1 input** switch, the
  **recording source** (2nd audio) and *Turn off all units* (the remote's “ALL OFF”).
- *Radio*: FM/AM tuning (scale, ±, seek, direct entry), RDS name, signal, FM scan and station list with Back/Next, **presets kept by the bridge**
  (up to 30; a star stores/removes the tuned station; Back/Next) and the amplifier's own presets (read-only: S-Link cannot write them). The tuner
  answers only while the TUNER source is selected, so with any other source the tab is dimmed and offers a one-click "Select Tuner".
- *Sound*: sound fields by category, equaliser with a draggable curve (front/rear), the eight surround levels (rear, centre, subwoofer, balance,
  effect size, reverb time, wall type, compression, LFE) and the **speaker configuration** read from the amplifier (front/centre/rear size,
  subwoofer present) which can also be changed; controls the amplifier cannot use in the current state are disabled, and the whole sound field is
  disabled while the 5.1 input is on (as on the Sony remote).
- *Statistics*: uptime, last reset reason, chip temperature, memory, WiFi (signal, channel), connected clients, S-Link bus counters, the S-Link
  units (model info, input type, flags) and the power-saving parameters (CPU, modem sleep, TX power, Target Wake Time); read via `GET /api/stats`
  only while the tab is open. Voltage and current are **not** measurable by the chip (an external INA219/INA226 or a USB power meter is needed).
- *Console*: log with a Copy button, command line, quick commands, and the **Documented commands** panel: all 225 S-Link commands found in the public
  documentation, with parameter widgets, a live frame preview and a filter; destructive or audible ones, and those for units not on the bus, ask for confirmation. Every
  S-Link reply is decoded in the log.
- *Settings*: interfaces (BLE, web, WiFi application interface), power saving, WiFi, MQTT, **S-Link diagnostics** (default device, frame repeats and gap,
  RX decoding, loopback, GPIOs, line level), volume safety limit, theme, language.

What the interface covers of the protocol, command by command and reply by reply, is in [`docs/WEB_COVERAGE.md`](docs/WEB_COVERAGE.md)
(generated by `python tools/gen_coverage.py` from the same catalogue). To try the page without an amplifier open `main/web/index.html` in the
browser (or add `?demo`): it uses a simulated amplifier.

The HTML file is embedded in the firmware: `tools/gen_web.py` converts it into `main/web_page.c` before every build
(automatic with PlatformIO, or by hand with `python tools/gen_web.py`). `tools/gen_coverage.py` regenerates the coverage document.

Build notes: ESP-IDF 6.x (`mdns` and `mqtt` are managed components, downloaded by the component manager: internet is needed on the
first build); partition table in `partitions.csv` (two app slots of 1.9 MB for the over-the-air update, flash 4 MB). After changing
`sdkconfig.defaults` delete the generated `sdkconfig.esp32s3` / `sdkconfig.esp32c3` / `sdkconfig.esp32c6` to regenerate them
(they are git-ignored).

## HTTPS and installing the app

On plain `http://` a browser treats the portal as an insecure page: it will not register the service worker and an Android phone creates only a shortcut (it opens in a browser tab, not as an app). `HTTPS ON` (or *Settings › Interfaces › HTTPS web portal*) starts a second server on port **443** with the same pages, API and WebSocket (`wss://`); port 80 stays as it was.

The device makes its own certificate authority the first time (an EC P-256 key generated on the chip, kept in the flash; a few seconds) and signs the server certificate with it:

1. `HTTPS ON`, then open `http://slink.local/ca.crt` (or the *Download the CA certificate* link under the switch) to get `slink-ca.crt`.
2. Install it. **Android**: *Settings › Security (or Biometrics and security) › Other security settings › Install from device storage › CA certificate*. **Windows**: double-click the file › *Install certificate* › *Local machine*, store *Trusted Root Certification Authorities*. **macOS**: add it to *Keychain Access* and set *Always trust*. **iOS**: open the file, install the profile, then *Settings › General › About › Certificate Trust Settings*.
3. Open `https://slink.local/` (or the host name / IP address): the lock is shown, and *Install app* / *Add to home screen* now creates a real app that starts full screen. Uninstall and install again if the app was installed from `http://` (it is another origin).

What the certificate says: the name is the host name (`HOSTNAME`) with the suffixes `.local`, `.fritz.box`, `.lan` and `.home.arpa`, and the **current IPv4 address**. It is made again by itself when the host name or the address changes (so with DHCP the address is better reserved in the router). A bare name such as `https://slink/` does not match: use `slink.local` or the full name.

**Security.** The key of the CA is in the device's flash, not encrypted (the board has no flash encryption by default): whoever has the board in hand could read it. For this reason the CA is limited with *name constraints* to `.local`, `.lan`, `.home.arpa`, `.fritz.box`, `.localdomain`, `.home`, `.internal` and the private IPv4 ranges (10/8, 172.16/12, 192.168/16, 169.254/16): a browser that trusts it will not accept a certificate for any public site signed with that key. The CA is different on every device (its name ends with the last bytes of the WiFi MAC address); install only the certificate of devices you own, and remove it from the phone if you sell the board. `HTTPS RESET` deletes the CA and makes another one. HTTPS protects the traffic from eavesdropping; the portal still has **no login**.

**The portal as a PWA.** The page ships a web app manifest and a service worker, so once the CA is registered on the phone (step 2 above) Chrome treats `https://slink.local/` as a secure origin and offers a real **Progressive Web App**: on Android it installs a *WebAPK* with its own icon, no browser bars and its own entry in the app list, instead of a shortcut that opens a browser tab. Without the CA (or over plain `http://`) the service worker is not registered and Android only makes the shortcut. The manifest is made by the firmware: the app name follows the titles set in *Settings* (`UITITLE PAGE` is the name, `UITITLE APP` the label under the icon); after changing them, Android updates the installed app by itself after a while, or reinstall it.

The **service worker** (`sw.js`) has a small role: it keeps a copy of the application shell (the page, manifest and icons) with a *network first* strategy, so the installed app always opens the version the device serves (also after a firmware update) and, when the device does not answer (switched off, WiFi down), it starts from the cached copy and shows the "disconnected" screen instead of a browser error. It never caches live data: the WebSocket `/ws` and `/api/*` always go to the device, and the amplifier cannot be controlled while it is unreachable. It is also what makes the page installable for the browser. It does not give offline control, background updates or notifications (there is no push server).

Cost: a TLS session needs tens of KB of RAM (the free heap of the ESP32-S3 goes from about 150 KB to about 110-120 KB with HTTPS on; at most 3 simultaneous https connections, 6 http). Tried on the ESP32-S3 only; the other chips build the same code but have less RAM and were not tried.

## Slink-e protocol over TCP

The **Slink-e** (Nirvis Systems) was a PC ↔ S-Link interface with a binary serial protocol ([documented here](http://www.woudy.org/slinke/help/fp/Slinke/lowlevel/lowlevel.htm)).
Programs written for it (the SlinkX control, Girder / MainLobby plug-ins, Linux tools ...) can use this device in its place: they open a TCP connection to
**port 2325** instead of a serial port (on Windows a "COM port redirector" / "serial over network" driver turns a COM port into a TCP connection).

Both functions are **disabled by default**, in Settings › Interfaces or from the console (saved in flash):

| Setting | Command | Default | Effect |
|---|---|---|---|
| Slink-e protocol on TCP | `SLINKE ON\|OFF` | **off** | opens TCP port 2325 (max 2 clients; WiFi must be on). While off the port is closed |
| Slink-e: allow sending to the bus | `SLINKETX ON\|OFF` | **off** | without it a connection only **receives** (read-only monitor): messages sent by the program are discarded. Turn it on to let the program control the system |

There is no authentication (like the text console): use it only on a trusted network. The USB port cannot carry this protocol: it is the text console
(USB Serial/JTAG), already used by the commands described above, so the binary protocol is offered over TCP only.

**What is implemented** (S-Link port 0 is the bus of this device):

| Slink-e | Bytes (hex) | Here |
|---|---|---|
| Port send | `0L b1 .. bL` (L = 1..30 bytes) | collected, sent as one frame (up to 24 bytes) at the *port send end*; the line must be free (`TX ... ; Slink-e` shows in the console) |
| Port send end | `00` | sends the frame; no message ⇒ nothing happens |
| Port receive / end | `0L b1 .. bL` then `00` | every frame seen on the bus, one *port receive* + one *port receive end* |
| Receive error | `1F 80` | frame with a timing error or leftover bits |
| Transmission timeout | `1F 81` | frame longer than 24 bytes, a message not finished within 2 s, or the line never idle: **critical**, resume state |
| Disable / enable reception | `1F 02` / `1F 03` (port 0), `FF 02` / `FF 03` (all ports) | answered with the same two bytes; disabling does not stop sending |
| Get version | `FF 0B` | `FF 0B 20` (2.0) |
| Get serial number | `FF 0C` | `FF 0C` + 8 bytes (`00 00` + the WiFi MAC address) |
| Save / load defaults | `FF 0E` / `FF 0F` | confirmed with the same bytes (nothing to save) |
| Resume | `FF AA` | leaves the resume state |
| Reset | `FF FF` | back to the starting state of the connection (no answer) |
| Serial-port commands | set baud `DF 08 B` (B = 0..4), get baud `DF 09`, send to serial port `C L ...` | the baud rate is stored and reported (the TCP link has none); bytes sent to the serial port are echoed back, as on the original |
| Illegal command | `<port> FF` | any command that does not exist here: critical, resume state (everything is ignored until `FF AA`) |

**Not implemented:** the IR, parallel and the three extra S-Link ports of the original (this device has a single bus; messages to ports 1–3 are ignored)
and the **S-Link bit mode** (`.. 1F 04 B data`, sending a number of bits that is not a multiple of 8: the hardware here sends whole bytes, the command is accepted and ignored).
Frames are limited to 24 bytes by the bus driver.

Example (Python): ask the amplifier for its status (`C0 0F`) and print what arrives. The same bytes in `SLINKETX ON` mode:

```python
import socket
s = socket.create_connection(("slink.local", 2325))
s.sendall(bytes([0x02, 0xC0, 0x0F, 0x00]))      # port send of 2 bytes (C0 0F), then port send end
while True:
    print(s.recv(64).hex(" "))                    # e.g. 07 c8 70 16 16 21 ff 00   (7 bytes of a frame, then the end marker)
```

The protocol was written from the published documentation only. It was exercised on a real ESP32-S3 with a script (version, serial number, enable/disable, baud, illegal command and resume,
send with and without `SLINKETX`, a status request answered by the amplifier, too-long and unfinished messages), but not with a real Slink-e program (none was available).

## Firmware update over WiFi (OTA)

Every WiFi chip (C3, C5, C6, S3; the H2 has none) keeps two firmware slots, so it can be updated without the USB cable, and goes back to
the previous firmware by itself when the new one does not work. **The first installation, and the one that introduces this partition table,
must be done over USB**; the settings (WiFi, stations, ...) are kept.

**Nothing is installed unattended.** The device never checks for or downloads updates by itself. Two switches (*Settings > Firmware update*, or the CLI) decide what is allowed:

| Switch | CLI | Default | Meaning |
|---|---|---|---|
| Push from outside | `OTA PUSH ON\|OFF` | **off** | accept a `.bin` sent to `POST /api/ota` (`tools/ota_push.py`, portal upload); while off the request is refused with `403` |
| Download (pull) | `OTA PULL ON\|OFF` | on | allow `OTA UPDATE` / the *Check and install* button to download; it only ever runs when the user asks |

- **From the device (pull)**: *Settings > Firmware update > Check and install*, or the CLI command `OTA UPDATE`. The device downloads
  `slink-<chip>.bin` from the latest GitHub release of this repository (https, certificates checked) and restarts. Another source can be
  set in the same card or with `OTA URL https://example.org/slink-esp32s3.bin` (`OTA URL DEFAULT` goes back to GitHub; http:// works for a
  server on the local network). An optional access token (`OTA TOKEN <text>`, sent as `Authorization: Bearer`) is meant for a server of your own.
  `OTA UPDATE FORCE` installs even when the version is the same.
- **From a computer (push)**, after `OTA PUSH ON`: `python tools/ota_push.py slink.local .pio/build/esp32s3/firmware.bin` (or `--build esp32s3` to build and send;
  `--password` if one is set). The *Settings* card can also send a `.bin` chosen in the browser.
- **Upload password (optional, off by default)**: `OTA PASS <text>` or the Settings card; `OTA PASS OFF` removes it. When set, the device gives a
  one-time nonce (`GET /api/ota`) and the sender proves the password with `sha256("<nonce>:<password>")` (header `X-OTA-Auth`), so the password
  never travels on the network. **Without a password anyone on the local network can upload a firmware** (like the rest of the interface, which has no
  login): the password protects the upload endpoint only, not the CLI/portal commands that change the OTA settings.
- **Rollback**: a new firmware is *on trial* until it has run 30 s (and, if the WiFi is in use, got an IP address); if it crashes or hangs before,
  the bootloader starts the previous one. `OTA ROLLBACK` (or the button in Settings) goes back by hand. `OTA` shows the state of both slots.
- Only an image with the right chip and the right project name is accepted; an upload is written to the unused slot and checked before it is selected.

**Releases**: `.github/workflows/firmware.yml` builds C3, C5, C6, H2 and S3 with ESP-IDF 6.1 on every push (files kept as workflow artifacts);
pushing a tag `v*` also publishes a release with `slink-<chip>.bin` (what OTA installs), `slink-<chip>-factory.bin` (bootloader + partitions +
application, to flash over USB at address 0: `esptool.py --chip esp32s3 write_flash 0x0 slink-esp32s3-factory.bin`) and `SHA256SUMS.txt`.
The repository used by `OTA UPDATE` is `SLINK_OTA_GITHUB_REPO` (menuconfig, *S-Link interface*).

## Tester check (finding the right pin)

`BLINK HIGH 500 LOW 500 60` or `LEVEL LOW`. The S-Link pin is open-drain:
"LOW" = 0 V; "HIGH" = pin released, so the voltage depends on the pull-up
(internal ESP ~45 kΩ → ~3.3 V unloaded, or the amplifier's voltage if connected).
A low-impedance meter may read less than 3.3 V: that is normal. During a test
sending S-Link commands is blocked. The LED (`LED` command) also shows incoming and
outgoing frames.

## How it works

- **RX** (`slink.c`): ISR on both edges of the S-Link pin (GPIO8) (same pin as TX, open-drain), measures
  the duration of the low "marks": ≥1800 µs sync, >900 µs bit 1, otherwise 0; frame closed by
  silence >2 ms; MSB first.
- **TX**: **RMT** peripheral at 1 MHz; line-free check ≥3 ms and distance ≥30 ms from the
  previous frame. The echo of its own TX is ignored.
- ESP-IDF console disabled (`sdkconfig.defaults`): only the protocol goes over the COM port.
- Timing and codes from Ircama/Sony_SLink and boehmel.de/slink.htm.

## Known limits
- No active collision detection while transmitting (only "line ready" beforehand).
- The amplifier's answers arrive as asynchronous `RX` lines.
- The tuner answers only while the TUNER input is selected, and has no band-change or direct-preset-write command.
- Web portal, TCP console, Slink-e port and BLE are unauthenticated; the portal is HTTP, or HTTPS with the device's own CA (encrypted, still no login).

## Documentation
- [`docs/SOURCES.md`](docs/SOURCES.md): the sources read (also the Slink-e documentation used for the TCP protocol).
- [`docs/WIRING.md`](docs/WIRING.md): hookup (MOSFET or direct), first test.
- [`docs/ESP32-H2.md`](docs/ESP32-H2.md): the ESP32-H2 (no WiFi: USB serial + BLE only), what was checked about its power use, how to build it.
- [`docs/INPUTS_AND_FRAMES.md`](docs/INPUTS_AND_FRAMES.md): inputs, format of all measured frames (radio, EQ, sound field, volume, power, mute), FM station list, calibration of the knob.
- [`docs/RM-TP501.md`](docs/RM-TP501.md): the Sony RM-TP501 touch remote (the reference remote; RM-TP501E, RM-TP502, RM-TP503, RM-TP504 and RM-TP1 are the other reference remotes) (two-way infrared link, same messages as Control-A1) function by function against this bridge, the other sites on the protocol, and the decoding added from them.
- [`docs/WEB_COVERAGE.md`](docs/WEB_COVERAGE.md): what the web interface covers: every documented command, every firmware command, every S-Link reply (generated).
- [`docs/CODES_TO_TRY.md`](docs/CODES_TO_TRY.md): codes found online and not yet tried (already decoded by the firmware), with the sources.
- News: `C1 0E` = FM signal level; sound fields 0–29 are shown with their name (from the hifi-remote table, to be confirmed by ear).

## License

EUPL-1.2 License - See [LICENSE](LICENSE) for details. (The ESP-IDF framework and the managed components `espressif/mdns`
and `espressif/mqtt` that the firmware builds on keep their own licences.)
