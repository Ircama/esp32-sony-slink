# Inputs and frames observed on Sony STR-DA50ES (S-Link / Control A1)


Everything below was **measured on a real unit** (STR-DA50ES, ID `C0`)
with this firmware, except where marked "not verified". Frames are in hexadecimal.

## Addresses (first byte = `nnnn X yyy`)
| Command | Reply | Device |
|---|---|---|
| `C0` | `C8` | Amplifier |
| `C1` | `C9` | Tuner |
| `C3` | `CB` | Surround / sound field |

`X` (bit 3) = 1 → reply from the unit. The firmware annotates every `RX` with `; ...` (`DECODE` command).

## Inputs (`C0 50 SS` selects, `C0 48 SS` reads the label)
Labels read from the amplifier with the `INPUTS` command:

| Code `SS` | Label on the display | Description |
|---|---|---|
| `00` | TUNER | Internal FM/AM tuner (controlled with `C1 ..`) |
| `01` | PHONO | Turntable input |
| `02` | C D | CD player |
| `03` | MD/DAT | MD / DAT recorder (alias of `04`) |
| `04` | MD/DAT | MD / DAT recorder (input active at the time of the test) |
| `05` | TAPE | Cassette / tape recorder |
| `10` | VIDEO 1 | Audio/video input 1 |
| `11` | VIDEO 2 | Audio/video input 2 |
| `12` | VIDEO 3 | Audio/video input 3 |
| `15` | DVD | DVD input (alias of `19`; when selecting `15` the amplifier replies `19`) |
| `16` | TV/LD | TV / Laserdisc input (SAT/TV source) |
| `17` | TV/LD | Alias of `16` (when selecting `17` the amplifier replies `16`) |
| `19` | DVD | DVD label; selecting `19` directly gives `error`, use `15` |
| `07`, `13` (and others) | – | Not present on this model: reply `C8 0F` (error) |

After `C0 50 SS` the amplifier replies `C8 70 AA AV CC XX` (input state):
`AA` = audio source, `AV` = video source, `CC` bit 1 = **mute** (`01` normal, `03` muted),
`XX` = undocumented per-input byte (observed: MD `B0`, CD `90`, TUNER `C1`, TAPE `A4`,
VIDEO2 `91`, others `FF`). The surround often replies with `CB 70 ..` (state, not decoded).
Note: `C0 50 04` leaves the video on `16`; to return exactly to the initial state use `C0 50 04 16`.

## Volume and mute
- `C0 14` volume up, `C0 15` volume down: **no reply** (as per the documentation) and the
  level does not appear in the `C8 70` state (the `XX` byte does not change). At least 3 ms is needed between two commands.
- `C0 06` mute on → `C8 70 .. CC=03`; `C0 07` mute off → `CC=01`. Both confirmed.
- Volume queries `C0 40`/`41`/`42`/`43`/`45`/`46`/`47` → `C8 0F` (error): on this
  model there is no known way to read the volume.
- `C0 0F` = state request (reply `C8 70 ..`); `C0 0E` → error.

## Radio (tuner `C1`)
State: `C9 70 BB NN FL HH LL`
- `BB` preset bank (01–03), `NN` preset number (`FF` = no preset),
- `FL` flags: bit 0 = AM (0 = FM), bit 5 (`20`) = seek in progress, bit 7 (`80`) = mono,
- `HH LL` frequency: FM in hundredths of MHz (`26 61` = 98.25 MHz), AM in kHz (`02 D0` = 720 kHz).

Verified commands: `C1 50 00 HH LL` direct FM tuning; `C1 50 01 HH LL` AM (rounded to
9 kHz steps: 728 → 720); `C1 06`/`C1 07` seek up/down (the `20` flag stays set while seeking);
`C1 08`/`C1 09` preset up/down; `C1 0A` mono / `C1 0B` stereo. Error `C9 0F` for `C1 03 BB` (band),
`C1 51 BB NN` (direct preset), `C1 70`, `C1 6A`, `C1 48`: not supported here.
Undecoded event: `C9 71 01 FF 0A 00/01` (appears when switching to the TUNER input).

