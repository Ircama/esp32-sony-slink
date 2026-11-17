# Matter (experimental, ESP32-S3)

With the Matter build the bridge shows up as a **Matter dimmable light** that Google Home, Apple Home, SmartThings, Home Assistant, Alexa (with a Matter controller) and other Matter ecosystems can pair with:

| The ecosystem says | The amplifier does |
|---|---|
| on / off | power on / standby |
| brightness (level 1-254) | volume: 100 % of the level is the **maximum volume** (`MATTER MAX`, default **30 %** of the knob travel), never more |

Matter is a separate firmware, **experimental**, **ESP32-S3 only** (see [Status](#status-and-limits)). It lives next to the other voice interfaces: [Philips Hue](../README.md#philips-hue-alexa) (Echo only, no cloud) and the [MCP server](MCP.md) (AI assistants).

## Using it

Install the Matter image (`tools/build_matter.sh` builds it; over WiFi `python tools/ota_push.py <host> out/slink-esp32s3-matter.bin`, or the first time over USB `slink-esp32s3-matter-factory.bin` at 0x0). Then *Settings › Interfaces › Matter* (the card only appears in this build) or the console:

| Command | |
|---|---|
| `MATTER` | state, name, volume cap, number of ecosystems paired, the pairing codes |
| `MATTER ON` / `OFF` | switch Matter on or off (saved); **the device restarts** |
| `MATTER OPEN` | open the pairing window for 15 minutes (it opens by itself after every start while nobody is paired) |
| `MATTER NAME <text>` | the name the ecosystem app shows (default *Amplificatore*; read at the next start) |
| `MATTER MAX <1-100>` | volume at 100 % level, percent of the knob (default 30; applies at once) |
| `MATTER NEWCODE` | a new random pairing code (from the next start) |
| `MATTER RESET` | forget every ecosystem; the device restarts |

Pairing: in the app add a Matter device and scan the QR code, or type the **manual pairing code** (the portal and `MATTER` show both; the QR is a link to the Matter project's code generator). The device is reachable over Bluetooth LE and over the local network (DNS-SD `_matterc._udp`), the phone and the bridge must be on the same network. Google Home needs a Google Home hub (Nest Hub, Nest speaker ...) to control Matter devices; Apple Home needs a home hub (Apple TV, HomePod) for remote use; Home Assistant needs its Matter server.

## What it takes and what changes

- **The pairing code is random** (made at the first start, kept in the settings): the well-known test code 20202021 is *not* used. `MATTER NEWCODE` makes a new one.
- **Not certified.** The device uses the test vendor id `0xFFF1` and test attestation certificates, so Google Home and others show a warning ("uncertified device") when pairing. It is meant for personal use.
- **Matter takes over Bluetooth.** Its commissioning uses the Bluetooth host, so while Matter is on the Bluetooth UART / the portal over Bluetooth (GitHub Pages) are off. WiFi, the portal, HTTPS, Hue, MQTT and the MCP server keep working.
- **Reset loop guard.** If the last restart was a crash, Matter switches itself off (nobody has a USB cable on the amplifier): `MATTER ON` tries again.
- **Memory.** Measured on the S3 with Matter on: free internal RAM 16 KB (minimum seen about 8 KB; the same build without Matter keeps about 39 KB), PSRAM 2 MB almost untouched. The image is 1.81 MB in a 1.97 MB slot (8 % free); the standard image is 1.53 MB. There is little room left on a 4 MB flash.
- Settings added: `matter` (on/off), `matter_max`, `matter_name`, and the random passcode and discriminator (not exported by `CFG`).

## Building

`esp-matter` does not build on Windows: use Linux or WSL. It is plain ESP-IDF **6.1** (the same version as the rest of the project) with esp-matter `release/v1.6.1`; no change to either was needed. The recipe is at the top of [`tools/build_matter.sh`](../tools/build_matter.sh):

```
git clone --depth 1 --branch v6.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git && (cd esp-idf && ./install.sh esp32s3)
git clone --depth 1 --branch release/v1.6.1 https://github.com/espressif/esp-matter.git
(cd esp-matter && git submodule update --init --depth 1 && cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --shallow --platform esp32 linux)
(cd esp-matter && ./install.sh --no-host-tool)

. esp-idf/export.sh && export ESP_MATTER_PATH=$PWD/esp-matter && . esp-matter/export.sh
tools/build_matter.sh        # out/slink-esp32s3-matter.bin and out/slink-esp32s3-matter-factory.bin
```

About 8 GB for the sources; the first build compiles the Matter SDK (30-40 minutes). The normal builds (PlatformIO, GitHub Actions) do not change: `CMakeLists.txt` and `main/CMakeLists.txt` add `esp_matter` and `main/matter.cpp` only when the environment has `SLINK_MATTER` and `ESP_MATTER_PATH`; in every other build `MATTER` answers that Matter is not in this build (the empty versions are in `main/matter.h`). The extra settings of the Matter build are in `sdkconfig.defaults.matter`; the partition table is the same as the standard one, so the settings survive moving between the two firmwares.

## How it works

`main/matter.cpp`: one endpoint, a *dimmable light* (`esp_matter::endpoint::dimmable_light`). The OnOff attribute becomes `PWRON` / `PWROFF`, the CurrentLevel attribute becomes `VOL SET` scaled to `MATTER MAX` (the same rule as Hue, `hue.c`). The other way round, every change the bridge sees on the S-Link bus (`net_notify_state`) updates the two attributes, so the ecosystem shows the real state. The setup passcode and discriminator come from a custom *commissionable data provider* (the SPAKE2+ verifier is computed from the random passcode), and the pairing window is opened by hand with `OpenBasicCommissioningWindow` (Bluetooth + DNS-SD).

## Status and limits

Verified on the real bridge (ESP32-S3 with PSRAM, WiFi + HTTPS + Hue + web portal on):

- the Matter image builds with ESP-IDF 6.1 and esp-matter 1.6.1 without changes, installs over the air and confirms itself (rollback not triggered);
- `MATTER ON` starts the stack, the node comes up with a random passcode, the pairing window opens by itself, the device advertises over Bluetooth and answers DNS-SD (`_matterc._udp`, vendor `0xFFF1`, discriminator as in the code);
- the web portal, HTTPS and the standard commands keep working with Matter running.

**Not verified** (no Matter controller was at hand: no Google/Apple hub, and the PC cannot reach the device's multicast from WSL):

- pairing with a real ecosystem (Google Home, Apple Home, Home Assistant ...);
- that on/off and level really drive the amplifier through Matter, and the state reports back;
- long-run stability and the memory margin (about 8 KB of internal RAM at the minimum) with a controller connected;
- Wi-Fi behaviour when the controller reconfigures the network; the Matter stack and the bridge share the same WiFi connection.

If you try it with a controller, `MATTER` shows the state and the number of ecosystems paired; the console `HELP` lists the commands.

Other chips: esp-matter also supports the C3, C6, C5 and H2 (Thread), but the bridge's memory and flash budget there was not looked at; only the S3 build exists. An alternative stack in Rust, [rs-matter](https://github.com/project-chip/rs-matter), was prepared (toolchain installed) but not tried: esp-matter worked first.
