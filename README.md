# ESP32 Sony S-Link / Control-A1 Controller

Control and monitor Sony Hi-Fi gear (such as the STR-DA50ES) from your phone, PC, or smart home using an ESP32 and a single wire connected to the S-Link jack.

Written in C with ESP-IDF, this firmware interfaces with the Sony S-Link / Control-A1 bus, decodes bus traffic in real time, and exposes your devices through multiple interfaces:

- **Web Portal:** A modern, embedded web app for full device control, inspired by the Sony RM-TP501 remote (reference remotes: RM-TP501, RM-TP501E, RM-TP502, RM-TP503, RM-TP504 and RM-TP1).
- **REST & WebSocket API:** For custom integrations and real-time state updates.
- **MQTT:** Native integration with automatic Home Assistant discovery.
- **Bluetooth LE:** Wireless control via the Nordic UART Service.
- **Raw TCP Console:** Direct text-based command interface over the network.
- **USB Serial:** Direct local connection for logging and commands.

**Supported hardware:** ESP32-C3, ESP32-S3, ESP32-H2, ESP32-C6, ESP32-C5.

## Build and flash

PlatformIO (latest `espressif32` platform, Python ≥ 3.10):

```
pio run -e esp32s3 -t upload --upload-port COM3     # ESP32-S3 (4 MB flash)
pio run -e esp32c3 -t upload --upload-port COMx     # ESP32-C3 Super Mini
pio run -e esp32c6                                   # ESP32-C6 (WiFi 6): builds, not tried on a board yet
# ESP32-H2 (no WiFi, USB + BLE only) and ESP32-C5 (WiFi 6, 2.4 + 5 GHz): build with ESP-IDF (PlatformIO's platform does not support them yet),
# see docs/en/ESP32-H2.md; the GitHub workflow builds both (.github/workflows/firmware.yml)
```

or ESP-IDF: `idf.py set-target esp32c3 && idf.py menuconfig && idf.py build flash`
(menu *S-Link interface*: GPIO, default device ID, LED GPIO).

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
| `INPUTS` | ask the amplifier for the label of every input (answers decoded like `RX`). Measured list and formats of all frames (inputs, radio, EQ, sound field): [`docs/en/INPUTS_AND_FRAMES.md`](docs/en/INPUTS_AND_FRAMES.md) |
| `DECODE ON` / `DECODE OFF` | annotate `RX` lines as AMP / TUNER / SURROUND messages (e.g. `RX C8 2E ; AMP reply: power ON`). Default ON |
| `LOOPBACK ON` / `LOOPBACK OFF` | also show the transmitted frames as `RX`: checks that the line is healthy |
| `LINE` | line level: 1 = free/high, 0 = low |
| `BLE ON\|OFF` | enable/disable **UART over BLE** (Nordic UART Service). **Saved in flash**: it persists after a reboot. Without an argument it shows the state |
| `WEB ON\|OFF` | enable/disable the **web portal** (saved) |
| `APP ON\|OFF` | enable/disable the **WiFi application interface**: TCP console (port 2323) and MQTT bridge with Home Assistant discovery (saved) |
| `WIFI <ssid> [password]` / `WIFI CLEAR` | WiFi network (saved). With blanks use quotes: `WIFI "My home" "the password"`. WiFi turns on only if `WEB` or `APP` is ON |
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
USB port and receives all the output (`RX ..`, `TX ..`, `OK`, `ERR ..`). The settings (`BLE`, `WEB`, `APP`, `ECO`, WiFi, MQTT,
host name) are stored in NVS and **persist**.