**FM station scan (87.50–108 MHz) carried out:** the only locked station is **98.25 MHz**
(stereo). Over the rest of the band the automatic seek does not stop: antenna missing or very weak.
The station name is read with `C1 48 BB FF` (see the section below on RDS names).

## Other replies
- `C0 6A` → `C8 6A "STR-DA50ES"` (device name, ASCII).
- After power-on the amplifier sends `C8 2E`, `C8 61 ..`, `C9 61 ..`, `CB 61 ..` (model info,
  undocumented) and the `70` states.
- From the tuner/surround `C1 6A`/`C3 6A` → `C9 0E` (not available) / `CB 0F` (error).

---

# Surround, sound field and equaliser (device `C3`, replies `CB`)

Codes from the documentation [hifi-remote Sony_dsp_195](https://www.hifi-remote.com/sony/Sony_dsp_195.htm)
(there in decimal, here in hexadecimal), **verified on the STR-DA50ES** where indicated.
All tests were done by changing a value, reading it back and **restoring** the original;
the final state was compared with the initial snapshot and is identical.

## Initial state found (MD/DAT input, before any change)
| Item | Initial value | How it was read |
|---|---|---|
| Sound field | off: `CB 70 00 00 00` | `C3 0F` |
| Front EQ bass | 100 Hz, gain 0 | `C3 50 00 00` → `CB 50 00 00 00 64 00 01 01` |
| Front EQ mid | 5000 Hz, gain 0 | `C3 50 00 01` → `CB 50 00 01 13 88 00 01 01` |
| Front EQ treble | 10000 Hz, gain `0A` (+10) | `C3 50 00 02` → `CB 50 00 02 27 10 0A 01 01` |
| Centre / rear EQ | `CB 0E` (not available with sound field off) | `C3 50 01/02 BB` |
| Rear / sub / balance levels | `CB 0E` (not available with sound field off) | `C3 40/42/43` |
| Dynamic range compression (DRC) | 0 (off) → `CB 46 00 01` | `C3 46` |
| LFE mix | 0 → `CB 47 00 01` | `C3 47` |

With the sound field on (`C3 04`, which on the TUNER input brought up field `1D`):
rear level, subwoofer, balance, DRC, LFE all reply `0`; the centre level
stays `CB 0E` (no centre speaker); the rear mid EQ replies 1200 Hz, gain 0.
The sound field is **stored per input**: TUNER → `CB 70 1D 00 0C`, MD/DAT → `CB 70 00 00 00`.

## Format of the `CB` replies
| Frame | Meaning |
|---|---|
| `CB 70 FF PP SS` | state: `FF` = sound field number (`00` = off); `PP` = sound field preset (0–4); `SS` = **speaker configuration** bitmap (`01` front small, `04` centre downgraded to small when the front is large, `08` no centre, `10` rear downgraded, `20` no rear, `40` no subwoofer), reported only with a field on. On the DA50ES `00 0C` = no centre speaker (source: hifi-remote Sony_dsp_203, see `docs/en/RM-TP501.md`) |
| `CB 50 POS BAND FH FL G SL TY` | equaliser: position (0 front, 1 centre, 2 rear), band (0 bass, 1 mid, 2 treble), frequency `FH FL` in Hz, gain `G` (signed byte, dB), slope `SL` (0 narrow, 1 medium, 2 wide), `TY` = `01` reply to a query, `03` setting confirmation |
| `CB cc VV TY` | parameter: `cc` = `40` rear level, `41` centre, `42` subwoofer, `43` rear balance, `46` dynamic range compression, `47` LFE mix; `VV` signed value; `TY` = `01` read, `03` setting confirmed (`02`/`00` appear after the ± keys, see below) |
| `CB 0E` | not available (in the current state) |
| `CB 0F` | error / command not supported |

DRC: `0` off, `1`–`9` = 0.1–0.9, `10` standard, `255` maximum. The LFE mix and the levels are signed bytes
(`FE` = −2, `FF` = −1).

## Verified commands (`C3`)
| Command | Effect | Result |
|---|---|---|
| `C3 0F` | state request | `CB 70 FF xx yy` ✔ |
| `C3 04` / `C3 05` | sound field on / off | ✔ (reply `CB 70 ..`) |
| `C3 51 NN` | select sound field `NN` | ✔ for `NN` = `01`–`1D` (29 fields), `00` = off; `1E` and above → `CB 0F` |
| `C3 50 POS BAND` | query EQ | ✔ only position 0 with the field off; 0 and 2 (mid) with the field on |
| `C3 90 POS BAND FH FL G SL TY` | set EQ (TY 0 = gain, 1 = frequency) | ✔ gain bass +2, mid −2, treble +8; frequency bass 100→140 Hz: all read back correctly and restored |
| `C3 80 VV` / `82` / `83` | set rear level / subwoofer / balance | ✔ `CB cc VV 03` and read back with `C3 40/42/43` |
| `C3 86 VV` | set dynamic range compression | ✔ 0→5→0 |
| `C3 87 VV` | set LFE mix | ✔ 0→−2→0 |
| `C3 40`/`42`/`43`/`46`/`47` | queries | ✔ (`CB 0E` if not available) |
| `C3 10`/`11` | rear level up/down | reply `CB 40 14 02` and then `CB 40 FF 00`: the keys move the level (I read −1 after "down"), meaning of `14 02` unclear; use `C3 80 VV` to set an absolute value |
| `C3 06` / `C3 07` | tone EQ on/off | ✘ `CB 0F` on this model |
| `C3 A0 tt` | test tone (`00` off, `10` auto, `20`–`25` speakers) | ✘ `CB 0F` on this model, tried with the sound field off and on (field 29) |
| `C3 0D`, `C3 48`, `C3 55`–`C3 5C`, `C3 60`, `C3 58 BB` | AFD, string, distances, crossover, trim etc. | ✘ `CB 0F` on this model |

Not tried so as not to alter the setup: EQ preset selection (`C3 54 n`), speaker sizes
(`C3 53`), distance (`C3 95`), crossover (`C3 97`), rear position (`C3 96`), test tone (`C3 A0`).
From the documentation: `C3 54 n` EQ preset 0–5, `C3 53` bit map of the sizes, `C3 95` distances
(value + 7), `C3 97` crossover 40–200 Hz in steps of 10, `C3 96` position (0 side, 1 centre,
2 behind, 3 high), `C3 A0` tone (0 off, 16 auto, 32 left, 33 centre, 34 right, 35 surround right, 36 surround left, 37 sub).

## Tuner (`C1`): additions
- The tuner replies **only with the TUNER input active**; otherwise `C9 0E` (not available) for every command.
- `C1 0C` and `C1 0F` both return the state `C9 70 ..`; `C1 22` → `C9 61 07 03 1E 03 0A` (capabilities/model).
- `C1 51 BB NN` recalls the preset (bank 1–3, `NN` from 1 to 10: preset "0" is `0A`) ✔; the presets were all empty
  (frequency 87.50 MHz). `C1 51 BB 00` gives an error.

## How the firmware decodes it
Every `RX` line receives `; DEVICE cmd|reply: text` (`DECODE ON|OFF` command, default ON):
`AMP`, `TUNER`, `SURROUND`, plus generic CD/MD. With `LOOPBACK ON` the transmitted frames are decoded too.
Undocumented frames (`C8 61`, `C9 61`, `C9 71`, trailing `XX` byte of `C8 70`) are left raw.

---

# Complete measurements: FM radio, volume, equaliser, power, mute (STR-DA50ES)

## FM radio (C1/C9)
State: `C9 70 BB NN FL HH LL` (bank, preset `FF` = none, flags, frequency). Bits of the `FL` flags (measured
on 94.80 MHz with stereo/mono forced and during scans):

| Bit | Value | Meaning |
|---|---|---|
| 0 | `01` | AM band (0 = FM) |
| 3 | `08` | signal locked |
| 5 | `20` | automatic seek in progress |
| 6 | `40` | stereo pilot received (disappears when mono is forced) |
| 7 | `80` | mono forced (`C1 0A`; `C1 0B` returns to stereo) |

Bits 3 and 6 appear ~1 s after tuning: immediately after `C1 50` the flag is `00`.
FM frequency in hundredths of MHz with **50 kHz step** (97.36 → 97.35); AM in kHz, 9 kHz step.
Event `C9 71 BB NN LV ST` after a direct tuning (`C1 50`): `LV` = signal level (observed 1 weak …
12 strong), `ST` = `00` then `01` (measurement finished). It is not emitted after automatic seeks.
`C1 52 PP` (PTY seek) → `C9 1B` (not supported); the RDS name is read only on request with `C1 48 BB FF` (see the names section).
`C1 51 BB NN`: recalls the preset (`NN` 1–10, the presets were empty = 87.50 MHz).

**FM scan carried out (antenna connected): 47–50 stops** between 87.70 and 107.90 MHz. Firmware command:
`FMSCAN` (`C1 06` repeated, waiting for the `20` flag to go to zero, re-reading after 2 s). Frequencies found (MHz):
88.50 · 88.85 · 90.10 · 90.80 · 91.10 · 91.70 · 92.00 · 92.40 · 92.60 · 93.10 · 93.40 · 93.80 · 94.20 · 94.80 ·
95.20 · 95.50 · 95.60 · 95.70 · 96.10 · 96.30 · 96.50 · 96.80 · 97.10 · 97.80 · 98.20 · 98.90 · 99.90 · 100.30 ·
101.20 · 101.50 · 102.05 · 102.50 · 102.80 · 103.05 · 103.50 · 104.20 · 104.50 · 104.80 · 105.10 · 105.50 ·
105.90 · 106.30 · 106.70 · 107.00 · 107.30 · 107.60 · 107.90. Signal locked and stereo: the strongest
(88.50 level 12, 105.50 level 12, 91.10 and 101.50 level 11). Some stops are only noise (without `locked`).
The seek stops many times: the tuner sensitivity is high; the automatic seek does not guarantee
a real station at every stop (check `locked`).

## Volume (motorised knob)
- There are **only steps**: `C0 14` (up) and `C0 15` (down). No reply, no level reading,
  no absolute volume (`C0 40`/`80 VV` not supported). The volume does not appear in the state frames.
- Measurements with the knob observed: 20 steps ≈ 5° (≈ 0.25°/step); 200 steps at maximum rate ≈ 120°
  (≈ 0.6°/step): at a high rate the motor moves **continuously**, at a low rate in jerks.
- **Maximum rate on the bus: 30 ms per frame** (≈ 25 ms of frame + 3 ms of idle line, `GAP 0`);
  with the default `GAP` (30 ms between starts) you get 39 ms/step, with 100 ms → 105 ms/step.
- Firmware: `VOL UP|DOWN [n]` (n = 1–250, blocking), `GAP <ms>`. At the end of travel the extra steps are harmless
  (used to return to zero: 400 steps down).

## Equaliser (3 bands, not graphic)
`C3 50 POS BAND` → `CB 50 POS BAND FH FL G SL TY`. Positions: **0 front**, **2 rear**
(readable only with the sound field on), 1 centre and 3 always `CB 0E`. **Bands: 0 bass, 1 mid, 2 treble**;
indices 3 and above repeat band 0. Slope always 1. Values found at the start:
front bass 250 Hz 0 dB, mid 5000 Hz +4 dB, treble 10000 Hz +10 dB; rear bass 250 Hz 0 dB,
mid 1200 Hz 0 dB, treble 2500 Hz 0 dB.

**Gain:** −10…+10 dB, 1 dB steps; out-of-range values are clamped to ±10 (−12 → −10, +12 → +10).
**Selectable frequencies** (the amplifier rounds to the nearest; same lists front and rear):

| Band | 21 available frequencies (Hz) |
|---|---|
| Bass | 100, 111, 125, 140, 155, 175, 200, 220, 250, 280, 315, 350, 400, 445, 500, 560, 630, 710, 790, 890, 1000 |
| Mid | 500, 561, 630, 707, 794, 891, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3100, 3600, 4000, 4500, 5000 |
| Treble | 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3100, 3600, 4000, 4500, 5000, 5600, 6300, 7100, 8000, 9000, 10000 |

**Control verified** on all 6 bands (front and rear): `C3 90 POS BAND FH FL G 01 TY` with
`TY` = 0 (set gain) or 1 (set frequency, keeping the gain in the frame). The confirmation is `CB 50 ... 03`.
The firmware: `EQ` (reads everything), `EQ F|R BASS|MID|TREBLE GAIN <dB>` and `... FREQ <Hz>`. After all the tests the six
values were rewritten and read back: no difference.

## Power and mute
- `C0 2F` switches off (standby): the amplifier sends a truncated frame (`C8 +7bit !timing`) and then still replies
  to queries with `CC = 02` (bit 0 at 0). `C0 2E` switches on: `C8 2E`, `C8 61 ..`, `C9 61 ..`,
  `CB 61 ..` arrive, then `C8 70`, `C9 70`, `CB 70`, `C9 71` (about 2–3 s). **The whole state is kept** across the restart
  (input, video, mute, radio, sound field, EQ).
- Status byte `CC` of `C8 70`: **bit 0 = on** (1) / standby (0), **bit 1 = mute**. Values: `01` on,
  `03` on+mute, `02` standby+mute.
- `C0 06` mute, `C0 07` unmute: idempotent, confirmed by `C8 70`. If the user uses the keys on the
  front panel the amplifier spontaneously sends `C8 70 ..` (useful as a notification).

---

# Volume knob: travel (measured by direct observation)
- Command: `VOL UP|DOWN n` (n ≤ 250 per command), `VOL MIN` / `VOL MAX` (750 steps, beyond the end stop).
- From minimum: **250 steps = about 90°**; **500 steps = maximum end stop** reached (confirmed by the user);
  with 750 steps the **motor keeps turning** against the end stop (clutch): the extra steps are harmless.
  Downwards: 750 steps bring it back to the minimum (verified several times). The full travel is therefore
  between 250 and 500 steps (≈ 8–15 s at 30 ms/frame). The exact maximum angle was not quantified.
- An earlier measurement (200 steps ≈ 120°) does not match the line above (250 steps ≈ 90°): the angle per step
  depends on the rate and on the starting point; there is no electronic reading, only observation.
- The volume can be neither read nor set in absolute terms; during all the tests mute was active
  (`C0 06`), then removed at the end of the test with the knob at minimum.

# FM stations detected (FMSCAN with direct re-read)
Level 0–12 from the `C9 71` frame (after direct tuning); *locked* = signal locked (flag `08`);
*stereo* = stereo pilot (flag `40`). Stops without a level (`-1`) and *unlocked* are noise or stations that are too weak.

| MHz | Level | Lock | Stereo |
|---|---|---|---|
| 88.50 | 12 | yes | yes |
| 105.50 | 12 | yes | yes |
| 91.10 | 11 | yes | yes |
| 101.50 | 11 | yes | yes |
| 104.50 | 11 | yes | yes |
| 90.10 | 10 | yes | yes |
| 93.10 | 10 | yes | yes |
| 101.20 | 10 | yes | yes |
| 90.80 | 9 | yes | yes |
| 93.40 | 9 | yes | yes |
| 94.80 | 9 | yes | yes |
| 96.50 | 9 | yes | yes |
| 97.10 | 9 | yes | yes |
| 97.80 | 9 | yes | yes |
| 98.90 | 9 | yes | yes |
| 102.50 | 9 | yes | yes |
| 102.80 | 9 | yes | yes |
| 104.20 | 9 | yes | yes |
| 105.90 | 9 | yes | yes |
| 106.30 | 9 | yes | yes |
| 106.70 | 9 | yes | yes |
| 107.00 | 9 | yes | yes |
| 107.30 | 9 | yes | yes |
| 99.90 | 7 | yes | yes |
| 92.00 | 9 | yes | no (mono) |
| 107.90 | 7 | yes | no (mono) |
| 107.60 | 1 | yes | yes (very weak) |
| 96.80 | 1 | yes | no (very weak) |
| 104.80 | 1 | yes | no (very weak) |
| 88.85 | 0 | yes | no (extremely weak) |

Other unconfirmed stops (without lock): 91.70 · 92.40 · 92.60 · 94.60 · 95.20 · 95.50 · 95.70 · 96.10 ·
96.30 · 98.20 · 98.35 · 98.40 · 100.30 · 103.05 · 103.50 · 105.10. The number of stops varies between scans
(46–50): the automatic seek is sensitive to noise. The RDS names are read on request with `C1 48 BB FF` (names section).


## Number of steps for the full travel (measurement by fractions)
From the minimum, at 30 ms per step (`GAP 30`, mute active, direct observation of the knob):

| Test | Result |
|---|---|
| 250 steps then 10 stages of 25 steps | the knob advanced up to the 7th stage (400–425 steps), then stopped |
| 400 steps then stages of 10 steps | advanced up to the 2nd stage (420 steps), then stopped |

**Maximum end stop = about 420 steps (410–420) from the minimum**, i.e. about 12.6 s of commands. Beyond that value
the motor keeps turning against the end stop with no effect. The minimum is reached with 750 steps down
(`VOL MIN`). The maximum angle was not measured; extrapolating from the ~90° seen at 250 steps it would be
about 150° (unverified estimate). Measurement resolution: ±10 steps; the value depends on the rate
(other rates were not compared quantitatively).

## Estimated knob position (firmware)
So as not to run the motor idle, the firmware keeps an **estimate** of the position in steps from the minimum (travel 420):
`VOL UP/DOWN n` update it (clamped at 0 and 420), `VOL MIN`/`VOL MAX` send only the missing steps **+ 10 as margin**
(if the position is unknown, 430 steps), `VOL SET <0-100>` goes to a percentage of the travel, `VOL POS` shows the estimate
and `VOL POS <steps>` declares it without moving anything. The estimate is lost on restart and if the knob is turned by hand
(in that case: `VOL MIN` to start again from a certain reference). Verified: `VOL SET 50` = 210 steps; `VOL SET 0` from 220 steps
sends 230.

## Knob travel: up and down are NOT symmetric (final calibration)
Tests with direct observation, rate 30 ms/step, mute active:

| Test | Outcome |
|---|---|
| +400 steps from the minimum | "just short of the maximum" |
| +410 steps from the minimum | at the maximum end stop |
| +420 steps from the minimum | maximum reached (repeated 3 times) |
| 410 down from the maximum | "looks exactly at the minimum" (after +410) but afterwards not reproducible |
| −420 after +420 | does **not** reach the minimum: 5°–15° remain (≈ 15–40 steps), reproducible |
| −445 after +420 | **at the minimum, almost no idle turning** |

**Full travel: 420 steps up, 445 steps down** (one step down moves ≈ 94% of one step up). With these
constants the cycle `VOL SET 100` → `VOL SET 0` (430 up, 455 down: margin 10) reaches the exact maximum and minimum, with
negligible idle turning (verified by the user). The firmware keeps the estimated position in "steps up" and converts the descent
(`down_to_up`); `VOL MIN` from an unknown position sends 455 steps, `VOL MAX` 430. Because of the mechanical imprecision
the estimate may drift after many movements: starting again from an extreme (`VOL MIN`/`VOL MAX`) resets it.


---

# Station names (RDS): `C1 48 BB FF`
The tuner **decodes RDS but never sends the name spontaneously** (neither during `TUNER SCAN+`, nor after tuning): it must be
**requested** with `C1 48 BB FF`, where `BB` is the current bank read from the state (`C9 70 BB ..`, here 03) and `FF` means
"station tuned now". Reply: `C9 48 BB FF` + 8 ASCII characters (e.g. `C9 48 03 FF 4D 61 72 63 6F 6E 69 20 ..` = "Marconi").
- If the station does not transmit an RDS name (or it has not been decoded yet, it takes about 1–3 s after tuning): `C9 0E` (not available).
- If `BB` is not the current bank: `C9 0E`. With `NN` = 1–10 (name of a preset): `C9 10` (no stored name; the presets were empty).
- Without parameters (`C1 48`) or with bank 0: `C9 0F` (error).
- Firmware: `NAME` (current station) and `FMSCAN` (prints the name of every station); in the `C9 48 ..` state the decoder shows
  `station name "..." (bank 3, current station)`.

FM stations with RDS name (scan, antenna connected):

| MHz | RDS name | Level | Stereo |
|---|---|---|---|
| 88.50 | \*Radio2 | 12 | yes |
| 88.85 | R. MATER | 0 | no |
| 90.10 | CAPITAL | 10 | yes |
| 90.80 | VIVA FM | 9 | yes |
| 91.10 | FRECCIA | 11 | yes |
| 92.00 | GAMMARAD | 9 | no |
| 93.10 | M DUE O | 10 | yes |
| 93.40 | #STUDIO+ | 9 | yes |
| 94.80 | Marconi | 9 | yes |
| 97.10 | BIRIKINA | 9 | yes |
| 97.80 | KISSKISS | 9 | yes |
| 98.90 | 105 | 9 | yes |
| 99.90 | \*Radio3 | 7 | yes |
| 101.20 | R101 | 10 | yes |
| 101.50 | VIRGIN | 11 | yes |
| 102.50 | RTL102.5 | 9 | yes |
| 102.80 | ZETA | 9 | yes |
| 104.20 | \*Number1 | 9 | yes |
| 104.50 | VIRGIN | 11 | yes |
| 104.80 | RADIO 24 | 1 | no |
| 105.50 | RMC | 12 | yes |
| 105.90 | 105 | 9 | yes |
| 106.70 | R ITALIA | 9 | yes |
| 107.00 | DEEJAY | 9 | yes |
| 107.30 | \* RDS \* (placeholder name of the station) | 9 | yes |
| 107.60 | POPOLARE | 1 | yes |
| 107.90 | R.MARIA. | 7 | no |
| 106.30 | (no RDS name) | 9 | yes |

The other stops of the seek (without lock or with an extremely weak signal, e.g. 88.30, 91.70, 92.40, 95.50, 98.20, 100.30, 102.10, 105.10)
have no name. The name is the 8-character RDS "Programme Service name" transmitted by the station (it may have leading/trailing spaces).

## Tuning by name (firmware feature, not a protocol feature)
The protocol only has tuning **by frequency** (`C1 50 00 HH LL`, FM in hundredths of MHz, 50 kHz step) and by **preset** (`C1 51 BB NN`);
seeking by programme type (`C1 52`) is not supported (`C9 1B`). To tune by name the firmware uses the table built by `FMSCAN`
(frequency + RDS name, up to 48 entries, saved in NVS): `STATIONS` shows it, `TUNE ZETA` / `TUNE #9` / `TUNE 94.80` tune.
Names may vary between scans (RDS read too early, dynamic names such as "*DISCO*", "ANNI'60"): run `FMSCAN` again to update.


## Audio and video from different inputs

The status reply `C8 70 <audio> <video> ...` has separate audio and video fields. Measured on the STR-DA50ES (11 Nov 2026), with the unit on and mute on:

| Sent | Status afterwards |
|---|---|
| `C0 50 15` (DVD) | audio = DVD, video = DVD |
| then `C0 50 00` (TUNER) | audio = TUNER, **video = DVD** (kept) |
| `C0 50 16` (SAT/TV), then `C0 50 00` | audio = TUNER, video = SAT/TV |

So selecting a **video input** (`10`–`19`) sets both, while an **audio-only input** (`00`–`05`: Tuner, Phono, CD, MD/DAT, Tape) changes only the audio. To get audio X with video Y: select Y, then X. `C0 82 AA VV` (audio split) answers `C8 0F` (error) on this unit. The audio cannot come from a video input while the video comes from another one.
Firmware: `INPUT AV <audio> <video>` (video first, 700 ms later the audio); web interface: the **Sorgente video** card.
