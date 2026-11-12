"""Generates docs/en/WEB_COVERAGE.md: which documented S-Link commands, firmware commands and S-Link replies the web interface
supports. The command table is the CATALOG in main/web/index.html (the same data drives the "Documented commands" panel of the
Console tab), so the document cannot drift from the page:

    python tools/gen_coverage.py
"""
import io
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html = open(os.path.join(root, "main", "web", "index.html"), encoding="utf-8").read()
cat = re.search(r"const CATALOG = `(.*?)`;", html, re.S).group(1).strip().split("\n")
items = []
for line in cat:
    d, code, name, params, flags = (line.split("|") + [""] * 5)[:5]
    items.append(dict(dev=d, code=code, name=name, params=[p for p in params.split(";") if p], flags=flags))

DEV = {"AMP": ("C0", "Amplifier"), "TUN": ("C1", "Tuner"), "SUR": ("C3", "Surround / sound field"),
       "CD": ("90..92", "CD player / changer"), "MD": ("B0", "MD recorder")}
STATUS = {"v": "✔ verified", "e": "✘ error on STR-DA50ES", "u": "? untested", "x": "○ no unit on the bus"}

# where each command with a dedicated control lives in the interface
UI = {
    ("AMP", "06"): "Listen › Mute (also in the header)", ("AMP", "07"): "Listen › Mute (also in the header)",
    ("AMP", "0F"): "Listen › Refresh / on connect", ("AMP", "14"): "Listen › Volume +, dial, header", ("AMP", "15"): "Listen › Volume −, dial, header",
    ("AMP", "2E"): "Power button", ("AMP", "2F"): "Power button", ("AMP", "48"): "Listen › source labels (INPUTS)",
    ("AMP", "50"): "Listen › Source", ("AMP", "6A"): "Header: amplifier name",
    ("TUN", "06"): "Radio › Seek", ("TUN", "07"): "Radio › Seek", ("TUN", "0A"): "Radio › Mono", ("TUN", "0B"): "Radio › Stereo",
    ("TUN", "0E"): "Radio › signal bars", ("TUN", "48"): "Radio › RDS station name", ("TUN", "4A"): "Radio › Presets of the amplifier (read)",
    ("TUN", "50"): "Radio › tuning, scale, stations, presets", ("TUN", "51"): "Radio › Presets of the amplifier",
    ("SUR", "04"): "Sound › Sound field On", ("SUR", "05"): "Sound › Sound field Off", ("SUR", "0F"): "Sound › state / on connect",
    ("SUR", "40"): "Sound › Surround levels (rear)", ("SUR", "41"): "Sound › Surround levels (center, unavailable here)",
    ("SUR", "42"): "Sound › Surround levels (subwoofer)", ("SUR", "43"): "Sound › Surround levels (rear balance)",
    ("SUR", "44"): "Sound › Surround levels (effect size)", ("SUR", "45"): "Sound › Surround levels (reverb time)",
    ("SUR", "46"): "Sound › Surround levels (compression)", ("SUR", "47"): "Sound › Surround levels (LFE)",
    ("SUR", "50"): "Sound › Equaliser (read)", ("SUR", "51"): "Sound › Sound field list",
    ("SUR", "80"): "Sound › Surround levels (rear)", ("SUR", "81"): "Sound › Surround levels (center)", ("SUR", "82"): "Sound › Surround levels (subwoofer)",
    ("SUR", "83"): "Sound › Surround levels (rear balance)", ("SUR", "84"): "Sound › Surround levels (effect size)", ("SUR", "85"): "Sound › Surround levels (reverb time)",
    ("SUR", "86"): "Sound › Surround levels (compression)", ("SUR", "87"): "Sound › Surround levels (LFE)", ("AMP", "0C"): "Listen › 5.1 input", ("AMP", "0D"): "Listen › 5.1 input", ("AMP", "0E"): "Listen › Recording source (the DA50ES answers an error; the value comes from C8 71)",
    ("AMP", "52"): "Listen › Recording source", ("SUR", "4B"): "Sound › Surround levels (wall type)", ("SUR", "8B"): "Sound › Surround levels (wall type)",
    ("SUR", "53"): "Sound › Speakers (read and apply)",
    ("SUR", "90"): "Sound › Equaliser (dragging a point or a slider)",
}
order = list(DEV)
items.sort(key=lambda i: (order.index(i["dev"]), int(i["code"], 16)))
ui_flagged = {(i["dev"], i["code"]) for i in items if "U" in i["flags"]}
assert ui_flagged == set(UI), (sorted(ui_flagged ^ set(UI)))