| Interface | How to use it |
|---|---|
| USB Serial/JTAG | always on, as before |
| **BLE UART** (`BLE ON`) | Nordic UART Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`; the device advertises as `S-Link-XXXX`. Apps: nRF Toolbox, Serial Bluetooth Terminal, Web Bluetooth. It is not authenticated: enable it only when needed |
| **Web portal** (`WEB ON`) | `http://slink.local/` (or the IP: `NET`). Single page, light/dark theme, suited to phone and desktop; WebSocket `/ws` for commands and real-time state. Also REST: `GET /api/state`, `/api/presets`, `/api/stations`, `/api/favs`, `/api/stats` and `POST /api/cmd` (body = one command; the output comes over the WebSocket). A WebSocket client is registered when it sends its first frame |
| **Jukebox tab** | appears only while a CD changer (`90`/`91`/`92`) or an MD recorder (`B0`) answers the probe `90 22` / `B0 22`: transport, disc and track lists, memos / CD Text / MD titles, groups and artists. Reading and transport are free; every write (memo, MD titles, artist names, erase track, power off) asks for confirmation. Untested on real units: none is attached to the author's bus. Open `?demo&jb` to see it with an emulated changer and recorder |
| **Backup / restore** | Settings › Backup and restore saves one JSON file with the amplifier's configuration (input names, sound field, EQ, surround levels, speaker set-up, 2nd audio, 5.1; its tuner presets are stored for reference only, S-Link cannot write them), what the ESP32 keeps (settings, FM stations, favourites; passwords only if asked) and this browser's interface preferences; the file can be restored selectively. Volume, input and power state are never touched. Only writes verified on the STR-DA50ES are used (sound field, EQ, surround levels, 5.1); input names (`C0 88`), speaker sizes (`C3 53`) and the second audio source (`C0 52`) do not work on it and stay in the file for reference |
| **Raw TCP** (`APP ON`) | `nc slink.local 2323`: same text protocol as USB (max 2 clients) |
| **MQTT / Home Assistant** (`APP ON` + `MQTT ...`) | automatic discovery under `homeassistant/`; JSON state on `slink/<id>/state`, commands (any CLI line) on `slink/<id>/cmd`, availability on `slink/<id>/avail`. Entities: power, mute, input, sound field, volume (estimate), volume ±, FM frequency, station name, signal level |

First start (WiFi is configured over USB or BLE):

```
WIFI "NetworkName" "password"
WEB ON
NET            # shows the address
```

**Security.** The web portal is **HTTP only** (port 80, no HTTPS), and neither the portal nor the TCP console (2323) nor BLE has any authentication: use them only on a trusted network and do not forward ports from the router. The only configurable credentials are the WPA2/WPA3 WiFi password and the user/password
of the MQTT broker (`mqtt://` only, no TLS).

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

What the interface covers of the protocol, command by command and reply by reply, is in [`docs/en/WEB_COVERAGE.md`](docs/en/WEB_COVERAGE.md)
(generated by `python tools/gen_coverage.py` from the same catalogue). To try the page without an amplifier open `main/web/index.html` in the
browser (or add `?demo`): it uses a simulated amplifier.

The HTML file is embedded in the firmware: `tools/gen_web.py` converts it into `main/web_page.c` before every build
(automatic with PlatformIO, or by hand with `python tools/gen_web.py`). `tools/gen_coverage.py` regenerates the coverage document.

Build notes: ESP-IDF 6.x (`mdns` and `mqtt` are managed components, downloaded by the component manager: internet is needed on the
first build); partition table in `partitions.csv` (two app slots of 1.9 MB for the over-the-air update, flash 4 MB). After changing
`sdkconfig.defaults` delete the generated `sdkconfig.esp32s3` / `sdkconfig.esp32c3` / `sdkconfig.esp32c6` to regenerate them
(they are git-ignored).

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
- Web portal, TCP console and BLE are unauthenticated; the portal is HTTP only.

## Documentation
- [`docs/en/WIRING.md`](docs/en/WIRING.md): hookup (MOSFET or direct), first test.
- [`docs/en/ESP32-H2.md`](docs/en/ESP32-H2.md): the ESP32-H2 (no WiFi: USB serial + BLE only), what was checked about its power use, how to build it.
- [`docs/en/INPUTS_AND_FRAMES.md`](docs/en/INPUTS_AND_FRAMES.md): inputs, format of all measured frames (radio, EQ, sound field, volume, power, mute), FM station list, calibration of the knob.
- [`docs/en/RM-TP501.md`](docs/en/RM-TP501.md): the Sony RM-TP501 touch remote (the reference remote; RM-TP501E, RM-TP502, RM-TP503, RM-TP504 and RM-TP1 are the other reference remotes) (two-way infrared link, same messages as Control-A1) function by function against this bridge, the other sites on the protocol, and the decoding added from them.
- [`docs/en/WEB_COVERAGE.md`](docs/en/WEB_COVERAGE.md): what the web interface covers: every documented command, every firmware command, every S-Link reply (generated).
- [`docs/en/CODES_TO_TRY.md`](docs/en/CODES_TO_TRY.md): codes found online and not yet tried (already decoded by the firmware), with the sources.
- News: `C1 0E` = FM signal level; sound fields 0–29 are shown with their name (from the hifi-remote table, to be confirmed by ear).

## License

EUPL-1.2 License - See [LICENSE](LICENSE) for details. (The ESP-IDF framework and the managed components `espressif/mdns`
and `espressif/mqtt` that the firmware builds on keep their own licences.)
