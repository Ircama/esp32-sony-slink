# Sony RM-TP501 vs this bridge: what the remote does with the STR-DA50ES, and what this software covers

This compares the control the Sony **RM-TP501** touch-panel remote has over the STR-DA50ES with what the bridge offers. The remote's
infrared codes for other brands (its second job) are out of scope. Sources: the RM-TP501 operating instructions (3-864-784-11(1), 1998;
a local copy is kept outside the repository: it is Sony's copyrighted document), the public Sony DSP/Control-A1 tables and the other sites listed at the end.

**Reference remotes.** The Sony touch-panel remotes taken as the reference for the functions of this software are the **RM-TP501**, the 
**RM-TP501E**, the **RM-TP502**, the **RM-TP503**, the **RM-TP504** and the **RM-TP1**. Only the RM-TP501 manual was available and analysed for this
comparison; for the others nothing is claimed here beyond their being remotes of the same kind, so every function listed below is the RM-TP501's.

## How the RM-TP501 talks to the amplifier

- The manual (p. 4): *"This unit is a bidirectional remote commander. Not only does it transmit infrared rays for operation of the receiver,
  it can also receive infrared rays emitted by the receiver to display characters and other information supplied by other components
  connected to the receiver with a CONTROL A1 cord."* So the **two-way link is infrared**, between the remote and the receiver (initial
  communication, p. 6; it must be pointed at the receiver's display). S-Link / CONTROL A1 is only the wired link between the receiver and
  the other units (CD changers, MD decks).
- [hifi-remote.com](https://www.hifi-remote.com/sony/Sony_dsp.htm) documents that bidirectional IR channel (it calls it the *Sony DSP* protocol)
  and states that **it is the IR version of the wired Control-A1 protocol**: a Sony 15-bit command (device 36, command 96) marks a DSP message,
  then a variable number of 8-bit bytes (LSB first: device, command, parameters), then a checksum (XOR of the bytes plus the byte count).
  The device/command/reply codes of the DSP tables (`192/193/195/144`, replies `200/201/203`) are therefore the **same messages the bridge
  sends and receives on the S-Link bus** (`C0/C1/C3`, `C8/C9/CB`; `70` instead of `C0` on newer amplifiers: "subtract 80 for AV2").
- Consequence: anything the RM-TP501 can read or change on the amplifier through that channel is reachable from the bus with the same
  bytes. What the remote does by plain Sony IR (SIRC) key codes (for example memorising a preset, the sleep timer) is **not** in that
  message set and cannot be done over S-Link.

## What the remote can do (manual) and where the bridge stands

"✔" = covered, "new" = added by this review, "–" = not possible over S-Link / not applicable.

| RM-TP501 function (manual page) | S-Link message(s) | Bridge |
|---|---|---|
| Select function / input (11, 14) | `C0 50 SS` | ✔ Listen › Source |
| Components connected to the receiver shown on the FUNCTION screen (11, 19) | `C8 70 AA AV CC XX`: `XX` = S-Link address of the unit on that input | **new**: decoded and shown (badge, Statistics) |
| 5.1 INPUT function (23) | `C0 0C` / `0D`, flag in `C8 70` | **new**: Listen › 5.1 input switch; the sound field is disabled while it is on (as on the remote). **Verified on the STR-DA50ES** |
| VOL +/−, MUTING (9) | `C0 14/15`, `06/07` | ✔ and better: safety limit, 3-scale dial, fixed levels |
| SYSTEM/STANDBY: ALL OFF / receiver only (9, 21) | `2F` to every unit / `C0 2F` | **new**: `ALLOFF` and Listen › Turn off all units; ✔ power |
| SLEEP (9) | not in the DSP message set (receiver IR key) | – |
| CD / MD operation and LIST of disc/track titles (13) | `90/91/92`, `B0` commands; titles `40/44/45/48/4A/98` | ✔ every command in the catalogue and decoded; ✘ no titles list UI: no player on this bus to test |
| REC EDIT: choose player and recorder (15) | `C0 52 SS` (2nd audio = recording source), `C0 50` | **new**: Listen › Recording source (value from `C8 71`; the DA50ES answers `C0 0E` with an error, so it cannot be queried) |
| Tuner: AM/FM, PRESET +/−, TUNING +/−, DIRECT (14) | `C1 50/51/06/07/08/09` | ✔ Radio (tuning by band, seek incl. downward, presets, direct entry) |
| Tuner: SHIFT memory pages A/B/C | `C1 51 BB NN`, `C1 4A` | ✔ Radio › presets of the amplifier (banks) |
| Tuner: MEMORY (store a station) and station names | plain IR keys of the receiver | – not possible over S-Link; the bridge keeps its **own 30 presets** (`FAV`) and reads RDS names live, which the remote cannot |
| Sound field: on/off, list by genre (16) | `C3 04/05/51` | ✔ Sound › Sound field |
| EQ EDIT: speaker × BASS/MID/TREBLE × GAIN/FREQUENCY (17) | `C3 50` (read), `C3 90` (set) | ✔ Sound › Equaliser (front/rear, draggable curve) |
| SURROUND EDIT: EFFECT, WALL TYPE, REVERB TIME, LFE MIX, D.RANGE COMPRESSOR (17) | `C3 84/8B/85/87/86` (queries `44/4B/45/47/46`) | ✔ effect, reverb, LFE, compression; **new**: wall type (`C3 4B` query inferred from the reply `CB 4B` and the 4x/8x pattern) |
| LEVEL: speaker levels (18) | `C3 80..83` (queries `40..43`) | ✔ Sound › Surround levels (rear, centre where available, subwoofer, balance) |
| LEVEL: TEST TONE (18) | `C3 A0` | catalogue only: the STR-DA50ES answers `CB 0F` (error); a precondition (a "level adjust" mode) is probably needed and is not documented |
| LEVEL: BASS BOOST (18) | no code in any public table | – unknown |
| Items greyed out by speaker settings / sound field (17, 18) | replies `CB 0E` / `CB 0F` | ✔ controls show "n/a" and are disabled |
| Speaker settings that grey items out | `CB 70 .. SS` (read), `C3 53` (set) | **new**: Sound › Speakers shows the configuration read from the amplifier and can change it |
| RELOAD (8, 17, 18) | re-read everything | ✔ **new**: *Refresh* reads status, 2nd audio, speakers, EQ, levels and labels again |
| SETUP › DOWNLOAD: FUNCTION NAME, TUNER PRESET NAME, CD MEMO (20) | `C0 48 SS` (+`6A/6B`), `C1 4A/48`, CD `40/48/98` | ✔ input labels and tuner preset names (`INPUTS`, `PRESETS`, `SETUP`); CD memo as above |
| Touch panel, LCD auto-off, beep, alarm, FUNCTION HOOK UP, ALL CLEAR, registering non-Sony components (18–21) | remote-side only | – not applicable |

**Verdict.** For everything the remote can read or change on the amplifier over the DSP/Control-A1 message set, the bridge is now **equal or
better**, with four exceptions: BASS BOOST (no known code), TEST TONE (the amplifier rejects the documented code), the CD/MD title lists
(no player on the bus to test) and the functions that the remote performs with plain IR keys (memorising presets, sleep timer). Beyond the
remote it adds the 226-command catalogue, REST/WebSocket/MQTT/TCP access, RDS names, statistics and automation.

## Decoding added from the public reply tables

The reply pages hifi-remote Sony_dsp_[200](https://www.hifi-remote.com/sony/Sony_dsp_200.htm),
[201](https://www.hifi-remote.com/sony/Sony_dsp_201.htm) and [203](https://www.hifi-remote.com/sony/Sony_dsp_203.htm) were not used before:

| Reply | New decoding |
|---|---|
| `CB 70 FF PP SS` | `PP` sound field preset, `SS` speaker configuration bitmap (on the DA50ES `1D 00 0C` = field 29, preset 0, **no centre speaker**) |
| `C8 70 AA AV CC XX` | `CC` bits: tape loop (`08`), 5.1 input (`10`), auto-function off (`20`); `XX` = S-Link address of the unit believed to be on that input (`B0` MD, `90` CD 1, `C1` tuner, `FF` none) |
| `C9 70 .. FL ..` | `FL` bit `10` = seeking down, `20` = seeking up (the interface now shows both) |
| `C9 61 07 03 1E 03 0A` | tuner capacity: 30 presets, 3 banks of 10 |
| `CB 4B WW` | wall type, −8 soft … +8 hard |
| `C8 05`, `C9 05` | "ignored" |
| `C8 40 VV` | volume level (not sent by the DA50ES) |

Also from those pages: the frame format of the IR version (checksum rule above) and the EQ frequency tables per position and band.

## Other sites on Control-A1 / S-Link

| Source | What it adds |
|---|---|
| [hifi-remote.com Sony DSP](https://www.hifi-remote.com/sony/Sony_dsp.htm) and its pages 192, 193, 195, 144, 200, 201, 203, `eq`, `sfp`, `eqp` | the command and reply tables; the IR form of the protocol |
| [boehmel.de/slink.htm](https://www.boehmel.de/slink.htm) | waveform, MD/amplifier/tuner/surround tables incl. the commands with unidentified functions |
| [Ircama/Sony_SLink](https://github.com/Ircama/Sony_SLink) | Arduino library; `70` device ID of the STR-DA1000ES; volume timing note |
| [fgoulding/ESP8266_Slink](https://github.com/fgoulding/ESP8266_Slink) | timer-based ESP8266 port |
| [robho/sony_slink](https://github.com/robho/sony_slink), [robho/hass_sony_slink](https://github.com/robho/hass_sony_slink) | Arduino/ESP8266 bridge and Home Assistant component (tested on an STR-DB2000; `70xx` frames for newer amplifiers) |
| [hajdbo/sony-jukebox-slink](https://github.com/hajdbo/sony-jukebox-slink) | CDP-CX jukebox control (Jeff Behle's reverse engineering) |
| [Sony S-Link Resource Center](http://www.brian-patti.com/s-link/) | index of BigDave's Control-A1 notes and CD-changer code lists (the page was not reachable for this review) |
| [hifi-wiki Control A1/A1II](https://hifi-wiki.com/index.php/Control_A1/A1II) | which CD players use A1 or A1II; 3.5 mm mono plug |
| [Sony S-Link gateway (Bradley Univ.)](https://cegt201.bradley.edu/projects/proj2001/sonyntwk/SLink.PDF) | hardware layer and waveforms |
| [RemoteCentral Sony receiver library](https://files.remotecentral.com/library/22-1/sony/receiver/index.html) | the separate RS-232 / IP control of later STR-DAxxxES models (not S-Link) |

No site documents a reverse engineering of the RM-TP501 itself beyond the hifi-remote DSP pages above.

## Checked on the amplifier (STR-DA50ES, sound field off, muted)

Read-only queries sent through the bridge:

| Query | Reply | Meaning |
|---|---|---|
| `C0 0F` | `C8 70 00 16 03 C1` | TUNER / SAT-TV, power on, **mute on** (`CC` = `03`); `XX` = `C1`: the unit linked to the TUNER input is the tuner |
| `C0 22` / `C0 6B` | `C8 61 37 00 47 02 03` / `C8 6B 02` | model and device info |
| `C0 0E` | `C8 0F` (error) | the 2nd audio source cannot be queried on this amplifier |
| `C1 0C` / `C1 22` | `C9 70 03 FF 48 23 32` / `C9 61 07 03 1E 03 0A` | tuner at 90.10 MHz, locked, stereo; capacity 30 presets, 3 banks of 10 |
| `C3 0F` | `CB 70 00 00 00` | sound field off; the speaker byte is not reported in this state |
| `C3 22` | `CB 61 1B 01` | surround model info |
| `C3 47` | `CB 47 00 01` | LFE mix level 0 |
| `C3 4B` | `CB 0E` | wall type: *unavailable* (not *error*) with the field off, so the query is recognised, like effect size / reverb time (`C3 44/45` also `CB 0E`) |
| `C3 61`, `C0 61` | `CB 0F`, `C8 0F` | `61` is a reply code, not a query |

Still to be confirmed with a sound field **on** (it changes the setting, so it was not done without asking): the speaker byte in `CB 70`
(`1D 00 0C` was seen earlier), the wall-type value in `CB 4B`, and the set commands `C0 52` and `C3 53`, which stay marked *untested* (`C0 0C/0D`, the 5.1 input switch, has been verified).