# firmware command line -> where the web interface covers it
CLI = [
    ("`<hex bytes>` (raw frame)", "Console · Documented commands (every frame)", "full"),
    ("`PWRON` `PWROFF`", "Power button", "full"), ("`ALLOFF`", "Listen › Turn off all units (the remote's SYSTEM/STANDBY “ALL OFF”)", "full"), ("`MUTE` `UNMUTE`", "Listen › Mute, header", "full"),
    ("`VOLUP` `VOLDN` `VOL UP/DOWN/MIN/SET`", "Listen › Volume, dial (3 scales), fixed-volume buttons, header", "full"),
    ("`VOL MAX` `VOL POS`", "Console only (deliberately not offered: they bypass the safety limit / declare a position)", "console"),
    ("`GAP`", "Settings › S-Link diagnostics", "full"),
    ("`INPUT`", "Listen › Source", "full"), ("`INPUTS` `AMPNAME`", "Automatic on connect; Console chip", "full"),
    ("`STATUS`", "Listen › Refresh; automatic on connect", "full"),
    ("`TUNER SCAN+/SCAN-/MONO/STEREO`", "Radio", "full"), ("`TUNER FM/AM`", "Radio › FM / AM", "full"),
    ("`TUNER PRESET+/PRESET-`", "Documented commands (C1 08/09); the Preset buttons step through the bridge presets", "catalogue"),
    ("`TUNE FM/AM/#n/<name>`", "Radio › scale, ±, form, station list, Back/Next", "full"), ("`PRESET`, `PRESETS`", "Radio › Presets of the amplifier (read-only)", "full"),
    ("`FAV` `FAV ADD/DEL/<n>`", "Radio › Presets (bridge presets, star, Back/Next)", "full"), ("`FMSCAN`, `STATIONS`", "Radio › FM stations; Console chip", "full"),
    ("`NAME`", "Radio › RDS name (automatic)", "full"), ("`EQ`, `EQ F|R ...`", "Sound › Equaliser", "full"), ("`EQCURVE`", "Sound › Equaliser (drawn natively)", "full"),
    ("`SF`", "Sound › Sound field", "full"), ("`REAR` `SUB` `BAL` `DRC` `LFE`", "Sound › Surround levels", "full"),
    ("`MODEL`, `SETUP`", "Console chips; Statistics › S-Link units shows the answers", "full"),
    ("`DEV`", "Settings › S-Link diagnostics", "full"), ("`REPEAT`", "Settings › S-Link diagnostics", "full"),
    ("`DECODE`, `LOOPBACK`", "Settings › S-Link diagnostics", "full"), ("`LINE`", "Settings › S-Link diagnostics", "full"),
    ("`LED`, `SLINK`", "Settings › S-Link diagnostics", "full"),
    ("`BLINK`, `LEVEL`, `EDGES`, `ECHO`", "Console only (pin-test tools / serial-terminal option)", "console"),
    ("`BLE` `WEB` `APP` `ECO`", "Settings › Interfaces", "full"), ("`WIFI`, `MQTT`, `HOSTNAME`", "Settings › Network, MQTT", "full"),
    ("`NET`, `STATS`", "Settings, Statistics; Console chip", "full"), ("`HELP`", "Console chip", "full"),
]

