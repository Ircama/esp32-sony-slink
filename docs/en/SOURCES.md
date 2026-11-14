# Sources audit

Status of each source listed for this project (what was read, what was used, what could not be read).

| Source | Status | Used for |
|---|---|---|
| [hifi-remote.com Sony DSP](http://www.hifi-remote.com/sony/Sony_dsp.htm) + pages 144/192/193/195/200/201/203, `eq`, `sfp`, `eqp` | read | command and reply tables, decoding of `C8 70`, `CB 70`, `C9 61`, EQ frequency tables, IR form of the protocol |
| [minidisc.org hacking](https://www.minidisc.org/part_hacking.html) | index read | pointers only (no S-Link bytes) |
| [minidisc.org PC/Mac link kits](https://www.minidisc.org/part_Mac+PC_to_MD_Link_Kits_Sony.html) | read | MD Link kit background (PC-Link is a different serial protocol) |
| [journaldulapin MDS-PC3 PC-Link](https://www.journaldulapin.com/2021/10/09/mds-pc3-pclink/) | read | PC-Link hardware background; no S-Link protocol detail |
| [Home Assistant thread "Sony S-Link and ESPHome"](https://community.home-assistant.io/t/sony-s-link-and-esphome/803525) and the repositories it links | read | timing, `70` device ID, wake-up pulse idea (long-pulse counter in Statistics) |
| Facebook group 373264593235555 | not readable (login required) | – |
| [reddit r/minidisc post](https://www.reddit.com/r/minidisc/comments/1fgb0w0/recording_with_cdtext_link_to_a_video_in_post_text) | not readable (blocked) | – |
| [Sony manual W0001475M.pdf](https://www.sony.com/electronics/support/res/manuals/W000/W0001475M.pdf) | not readable (blocked); the RM-TP501 manual was supplied locally and is not part of the repository | see `RM-TP501.md` |

Other sources are listed in `RM-TP501.md`.

## Second review: CD/MD tables, caelor, robho, JSa1987, pclk-mn10

| Source | Used for |
|---|---|
| [undeadscientist S-Link CD Command / Response Reference](https://web.archive.org/web/20070515230656/http://www.undeadscientist.com/slink/slinkcmds.html) ([responses](https://web.archive.org/web/20070704164051/http://www.undeadscientist.com/slink/slinkrsps.html)) and the S-Link Device for Linux page (saved locally, not redistributed) | the CD command list (all 62 codes are in the catalogue, including the ones documented as *unknown*: `23 29 30 31 47 49 81 84 91 E0`) and the full CD reply set; device addresses `90/91/92` + `93/94/95` (discs above 200) + `97` (all players), replies `98/99/9A` + `9B/9C/9D` |
| [boehmel.de/slink.htm](https://www.boehmel.de/slink.htm) (saved copy) | MD tables, `E1` status (bit 2 of the 2nd byte: 0 = after an S-Link command, 1 = after an infrared one) |
| [caelor/sony-a1-hass](https://github.com/caelor/sony-a1-hass) `doc/messages.md`, `doc/low_level.md` | the bitmask meaning of the `70` status byte S1 (bits 0-2 transport, bit 4 power off, bit 5 no disc: `23`/`33` seen on a deck without a disc), MD S2/S3 layout (shuffle, program, repeat, writable, input analog/optical/coax, mono), MD replies `31`, `33`; the 5 V TTL open-collector description |
| [JSa1987/SLink-HT-ESPHome](https://github.com/JSa1987/SLink-HT-ESPHome) | its CDP-CX225 command sheet is the same list as the undeadscientist one: nothing new |
| [robho/sony_slink](https://github.com/robho/sony_slink) | the `70xx` prefix of newer amplifiers (already supported); no new codes |
| pclk-mn10 command list | amplifier `C7` (query volume) / `C8` (set volume, mute), surround `C9`, tuner `04`/`05`, MD `28 48 57 C0`; the first two answer *error* on the STR-DA50ES |
| SIRCS (Control-S) and LANC pages | other Sony protocols: not S-Link, nothing to add |

What changed in the software: the decoder now reads the CD/MD `70` status as defined by those tables (it applied the CD layout to MD and mixed up the program numbers), decodes the CD Text, memo, group, artist, bar code and deck-capacity replies, and the catalogue grew by 19 rows (245 commands).

## Third review: the original Control-A1 notes and the ESP8266 port (14 November 2026)

Four more sources were read in full (the three Internet Archive pages were reachable this time; earlier notes in `CODES_TO_TRY.md` say they were not).
The decoder and the command catalogue already contained almost everything in them (the same material reached them through the pages listed above), so
the review was a line-by-line comparison; what was missing was added.

| Source | What it is | Result |
|---|---|---|
| [Control-A1 page of the Jukebox project, Univ. of Wisconsin](https://web.archive.org/web/20030414231523/http://www.upl.cs.wisc.edu/~orn/jukebox/controla1.html) (`controla1.html`) | the earliest write-up: oscilloscope notes (bus idles at 4.72 V, bit time about 600 µs), the "count the zeros" decoding of the captured bit streams, then the CD changer messages seen for address 3 (`9A ..`) and a command table `00`–`5F` | all commands and replies were already decoded. Added: `45 DD 00` = length of the track being played (track 00 is the current one, not a track number), pause-cue wording for `03`, wording of `52` and `0F` |
| [Slink Send and Receive Commands for Sony MiniDisc Recorders](https://web.archive.org/web/20030419100153/http://web2.airmail.net/will/mdslink.txt) (William Hollingworth, 11 April 1999, MDS-JB920 driven with a Slink-e) | every MD (`B0`) command the deck answers and every message it sends, with the bit layout of the `70` mode message | all known commands and replies were already decoded, including the `70` layout. Added to the decoder and to the Console catalogue: the commands that answer but have no known function (`29`–`2D`, `5C 00 01` answered by `4C`, `B2`), and the reply `05` now reads "eject / ignored" (this source says *eject*, `caelor/sony-a1-hass` says *ignored*) |
| [Reverse Engineered Control-A1 codes, CDP-CX250](https://web.archive.org/web/19981202175957/http://www.cc.gatech.edu/people/home/bigdave/cdplayer/control-a1.txt) (BigDave, 19 February 1998) | CD changer addresses (`98`/`99`/`9A` replies, `90`/`91`/`92` commands), disc number coding (BCD to 99, then hex minus 54), the bit fields of the `70` status, the `61` capacity message, power-up message sequences | already decoded (disc numbers, `70`, `61`). Added: capacity codes `50`/`51` shown as "50(+1) discs (speculated)", the wording BigDave gives |
| [fgoulding/ESP8266_Slink](https://github.com/fgoulding/ESP8266_Slink) (Tyler Goulding, 2020, CC-BY-SA 4.0) | an Arduino/ESP8266 sender built from Ircama's `Sony_SLink` library, timer driven | no new sequences: its device IDs (amplifier `C0`/`70`, tuner `C1`, surround `C3`, CD `90`–`95` and `97` for all players, MD `B0`) and command numbers are the ones already decoded. Its timings (sync 2400 µs, one 1200, zero 600, delimiter 600, 30 ms between frames) are the same as `main/slink.h`. BigDave's 2500/1240/650/625 µs are second-hand figures he had not measured. No code was copied. One thing to know if you reuse it: in `sendCommand`, the fourth byte is filled with `commandId2` instead of `commandId3`, so three-parameter commands go out wrong |

Everything taken from these pages is marked as *not verified here* in the code, like the rest of the CD/MD decoding: no CD changer or MD deck is on the bus
used for the tests.

