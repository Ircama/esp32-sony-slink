# Memory (RAM) of the bridge

What takes the internal RAM, how to see it, and what was done on the chips that have little of it. All numbers were **measured on real boards** (WiFi `TIM-...` at -60 dBm, 17 November 2026, the firmware of that day); "free" is the free heap of the internal RAM.

## Seeing it

- **Statistics** page: *RAM interna* (free of total, used, lowest free, largest block), *PSRAM esterna*, and the card **Interfacce e funzioni** with the RAM each interface took *when it started*: Bluetooth, WiFi, mDNS, web portal (HTTP + HTTPS), Hue, TCP console, Slink-e, MQTT, Matter. The value is the difference of the free heap before and after the start (`main/svcmem.c`), so it includes the task stacks and the buffers it allocates at start, not what open connections need later.
- Console: **`TASKS [seconds]`** lists every task with what it does, its **CPU** (since the start, or in the next `<seconds>`: `TASKS 10`) and its **stack margin** (the part of its stack that was never used); `STATS` is the S-Link bus counters, not memory. The Statistics page has the same as a panel: the *Task attivi* row opens it (bars for CPU and stack margin, refreshed every 3 s; `GET /api/tasks`, or `@GET tasks` over Bluetooth, also on the H2). The CPU counter is 32 bits (the Matter SDK needs it): "since the start" is exact only in the first 71 minutes, the windowed value always.
- Over Bluetooth (`@GET stats`) the same fields arrive, also on the ESP32-H2 (which has no portal).

## Measured

| Chip | Total internal RAM | Bluetooth only | WiFi + portal + Bluetooth | + HTTPS + Hue |
|---|---|---|---|---|
| ESP32-H2 | 230 KB | 143 KB free | no WiFi | no WiFi |
| ESP32-C6 | 340 KB | 234 KB | 149 KB (min 130) | 127 KB (min 116) |
| ESP32-C3, first configuration | 234 KB | 93 KB | **7 KB (minimum 0.6 KB)**: the portal stopped answering | HTTPS could not start |
| ESP32-C3, with `sdkconfig.defaults.esp32c3` | 265 KB | - | 57 KB (min 43) | **34 KB (min 17)** |
| ESP32-S3 + Matter (PSRAM) | 330 KB + 2 MB PSRAM | - | - | 22.7 KB (min 12.3) |

What each interface took on the C3 (with the profile): Bluetooth 82 KB (it shares the radio with WiFi), WiFi 41 KB, web portal with HTTPS 42 KB, mDNS 5 KB. On the H2 the Bluetooth took 44 KB.

Interfaces that start at the same time overlap in the measure: on the S3 Matter build the WiFi figure (131.6 KB internal + 12 KB PSRAM) includes part of what Matter (88.7 KB) takes during the seconds before the IP address arrives. With Matter on, that build had 32.0 KB free (minimum 25.6 KB) and 2.0 MB of PSRAM free.

## What was done

- **ESP32-S3:** the 2 MB PSRAM takes every allocation of 4096 bytes or more (TLS, WiFi / lwIP buffers); the Matter build lowers that threshold to 512 bytes (free RAM 16.0 → 22.7 KB, minimum 8.4 → 12.3 KB). See [`MATTER.md`](MATTER.md).
- **ESP32-C3** (no PSRAM, [`sdkconfig.defaults.esp32c3`](../sdkconfig.defaults.esp32c3)): the code that ESP-IDF copies into the internal RAM for speed (WiFi, lwIP, FreeRTOS, heap) stays in the flash; WiFi is station only, with fewer and smaller buffers; lwIP is IPv4 only with smaller TCP windows (the number of sockets stays 16: with 10 the portal stopped after `HUE ON`); TLS record buffers are dynamic and the received records are at most 4 KB (**a firmware upload over HTTPS does not work on this chip: use `http://`**); at most 2 HTTPS connections at a time. Result: 7 → 57 KB free with WiFi + portal + Bluetooth.
- **Everywhere:** task stacks sized from `TASKS` (`slink_cmd` 5 KB, `slink_out` 3.5 KB, `slink_net` 4.5 KB, `slink_blej`, `slink_rx` and `slink_ws` 3 KB), and the BLINK test generator task is created at the first `BLINK` instead of at start-up.

## What the tasks showed

- **S3 with Matter, at rest:** both `IDLE` tasks at 98-99 % (the CPU is idle: no task loops; the `CHIP` task of Matter is at 0 %). The heat of 90 °C was the WiFi radio always on, not the CPU ([`MATTER.md`](MATTER.md)).
- **Stacks almost used up on the S3**: `ipc0` (the inter-processor calls, where the NVS and Matter flash writes go) had **276 bytes** left of its 1.25 KB, the timer service 444, `IDLE0` 652, and two of ours (`slink_cli`, `slink_out`) under 800: a stack overflow there is a *panic*, and the first Matter test ended with one. On the chips with PSRAM the stacks are now `ipc` 2.3 KB, timers 3 KB, `IDLE` 2.3 KB, `slink_out` 5 KB, `slink_cli` 4 KB, `slink_cmd` 7 KB, `slink_ws` 4 KB: the lowest margin is 1.3 KB. This is the most probable cause of the panic, not a proven one.
- **C3:** the lowest margin is 1.0 KB (`slink_cli`); the stacks there stay small to save RAM.

## Limits that remain

- **C3 with many HTTPS clients at once:** six parallel HTTPS requests: three are served, three refused, the portal stays up and the memory comes back (36 KB), but the free RAM touches 0.6-1.5 KB for a moment. A browser opens several connections: on the C3 the HTTPS portal is usable, the HTTP one is lighter.
- **C6:** the flash is the tighter side (image 1.84 MB in a 1.97 MB slot, 6 % free); the RAM is plentiful. **C5:** not measured here.
- The profile was verified on the C3 only; the C6 and C5 use the standard configuration.
