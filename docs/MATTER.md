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
- **Memory.** Measured on the S3 with Matter on: free internal RAM 22.7 KB (minimum seen 12.3 KB; the same build without Matter keeps about 39 KB), PSRAM 2 MB almost untouched. Matter makes many small allocations, so this build sends everything from 512 bytes up to the PSRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512` in `sdkconfig.defaults.matter`; the standard S3 build keeps 4096): that raised the free internal RAM from 16.0 to 22.7 KB and the minimum from 8.4 to 12.3 KB. The image is 1.81 MB in a 1.97 MB slot (8 % free); the standard image is 1.53 MB. There is little room left on a 4 MB flash.
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

**Paired with Apple Home** (iPhone 14 Plus, QR code, no hub needed for this): the device is added, switching on and off works.
**Google Home does not work without a Google hub** (a Nest Hub, a Nest speaker or display ...): it refuses to add a Matter device from the phone alone, whatever the device.

Found with the first real controller, and what was done about each (not yet checked again on the phone):

- **The volume slider oscillated up and down.** A slider (and the cluster's own transitions) sends a series of levels, and each one became a `VOL SET` while the bridge also kept telling the controller the estimated position of the motorised knob, which was still on its way. Now the level a controller asks for is sent **once**, 400 ms after the last value; for 20 s the knob position is not reported back; in standby the level is not sent and the controller gets the real one.
- **Switching on and off was slow.** The WiFi power saving of the default eco profile (maximum modem sleep) left the radio asleep when the controller's UDP requests arrived, and they were retried after 300 ms. Switching the power saving off was **a mistake**: the radio always on took the chip of the S3, shut in the case next to the amplifier, from about 60 to **90 °C** (measured on the Statistics page; with `WIFIPWR AUTO MIN AUTO` it fell back to 61 °C in four minutes). With Matter on the WiFi now uses the **minimum modem sleep** (the radio wakes at every DTIM beacon of the router): the chip stays at **56-60 °C** with Matter running and paired (ping to the device averages about 230 ms). `WIFIPWR` still overrides it, but do not set `ps NONE` on a device in a closed case.
- **A panic (crash).** The Statistics page showed *last reset: panic* and the crash guard switched Matter off by itself. The probable cause is the task that tells the controllers about changes, which had a 3 KB stack: `attribute::update` goes through the CHIP stack and needs more; it is 6 KB now. **That is a probable cause, not a proven one**: the S3 sits on the amplifier without a USB cable and the console is off, so there is no backtrace. If it happens again, connect it to a computer.
- **Manufacturer and model were empty in Apple Home.** The device did not provide its identity (Basic Information cluster): now vendor *Ircama*, product *S-Link bridge*, label *Sony S-Link / Control-A1 bridge*, part number the chip, serial number the WiFi MAC, product URL the project page (a custom `DeviceInstanceInfoProvider`; the vendor id is still the test one, `0xFFF1`).
- The text "Please scan with your CHIPTool app" is not written by the firmware: it belongs to the page of the Matter project that the *QR code* link opens (an online QR generator). CHIPTool is the project's reference controller app; the same QR is read by Apple Home.

Memory: with the device already paired the Matter stack takes about 19 KB of internal RAM (free 93 KB, minimum 39 KB with the portal, HTTPS and Matter on); while the pairing window is open it takes about 89 KB more, because Matter then starts its own Bluetooth stack for the commissioning.

Verified on the real bridge (ESP32-S3 with PSRAM, WiFi + HTTPS + Hue + web portal on):

- the Matter image builds with ESP-IDF 6.1 and esp-matter 1.6.1 without changes, installs over the air and confirms itself (rollback not triggered);
- `MATTER ON` starts the stack, the node comes up with a random passcode, the pairing window opens by itself, the device advertises over Bluetooth and answers DNS-SD (`_matterc._udp`, vendor `0xFFF1`, discriminator as in the code); Apple Home adds it; on/off from the phone;
- the web portal, HTTPS and the standard commands keep working with Matter running; after a restart the pairing is kept (`paired with 2 ecosystems`).

**Not verified**:

- that the level (volume) from the phone behaves after the change above, and that the state reports back correctly;
- the real cause of the panic, and long-run stability with a controller connected;
- Google Home (needs a hub), Home Assistant, SmartThings;
- Wi-Fi behaviour when the controller reconfigures the network; the Matter stack and the bridge share the same WiFi connection.

If you try it with a controller, `MATTER` shows the state and the number of ecosystems paired; the console `HELP` lists the commands.

Other chips: esp-matter also supports the C3, C6, C5 and H2 (Thread), but the bridge's memory and flash budget there was not looked at; only the S3 build exists. An alternative stack in Rust, [rs-matter](https://github.com/project-chip/rs-matter), was prepared (toolchain installed) but not tried: esp-matter worked first.