# replies: (frame, meaning, decoded by decode.c, used by the web interface)
REPLIES = [
    ("C8 06 / 07", "mute on / off", "yes", "mute state (Listen, header)"),
    ("C8 0C / 0D", "5.1 input on / off", "yes", "“5.1 input” badge (from C8 70 byte 4 as well)"),
    ("C8 0E / 0F", "unavailable-already done / error", "yes", "toast “command not supported” for 0F after a user action; Console"),
    ("C8 2E / 2F", "power on / off", "yes", "power state"),
    ("C8 43 TT XX", "input type", "yes", "“Input type” badge, Statistics › S-Link units"),
    ("C8 48 SS + 8 chars", "label of input SS", "yes", "source labels"),
    ("C8 61 ...", "model/info (power-on)", "yes (raw)", "Statistics › S-Link units"),
    ("C8 6A + text", "device name", "yes", "header: amplifier name"),
    ("C8 6B NN", "device info", "yes", "Console only"),
    ("C8 70 AA AV CC XX", "status: sources, power, mute, tape loop, 5.1, auto function; XX = S-Link address of the unit on that input", "yes (flags and linked unit named)", "power, mute, input; badges tape loop / 5.1 / auto function off / unit on the input; 5.1 switch; flag byte in Statistics"),
    ("C8 71 AA ..", "2nd audio source", "yes", "“2nd audio input” badge, Statistics"),
    ("C8 E1 ..", "status after S-Link / IR", "yes (raw)", "Console only"),
    ("C8 1E", "polling for an MD/CD player", "yes", "Console only"),
    ("C9 70 BB NN FL HH LL", "tuner status: bank, preset, flags (01 AM, 08 locked, 10 seek down, 20 seek up, 40 stereo, 80 mono), frequency", "yes (both seek directions)", "Radio: frequency, band, preset, lock, stereo, mono, scan"),
    ("C9 71 BB NN LV ST", "signal level", "yes", "Radio: signal bars"),
    ("C9 48 BB NN + 8 chars", "RDS name / preset name", "yes", "Radio: RDS name; preset names"),
    ("C9 4A BB NN FL HH LL", "frequency stored in a preset", "yes", "Radio › Presets of the amplifier"),
    ("C9 10 / 1B / 0E / 0F", "no name / PTY not supported / unavailable / error", "yes", "Console; toast for 0F"),
    ("C9 61 xx xx TT BB PP", "tuner capacity: presets, banks, per bank (07 03 1E 03 0A = 30 / 3 / 10)", "yes (decoded)", "Statistics › S-Link units (raw bytes)"),
    ("C9 E1 ..", "status", "yes (raw)", "Console only"),
    ("CB 70 FF PP SS", "sound field, sound field preset, speaker configuration bitmap (only with a field on)", "yes (speakers decoded)", "Sound: sound field; Sound › Speakers (read-out and editing); Statistics"),
    ("CB 4B WW", "wall type (-8 soft .. +8 hard), reply to the query C3 4B", "yes", "Sound: surround levels (wall type)"),
    ("C8 05 / C9 05", "command ignored", "yes", "Console"),
    ("CB 50 POS BAND FH FL G SL TY", "equaliser band", "yes", "Sound: equaliser"),
    ("CB 40..47 / 80..87", "rear, center, sub, balance, effect size, reverb, compression, LFE", "yes", "Sound: surround levels (all eight)"),
    ("CB 53 54 55..5C 8B 8E 95..9C", "replies to speaker/EQ-preset/distance/crossover/trim queries and sets", "yes", "Console; the STR-DA50ES answers “error” to the queries"),
    ("CB 61 ..", "model/info", "yes (raw)", "Statistics › S-Link units"),
    ("CB 0E / 0F", "unavailable / error", "yes", "Sound: “n/a” levels; toast for 0F"),
    ("98 .. / 99 .. / 9A .. (CD) and B8 .. (MD)", "playback state, track/disc, texts, capacity, status", "yes (decode.c: CD and MD decoders)", "Console only: no player is on this bus"),
]

o = io.StringIO()
w = lambda s="": o.write(s + "\n")
w("# Web interface coverage of the S-Link protocol")
w()
w("> Generated by `tools/gen_coverage.py` from the command catalogue in `main/web/index.html` (the same table drives the "
  "*Documented commands* panel of the Console tab). Do not edit by hand.")
