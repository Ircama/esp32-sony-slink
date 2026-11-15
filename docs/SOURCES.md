# Sources

The documents, projects and pages used for the S-Link / Control-A1 decoder, the command catalogue and the other protocols of the firmware: what each one is and what was taken from it. Everything taken from the CD and MD documents is marked as *not verified here* in the code: no CD changer or MD deck is on the bus used for the tests (the author's amplifier is a STR-DA50ES). More sources are listed in [`RM-TP501.md`](RM-TP501.md).

## Command and reply tables

| Source | What it is | Used for |
|---|---|---|
| [hifi-remote.com Sony DSP](http://www.hifi-remote.com/sony/Sony_dsp.htm) and its pages 144 (CD), 192, 193, 195, 200, 201, 203, `eq`, `sfp`, `eqp` | the infrared form of the protocol and the command / reply tables | the base of the decoder and of the catalogue: decoding of `C8 70`, `CB 70`, `C9 61`, EQ frequency tables. The numbers on the CD page are **decimal** ("64 query disc memo", "68 query disc info", "69 query track info" are the hexadecimal `40`, `44`, `45`, replies `40`/`80`, `60`, `62`) |
| [Sony S-Link gateway](https://cegt201.bradley.edu/projects/proj2001/sonyntwk/SLink.PDF) (Rolf Eigenheer, 1998, hosted by Bradley University) | waveforms (idle 5 V, 44 mA short-circuit limit, multiples of 600 µs), part of BigDave's list, his own commands, the design and 8051 code of an RS232/RS485 gateway | message examples (`98 50 CA 03 03 48` = changer 1, playing, disc 148, track 3, 3:48; `98 50 62 14 02 14` = disc 62, track 14, 2:14); status byte `0E` read as "power is off" for the changer; `20`/`21` = display off/on. The document gives `08` and `09` both as "next track": a typo, `09` is previous. Timing: a gap above 2700 µs is a sync, bit counts are multiples of 8, a message is complete after 50 ms of silence and the sender starts 50 ms after the last incoming bit (the firmware sends 30 ms after a frame start, `SLINK_FRAME_MIN_US`) |
| [Control-A1 page of the Jukebox project, Univ. of Wisconsin](https://web.archive.org/web/20030414231523/http://www.upl.cs.wisc.edu/~orn/jukebox/controla1.html) | the earliest write-up: oscilloscope notes (bus idles at 4.72 V, bit time about 600 µs), "count the zeros" decoding, the CD changer messages for address 3 (`9A ..`) and a command table `00`–`5F` | `45 DD 00` = length of the track being played (track 00 is the current one), pause-cue wording for `03`, wording of `52` and `0F` |
| [Reverse Engineered Control-A1 codes, CDP-CX250](https://web.archive.org/web/19981202175957/http://www.cc.gatech.edu/people/home/bigdave/cdplayer/control-a1.txt) (BigDave, 1998) | CD changer addresses (`98`/`99`/`9A` replies, `90`/`91`/`92` commands), disc number coding, the bit fields of `70`, the `61` capacity message, power-up sequences | disc numbers (BCD to 99, then hex minus 54), `70`, `61`; capacity codes `50`/`51` shown as "50(+1) discs (speculated)". His timings (2500/1240/650/625 µs) are second-hand figures |
| SourceForge message [398382](https://sourceforge.net/p/jukebox-control/mailman/message/398382/) (Tramm Hudson, "Guide to Reverse Engineering Slink devices", 2000) | the address byte and the scan technique (the site answered 403: the text was supplied by the user) | address byte `type(4) \| direction(1) \| id(3)` (9 CD, B MD, C amplifier); `C8 2F`/`C8 2E` power off/on; `C8 61`/`C9 61`/`CB 61` model information; `C8 70 AA AV CC FF` (status requested with `C0 0F`); `CB 70` sound field / speakers; `0F` = error, `0E` = already done |
| pclk-mn10 command list (`commands.json`, M-Crew PCLK-MN10 adaptor) | commands of a PC-Link adaptor | amplifier `C0 C7` (query volume) and `C0 C8` (volume / mute: both answer *error* on the STR-DA50ES), surround `C9`, tuner `04`/`05`, MD `28 48 57 C0` (`B0 C0 xx` forced remote-control modes: it locks the recorder until reboot, so there is no button for it) |
| [Sony RM-TP501 manual](https://www.sony.com/electronics/support/res/manuals/W000/W0001475M.pdf) | the remote this project replaces | see [`RM-TP501.md`](RM-TP501.md); the site blocks the download, the manual was supplied locally and is not part of the repository |

## CD players and changers

| Source | What it is | Used for |
|---|---|---|
| [undeadscientist S-Link CD Command / Response Reference](https://web.archive.org/web/20070515230656/http://www.undeadscientist.com/slink/slinkcmds.html) ([responses](https://web.archive.org/web/20070704164051/http://www.undeadscientist.com/slink/slinkrsps.html)) and the S-Link Device for Linux page (saved locally, not redistributed) | the CD command list and the full reply set | all 62 CD codes are in the catalogue, including the ones documented as *unknown* (`23 29 30 31 47 49 81 84 91 E0`); device addresses `90/91/92`, `93/94/95` (discs above 200), `97` (all players), replies `98/99/9A` and `9B/9C/9D` |
| [jaikamat/cdp-cx355](https://github.com/jaikamat/cdp-cx355) | Arduino controller for a CDP-CX355 (disc 1-200 on `90`, 201-300 on `93`) | everything it uses was already decoded: disc BCD 1-99 and +54 for 100-200, `00/01/02/08/09`, `2E/2F`, `50` disc+track, `40` title, `80` memo, `0F` status `98 70 00 SS 00 DD 00`, `25/26` timing frames, replies `98 0E`, `14`, `52/58`, `0C`, `61`. It confirms the memo is **13 characters**; reply `0E` also means a memo write refused on a CD TEXT disc |
| Jukebox log (`90 80 41 <14 characters>` fails) | a Linux jukebox driver sending a 14-character memo with no padding | consistent with the 13-character memo: the web page sends 13 padded characters |
| [Crestron CIS-200 interface](https://applicationmarket.crestron.com/content/Help/Sony/cis-200.pdf) | module description for three Control-A1 CD players behind a serial interface | no packet dump. Confirms unit selection 1–3 and the basic commands. Numbers: `01`–`99` = discs 1–99, `00` = disc 100, `9A`–`FE` = discs 100 to 200; tracks in BCD (track 10 is `10`, not `0A`) |
| [Parallax: "Working S-Link code (long)"](https://forums.parallax.com/discussion/70973/working-s-link-code-long) (archived copy; the live page asks for a captcha) | a BASIC Stamp sender, commands verified on a CDP-CX200: `2E`, `2F`, `00`, `01`, `08`, `09`, `02`, `03`, `10`–`13`, `20`, `50` | already decoded. Track numbers ≥ 10 "plus 6" are BCD. It sends every command twice in case of a collision with a reply and pads frames to 45 ms |
| [Sony CDP-CX450 service manual](https://rcaudio.rclabs.com.br/library/docs/Schematics%2C%20Service%20Manuals%20and%20Catalogs/Sony/Sony-CDP-CX450-Service-Manual.pdf) | 200-disc changer, appendix "Control A communications" | no message bytes, only behaviour: Control A (50/100-disc, stereo plug), A1 (200-disc, mono plug), A1 II (300/400-disc, 2000); a 3-position switch selects the unit number; automatic function selection (CD play switches the receiver to the CD input); synchronised recording; the next disc is loaded and held in pause 30 s before the current track ends (probably what `54`/`58` after `0C` show: an inference); infrared commands received by one device are forwarded on the bus to all others |

## MiniDisc recorders

| Source | What it is | Used for |
|---|---|---|
| [boehmel.de/slink.htm](https://www.boehmel.de/slink.htm) (saved copy) | MD tables | MD commands and replies; `E1` status (bit 2 of the 2nd byte: 0 = after an S-Link command, 1 = after an infrared one) |
| [Slink Send and Receive Commands for Sony MiniDisc Recorders](https://web.archive.org/web/20030419100153/http://web2.airmail.net/will/mdslink.txt) (William Hollingworth, 1999, MDS-JB920 driven with a Slink-e) | every MD (`B0`) command the deck answers and every message it sends, with the bit layout of `70` | all known commands and replies; the commands that answer but have no known function (`29`–`2D`, `5C 00 01` answered by `4C`, `B2`); reply `05` reads "eject / ignored" (this source says *eject*, `caelor/sony-a1-hass` says *ignored*) |
| [caelor/sony-a1-hass](https://github.com/caelor/sony-a1-hass) (`doc/messages.md`, `doc/low_level.md`) | Home Assistant integration and message notes | bitmask of the `70` status byte S1 (bits 0-2 transport, bit 4 power off, bit 5 no disc), MD S2/S3 layout (shuffle, program, repeat, writable, input analog/optical/coax, mono), MD replies `31`, `33`; the 5 V TTL open-collector description |
| [minidisc.org hacking](https://www.minidisc.org/part_hacking.html) and [PC/Mac link kits](https://www.minidisc.org/part_Mac+PC_to_MD_Link_Kits_Sony.html) | MD background | pointers only; MD Link kits use PC-Link, a different serial protocol |
| [journaldulapin MDS-PC3 PC-Link](https://www.journaldulapin.com/2021/10/09/mds-pc3-pclink/) | PC-Link hardware | background only, no S-Link detail |

## Implementations for other platforms

| Source | What it is | Used for |
|---|---|---|
| [Ircama/Sony_SLink](https://github.com/Ircama/Sony_SLink) | the author's Arduino library | timings and device IDs |
| [fgoulding/ESP8266_Slink](https://github.com/fgoulding/ESP8266_Slink) (Tyler Goulding, 2020, CC-BY-SA 4.0) | an ESP8266 sender built from the library above, timer driven | no new sequences. Its timings (sync 2400 µs, one 1200, zero 600, delimiter 600, 30 ms between frames) are the ones of `main/slink.h`. No code was copied. If you reuse it: in `sendCommand` the fourth byte is filled with `commandId2` instead of `commandId3`, so three-parameter commands go out wrong |
| [Home Assistant thread "Sony S-Link and ESPHome"](https://community.home-assistant.io/t/sony-s-link-and-esphome/803525) and the repositories it links | ESPHome work on S-Link | timing, the `70` device ID, the wake-up pulse idea (long-pulse counter in Statistics) |
| [JSa1987/SLink-HT-ESPHome](https://github.com/JSa1987/SLink-HT-ESPHome) | ESPHome for a home theatre | its CDP-CX225 command sheet is the same list as the undeadscientist one |
| [robho/sony_slink](https://github.com/robho/sony_slink) | S-Link implementation | the `70xx` prefix of newer amplifiers (supported); no new codes |

## Slink-e serial protocol

[woudy.org/slinke, low-level pages](http://www.woudy.org/slinke/help/fp/Slinke/lowlevel/lowlevel.htm) (Nirvis Systems): the host protocol of the Slink-e interface (port send / receive, port enable, version, serial number, baud ...), not the bus. Implemented over TCP on port 2325, off by default: see the README, "Slink-e protocol over TCP". The IR, parallel and S-Link bit-mode commands are not implemented.

## Other protocols (not S-Link)

Read and set aside: nothing to add to the decoder.

| Source | What it is | Why it does not apply |
|---|---|---|
| [ukigumo1104, note.com](https://note.com/ukigumo1104/n/n81333e7ec999), SIRCS (Control-S) and LANC pages | Sony **infrared** (SIRC) codes, Control-S, LANC | other Sony protocols (IR: 600 µs unit, 12/15/20 bits) |
| [Sir68k/pclink-i2c](https://github.com/Sir68k/pclink-i2c) | I2C bus of the HCD-J300 hi-fi system (PC-Link): `20 90 54 00 01` opens the CD tray, `B0` = MD board | PC-Link, not S-Link. `90 54 00 01` ("open tray") has a decoder text flagged **PC-Link only, not verified on S-Link changers** (on S-Link `54` is the MD "remaining time" command and the CD reply "loading disc"); the firmware never sends it |
| [ginge/sony_md_remote](https://github.com/ginge/sony_md_remote) | library for the wired *LCD remote control* port of portable MiniDisc players (MZ-R55, MZ-R70, MZ-R90) and the RK-TXT1 link | one wire with sync and tri-state handshaking, display text: a different protocol |

## Not readable

| Source | Reason |
|---|---|
| Facebook group 373264593235555 | login required |
| [reddit r/minidisc post](https://www.reddit.com/r/minidisc/comments/1fgb0w0/recording_with_cdtext_link_to_a_video_in_post_text) | blocked |
| AVS Forum thread on Slink-e with Girder / MainLobby | the site asks for a payment before showing it; nothing was paid or bypassed |
