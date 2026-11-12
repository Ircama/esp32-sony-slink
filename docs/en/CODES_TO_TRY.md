# S-Link / Control-A1 codes found online: checked and still to check

## Systematic check on the STR-DA50ES (10 November 2026)

Every item of the lists below that could be tried **and restored** was tried on the amplifier, with mute on, noting the state first and
reading it back after each change. The amplifier was brought back to its initial state (TUNER, SAT/TV video, sound field off, 104.80 MHz, mute off).
Order matters: the amplifier remembers the sound field and the EQ **per input**, so the other inputs were visited and left as found.

| Code | Result | Verdict |
|---|---|---|
| `C0 23`, `C0 2C` | reply `C8 0F` (error) (11 Nov 2026) | not supported by the DA50ES; the CD-reference docs do not identify them either (`23` is documented as *unknown* on CD decks too) |
| `C0 2D` | no reply (11 Nov 2026) | not supported by the DA50ES |
| `C0 0A` / `C0 0B` tape loop | `C8 0F` (error) on TUNER, CD, MD/DAT, TAPE | **not supported** |
| `C0 0C` / `C0 0D` 5.1 input | verified by the owner on the front panel and the web interface | **works** |
| `C0 52 SS` second audio | `C8 0F` on TUNER, CD, MD/DAT (also with `0F`) | **not supported** |
| `C0 83 TT` / query `C0 43` input type | `C8 0F` on TUNER, CD, MD/DAT, TAPE, DVD/LD, SAT/TV, VIDEO 1 (values 1, 2, 4) | **not supported** |
| `C0 41` / `C0 42` / `C0 0E` queries | `C8 0F` | not supported |
| `C0 82 AA VV` | audio split: **error (`C8 0F`) on the DA50ES** (11 Nov 2026), with the unit on. Audio and video are separated with plain input selection instead: see `INPUT AV` | not supported; alternative implemented |
| `C0 81`, `C0 85`, `C0 86`, `C0 87` | no read-back exists (queries `41`–`47` are errors): not tried, a write could not be undone | **still to check** (see below) |
| `C3 84 NN` effect size | `CB 44 NN 03` (set ok), `C3 44` reads it back; restored | **works** (field 19, Small Hall; not available in field 29) |
| `C3 85 NN` reverb time | `CB 45 NN 03` (set ok), `C3 45` reads it back; restored | **works** |
| `C3 8B VV` wall type | `CB 4B VV 03`, `C3 4B` reads it back; restored | **works** |
| `C3 53 MM` speaker sizes (also the two-byte form `53 YY ZZ`) | no reply, `CB 70` still reports `0C`: nothing changes | **no effect** |
| `C3 0A`, `C3 0B` toggles | `CB 70` with the same field: nothing changes | **no effect** |
| `C3 22` | `CB 61 1B 01` (model info; it is a query, not a "type toggle") | works as a query |
| `C3 54`, `C3 95`, `C3 96`, `C3 97`, `C3 99`, `C3 9A`, `C3 9B`, `C3 9C` | no read-back (the queries are errors): not tried | **still to check** |
| `C3 A0` test tone | `CB 0F` earlier; audible: not repeated | not supported / skipped |
| `C1 04` / `C1 05` (tuning step, pclk-mn10) | `C9 0F` | **not supported** |
| `C0 C7`, `C3 C9` (pclk-mn10 queries) | `C8 0F`, `CB 0F` | not supported (`C2 D2`: no reply) |

**Still to check** (they change a setting that cannot be read back, so they cannot be undone exactly): display brightness `C0 86`, output level
`C0 87`, decode mode `C0 85`, video loop `C0 81`, audio split `C0 82`, EQ preset `C3 54`, speaker distances `C3 95`, rear placement `C3 96`,
crossover `C3 97`, front balance `C3 99`, dialogue/surround trim `C3 9A`/`C3 9B`, sub phase `C3 9C`. They need an explicit go-ahead.
The CD/MD commands cannot be tried: no CD changer or MD deck is on the bus (they are decoded on paper, see the Jukebox tab).

Consequences in the software: the catalogue flags were updated (`e` = error/no effect, `v` = verified), the web interface keeps the
recording-source and speaker controls but they report the error, and the configuration backup no longer tries to write input names, speakers or the
second audio source.