w()
w("What was compared: the command tables published at [hifi-remote.com](https://www.hifi-remote.com/sony/Sony_dsp.htm) "
  "(pages 192 amplifier, 193 tuner, 195 sound field/EQ, 144 CD player), [boehmel.de/slink.htm](https://www.boehmel.de/slink.htm) "
  "(MD, amplifier, tuner, surround), [Ircama/Sony_SLink](https://github.com/Ircama/Sony_SLink) and the Control-A1 reverse-engineering notes "
  "listed in `docs/en/CODES_TO_TRY.md`; the commands of the firmware command line (`main/cli.c`); and the replies decoded by the firmware (`main/decode.c`).")
w()
w("## Summary")
w()
cnt = {}
for i in items:
    s = (re.sub(r"[^vexu]", "", i["flags"]) or "u")[0]
    cnt.setdefault(i["dev"], {}).setdefault(s, 0)
    cnt[i["dev"]][s] += 1
w("| Unit | Commands | ✔ verified | ✘ error on STR-DA50ES | ? untested | ○ no unit on the bus | With a dedicated control |")
w("|---|---|---|---|---|---|---|")
tot = [0, 0, 0, 0, 0, 0]
for d in DEV:
    row = [sum(cnt[d].values())] + [cnt[d].get(k, 0) for k in "veux"] + [sum(1 for i in items if i["dev"] == d and "U" in i["flags"])]
    tot = [a + b for a, b in zip(tot, row)]
    w("| %s (`%s`) | %s |" % (DEV[d][1], DEV[d][0], " | ".join(map(str, row))))
w("| **Total** | %s |" % " | ".join("**%d**" % t for t in tot))
w()
w("- **Every documented command can be sent from the web interface** (*Console › Documented commands*: parameter widgets, a live frame preview, "
  "filter; destructive or audible ones, and those for units not on the bus, ask for confirmation). %d of them also have a dedicated control in the main tabs." % tot[5])
w("- **Every command of the firmware command line** is reachable: see the table further down (two groups of pin-test/terminal commands are console-only on purpose).")
w("- **Every S-Link reply is decoded and shown** in the Console. The replies that carry state are also used by the interface (table at the end).")
w("- The “verified” column is what was measured on a real STR-DA50ES; the others are from the documentation and may behave differently on other models "
  "(`C0 83` input type, for instance, is `0..4` on hifi-remote and `1/2/4` on boehmel).")
w()
for d in DEV:
    w("## %s (`%s`)" % (DEV[d][1], DEV[d][0]))
    w()
    w("| Code | Command | Parameters | Status | Dedicated control |")
    w("|---|---|---|---|---|")
    for i in items:
        if i["dev"] != d:
            continue
        s = (re.sub(r"[^vexu]", "", i["flags"]) or "u")[0]
        extra = " ⚠" if "!" in i["flags"] else ""
        w("| `%s` | %s | %s | %s%s | %s |" % (i["code"], i["name"], ", ".join(i["params"]).replace("|", "/") or "–", STATUS[s], extra, UI.get((d, i["code"]), "–")))
    w()
w("⚠ = audible, destructive or changes the setup of the system: the interface asks for confirmation.")
w()
w("## Firmware command line (`main/cli.c`)")
w()
w("| Command | Where in the web interface | Coverage |")
w("|---|---|---|")
for c, where, cov in CLI:
    w("| %s | %s | %s |" % (c, where, {"full": "dedicated control", "catalogue": "catalogue", "console": "Console only"}[cov]))
w()
w("## S-Link replies")
w()
w("| Reply | Meaning | Decoded by the firmware | Used by the web interface |")
w("|---|---|---|---|")
for r in REPLIES:
    w("| `%s` | %s | %s | %s |" % r)
w()
w("Replies that no unit on this bus sends (CD/MD) are decoded but have no state in the interface; the unit would have to be on the bus to be tried.")

dst = os.path.join(root, "docs", "en", "WEB_COVERAGE.md")
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8", newline="\n").write(o.getvalue())
sys.stdout.reconfigure(encoding="utf-8")
print("wrote", dst, "-", len(items), "commands")