---

## Original list (kept for the sources)

Sources consulted: [hifi-remote.com Sony DSP](https://www.hifi-remote.com/sony/Sony_dsp.htm) (pages
[192 amplifier](https://www.hifi-remote.com/sony/Sony_dsp_192.htm),
[193 tuner](https://www.hifi-remote.com/sony/Sony_dsp_193.htm),
[195 sound field/EQ](https://www.hifi-remote.com/sony/Sony_dsp_195.htm),
[144 CD player](https://www.hifi-remote.com/sony/Sony_dsp_144.htm),
[sound fields](http://www.hifi-remote.com/sony/Sony_dsp_sf.htm)), [boehmel.de/slink.htm](https://www.boehmel.de/slink.htm),
[Ircama/Sony_SLink](https://github.com/Ircama/Sony_SLink), [fgoulding/ESP8266_Slink](https://github.com/fgoulding/ESP8266_Slink),
[nirvis.com CDJ](http://www.nirvis.com/help/CDJ/Advanced/how_to_control_receiver.htm),
[Bradley Univ. (BigDave control-a1.txt)](https://cegt201.bradley.edu/projects/proj2001/sonyntwk/SLink.PDF).
Not reachable: web.archive.org (reza.net/text.txt, controla1.html) and device.report (403).
The tables on hifi-remote are in decimal; here they are in hexadecimal. **The firmware already decodes all the codes below**
(the rows marked "yes" are verified on the STR-DA50ES, the others on paper only).

## Discovered today
| `C1 48 BB FF` → `C9 48 BB FF` + 8 characters | **RDS name of the tuned station** (BB = current bank; it must be requested, it is not sent spontaneously) | yes |
| Frame | Meaning | Verified |
|---|---|---|
| `C1 0E` → `C9 71 BB FF LV 01` | **signal level request** for the tuner (0–12) without retuning | yes |
| `C3 44` / `C3 45` → `CB 0E` | surround effect / reverb time: codes recognised but "not available" in sound field 29 | yes |
| `C3 51 NN` | names of sound fields 0–29 (see below, from the hifi-remote table; the order matches, with 0 = off and 29 = Auto Format Decode) | numbers yes, names not confirmed by ear |

## Sound fields (`C3 51 NN`, reply `CB 70 NN ..`)
0 Off · 1 Normal Pro Logic Surround · 2 Cinema Studio A · 3 Cinema Studio B · 4 Cinema Studio C · 5 Night Theater ·
6 Mono Movie · 7 Stereo Movie · 8 Multi Rear · 9 Multi Dimensional · 10 Virtual Theater A/B/C (10–12) ·
13–14 Virtual Enhanced A/B · 15 Virtual Semi-Multi Dimensional · 16–18 Virtual Semi-Theater A/B/C · 19 Small Hall ·
20 Large Hall · 21 Opera House · 22 Jazz Club · 23 Club/Disco · 24 Church · 25 Live House · 26 Arena · 27 Stadium ·
28 Game · 29 Auto Format Decode. The list "varies from model to model": on the DA50ES, 0–29 are accepted.

## Amplifier (`C0`) – to try (they change the state: try with mute on and restore afterwards)
| Command | Effect | Notes |
|---|---|---|
| `C0 0A` / `C0 0B` | tape loop IN / OUT | changes the recorder routing |
| `C0 0C` / `C0 0D` | 5.1 input ON / OFF | **verified** (see the check above) |
| `C0 52 SS` | second audio source (0 tuner, 2 CD, 3 DAT, 4 MD, 5 tape, 0F "source") | reply `C8 71` |
| `C0 83 TT` | input type: 0 auto, 1 optical, 2 coaxial, 3 AC3 RF, 4 analogue | reply `C8 43` |
| `C0 85 TT` | decoding mode: 0 auto, 1 AC3, 2 DTS, 3 MPEG, 4 PCM | not on all models |
| `C0 86 NN` / `C0 87 NN` | display brightness (19–46) / output level (0 low, 1 normal) | |
| `C0 88 SS + 8 characters` | changes the label of input `SS` | **tried on the DA50ES: no effect.** The command is recognised (null reply, whereas unknown codes give `C8 0F`) but the label read with `C0 48 SS` does not change, neither with 8 characters nor with 8 + 5 null bytes, neither on MD/DAT nor on VIDEO 3 |
| `C0 81 NN`, `C0 82 AA VV` | video loop / audio split | |
Queries already tried on the DA50ES: `C0 40`–`47` → error (not supported); `C0 48 SS` works (labels).
Absolute volume `C0 40 VV` / `C0 80 VV`: not supported / "not working" (confirmed: motorised volume knob).

## Surround/EQ (`C3`) – to try (they change the state)
| Command | Effect |
|---|---|
| `C3 54 n` | EQ preset (0 = off, 1–5) |
| `C3 53 MM` | speaker sizes (bitmap: 01 small front, 04/10 downgrade centre/rear, 08 no centre, 20 no rear, 40 no sub) |
| `C3 95 U F C R` | distances: unit (0 m, 1 feet), value + 7 for front/centre/rear |
| `C3 96 P [H]` | rear speaker position: 0 side, 1 centre, 2 behind, 3 high; height 0 low/1 high |
| `C3 97 F C R` | crossover (value × 10 Hz, 40–200 Hz) |
| `C3 84 NN`, `C3 85 NN` | effect size (0–20) and reverb time (0–16); in field 29 "not available" |
| `C3 8B VV` | wall type (−8 soft … +8 hard) |
| `C3 99 VV`, `C3 9A VV`, `C3 9B VV`, `C3 9C VV` | front balance, dialogue, surround (±5 dB in 0.5 steps), sub phase (0/1) |
| `C3 A0 MM` | test tone: 0 off, 16 auto, 32 left, 33 centre, 34 right, 35 surround right, 36 surround left, 37 sub (**audible**, avoid) |
| `C3 0A`, `C3 0B`, `C3 22` | toggle genre / effect-mode / surround type |
Queries `C3 48`, `C3 55`–`5C`, `C3 5D/5E`, `C3 60`, `C3 58`, tone EQ `C3 06/07`: error on the DA50ES.

## Tuner (`C1`)
Tried: `C1 06/07/08/09/0A/0B`, `C1 0C`/`0F` (status), `C1 50`, `C1 51`, `C1 0E` (level). Error: `C1 03` (band), `C1 52` (PTY → `C9 1B`),
`C1 70`, `C1 6A`. `C1 22` → `C9 61 07 03 1E 03 0A` (capabilities/model, undocumented format).

## CD/MD players (not present on the bus: decoded on paper only)
Addresses: CD `90`–`92` commands / `98`–`9A` replies (changer with up to 3 decks; according to Ircama `93`–`95` for commands on discs > 200, replies undocumented), MD `B0`/`B8`.
Commands: `00` play, `01` stop, `02` pause, `03` toggle pause, `08`/`09` track ± , `10`–`13` forward/backward (fast), `1F` resume,
`20`/`21` remote mode on/off, `22` capabilities, `25`/`26` verbose status on/off, `27` CD Text, `2E`/`2F` power on,
`40` disc memo, `44` disc info, `45` track info, `50 DD [TT]` play disc/track, `51` pause at disc/track, `5E`/`5F` fade, `6A` model name, `72` contents.
Replies: `00/01/02` status, `08` ready, `09` track start, `0C` ~30 s to the end, `18` tray open, `50 DD TT mm ss` (BCD) current track,
`52/54/58` disc shown/loading/loaded, `61` capacity (00 = 100 discs, FE = 200), `70` status (play mode bits).
Disc numbers: 01–99 in BCD, 100 = `00`/`9A`, 101–200 = value − 54 (from `9B` to `FE`).
Example from the documentation, decoded: `98 50 CA 03 03 48` = "CD1: disc 148 track 3 (3:48)".

## How to try things safely
Turn **mute** on (`MUTE`), note the state (`STATUS`, `EQ`, `INPUTS`), change a single value, read it back, **restore**.
`LOOPBACK ON` shows the transmitted frames as `RX`, which is useful to see how they are decoded.

---

# Historical sources (reverse engineering 1997–2000) and what came of them

Sources reached and read: [Tramm H.'s guide, 30 May 2000](https://sourceforge.net/p/jukebox-control/mailman/jukebox-control-l/thread/200005300224.UAA02294@inago.swcp.com/),
[CAV-50 draft of 12 August 2000](https://sourceforge.net/p/jukebox-control/mailman/message/398409/),
[message on CD/DVD/MD interfaces](https://sourceforge.net/p/jukebox-control/mailman/message/398314/),
[Reverse Engineered Control-A1 Codes (BigDave, hosted by Brian Patti on the MiniDisc Wiki)](https://www.minidisc.wiki/_media/resources/www.brian-patti.com_s-link_sonyvt4.pdf)
(identical to the text already used by the Bradley document) and the archive
[jukebox-control-release-0dot3](https://sourceforge.net/projects/jukebox-control/files/jukebox-control/0.3/) (Linux driver `slink.c` by Mike Muise / Tramm Hudson,
`doc/protocol-CD`, modules `Slink/A1.pm`, `MD.pm`, `CD.pm`, `hardware-howto`). **Not reachable:** web.archive.org (Jeff Behle's page, controla1.html),
the Parallax discussion (empty page for the player), the mailing list beyond the messages above, `cjuke/doc/amp-setup` (not in the 0.3 archive, only in CVS).
Note: UniLink (BUS ON/CLOCK/DATA) is a different Sony system and must **not** be used for Control-A1.

## Confirmations and useful new findings
| Information | Source | Effect on the firmware |
|---|---|---|
| First byte `nnnn X yyy`: type (9 CD, B MD, C amplifier), bit 3 = direction (1 = message from the device), bits 2–0 = id | Tramm, `A1.pm` | already used; confirmed |
| `0F` = error, **`0E` = "already done"** (not just "not available") | Tramm, `MD.pm` | the decoder now writes "unavailable / already done" |
| `C0 0F` requests the `70` status; fields of `C8 70 IN NAME AUTO+MUTE FF`: input, input name, automatic function (high nibble of byte 5: `2` = off, `0` = on), mute (low nibble: `1` normal, `3` mute), last byte "always FF" (on the DA50ES it varies per input: `B0`, `90`, `C1`, ...) | Tramm | decoded; the automatic function OFF (`CC` bit 5) is reported |
| Power-on frames `C8 61 ..`, `C9 61 ..`, `CB 61 ..` "not decoded"; `C8 62 15 80 00 00 33` seen on another amplifier | Tramm | confirm that the `61` frames remain model information; `62` does not appear on the DA50ES |
| `CB 70 01 00 00 00` = speaker speed/position and processing mode | Tramm | on the DA50ES `CB 70 NN 00 0C` (NN = sound field) |
| Full CD messages (`doc/protocol-CD`): `50` track, `51` "verbose" status (track, index, min, sec), `52` TOC read, `53` disc does not exist, `54` going to disc, `58` now at disc, `60` disc info (disc, index, tracks, min, sec, frame), `61` capacity, `62` track length, `70` mode (`PLAY PROG ? DISC TRACK`), `72/73` disc list (bitmap up to 104 / 105–200), `40` memo, `42` group, `10/13/14/15` errors | `protocol-CD` | **CD decoder rewritten**; real example from the mailing list `90 0f` → `98 70 01 c0 00 17 07` = "CD1: playing, disc 17, track 7" |
| MD messages/commands (`MD.pm`): `04` recording, `05` eject, `07` record, `0A` start rec, `0E` query, `23/24` auto-pause, `40` delete track, `44` disc info, `72` recording date, `98/99` disc title (14 + 16 characters per line), `9A/9B` track title, replies `58/59/5A/5B` texts, `70` mode (`4` rec, `5` rec-pause) | `MD.pm` | **MD decoder** updated |
| Linux driver: bit 0 = 600 µs, bit 1 = 1200 µs, sync = 2400 µs, delimiter 600 µs; frame ended by a pause > 4500 µs (END = 4200 µs of high line); **transmits only after > 6 ms of idle line** | `slink.c` | the firmware waits 3 ms; the Bradley document indicates 50 ms. Not changed: the volume knob calibration was done at this rate |
| Software timing was unstable (wrong bits on slow CPUs) | comments in `slink.c` | not applicable: the firmware uses the hardware RMT for transmission |
| Driver hardware: collector of a 2N2222 on the bus, base on a data pin | `hardware-howto` | matches the transistor variant in the wiring document |
| Tramm's technique: try command bytes one at a time and read the reply (`0F` error, `0E` already done) | Tramm's guide | used only for safe requests; full code scan rejected (risk of changing the state) |

## Tests on the DA50ES with this information
| Command | Reply | Outcome |
|---|---|---|
| `C0 22` | `C8 61 37 00 47 02 03` | **works**: request for model information (undocumented format) |
| `C0 21` | `C8 0F` | not supported (enable front panel) |
| `C0 25` / `C0 26` | `C8 0F` | not supported (verbose status) |
| `C1 25` / `C1 26` | `C9 0F` | not supported |
The state of the system (TUNER input, 88.50 MHz, sound field 29, mute off) remained unchanged.

## Input labels (test of `C0 88`)
`C0 88 SS` + text (8 characters, also with 5 null padding bytes as in the read) produces no reply and **does not change** the label
on the DA50ES: read back immediately afterwards with `C0 48 SS` it remains "MD/DAT" (input 04) and "VIDEO 3" (input 12). Variants tried: length 0, 1, 8 and 13 bytes.
The command seems not to be implemented (or is ignored) on this model; label changes must therefore be made from the amplifier menu.
A label can in any case contain at most 8 characters ("Freetz EVO" has 10).

## Labels: second test, with the amplifier in INDEX mode (11/2026)
- **Writing via S-Link (`C0 88 SS` + text): does not work on the DA50ES**, even with INDEX mode on at the front panel, with `SS` = `04` or `03`,
  with 8 characters or 8 + 5 null bytes, and with the frame sent twice (`REPEAT 2`): no reply, label unchanged.
- **Reading (`C0 48 SS`): works and reflects changes made from the front panel** (verified: after the manual change the label read became
  "CDEFGHIJ", then "BDEFGHIJ"). Codes `03` and `04` share the same label.
- **Changing the name from the front panel transmits nothing on the bus**: in 3 minutes of passive listening, during the change, no frame
  about the name appeared (only two video state changes `C8 70 04 19 11 B0` / `C8 70 04 10 01 B0` made separately).
- Manual procedure (STR-DA50ES, p. 45, "Indexing Preset Stations and Program Sources"): select the source, press CURSOR MODE until
  INDEX lights up, choose the characters with the up/down arrows, advance with right; the name is saved automatically (up to 8 characters; the space is between "11" and "A").
  Source: [manual on ManualsLib](https://www.manualslib.com/manual/247787/Sony-Str-Da50es.html?page=45). On newer models the function is called "NAME IN" (AMP menu).
- The `88` command remains documented by [boehmel.de](https://www.boehmel.de/slink.htm) and [Ircama/Sony_SLink](https://github.com/Ircama/Sony_SLink) with no reply and no prerequisites:
  it is probably implemented only on newer amplifiers. A `LABEL` command was not added to the firmware.

## `NAME` command (RDS) – clarification
`NAME` replies only with the **TUNER** input active, on an **FM** station with RDS and using the **current bank** (read from the status): with other inputs
the tuner replies `C9 0E`. The firmware now re-reads the status first, tries 6 times (RDS can take a few seconds) and distinguishes the cases:
"select the TUNER input", "AM station" or "no RDS name after 6 attempts (weak signal or station without RDS)".

## Label changed from the front panel but not visible on the display (tests of November 2026)
- After changing the MD/DAT name from the front panel (INDEX), `C0 48 04` / `C0 48 03` return the new name ("BDEFGHIJ"), but the display still
  shows "MD/DAT", even after reselecting the input (TAPE → MD) and pressing DISPLAY several times.
- The DISPLAY button, the name change and the manual input change do not generate label frames on the bus (passive listening: 0 frames in the DISPLAY test).
- There is no S-Link command (in the tables found) to choose the display mode; the only display command is `C0 86 NN` (brightness).
- Unverified hypotheses: the INDEX name is shown by the DA50ES only for certain sources, or MD/DAT uses a fixed name on the display and the index stays only
  in the memory read via S-Link. Suggested test: change the VIDEO 1 name in the same way and see whether that one appears on the display.
