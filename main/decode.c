/* Decoder for S-Link frames: amplifier, tuner, surround (+ CD/MD names).
 * Tables from boehmel.de/slink.htm and Ircama/Sony_SLink.
 *
 * First byte = device code "nnnn X yyy": nnnn device family, X = 0 command to the
 * device / 1 reply from the device, yyy = sub-device (0 amp, 1 tuner, 3 surround).
 */
#include <stdio.h>
#include <string.h>
#include "decode.h"

static const char *src_name(uint8_t v)
{
    switch (v) {
    case 0x00: return "TUNER";   case 0x01: return "PHONO";  case 0x02: return "CD";
    case 0x03: return "DAT";     case 0x04: return "MD";     case 0x05: return "TAPE";
    case 0x07: return "DIGITAL"; case 0x10: return "VIDEO1"; case 0x11: return "VIDEO2";
    case 0x12: return "VIDEO3";  case 0x13: return "VIDEO4"; case 0x15: return "DVD/LD";
    case 0x16: return "SAT/TV";  case 0x17: return "TV";     case 0x19: return "DVD";
    case 0xFE: return "none";    default:   return "?";
    }
}

static const char *intype_name(uint8_t v)
{
    switch (v) {
    case 0: return "auto"; case 1: return "optical"; case 2: return "coax";
    case 3: return "AC3RF"; case 4: return "analog"; default: return "?";
    }
}


/* Sound field names: hifi-remote.com Sony_dsp_sf.htm ("list varies by receiver model"). On the STR-DA50ES numbers
 * 0 (off) .. 29 were accepted and 29 is the default on the TUNER input; names are NOT verified by ear. */
static const char *const k_sf[30] = {
    "Off", "Normal Pro Logic Surround", "Cinema Studio A", "Cinema Studio B", "Cinema Studio C", "Night Theater",
    "Mono Movie", "Stereo Movie", "Multi Rear", "Multi Dimensional", "Virtual Theater A", "Virtual Theater B",
    "Virtual Theater C", "Virtual Enhanced A", "Virtual Enhanced B", "Virtual Semi-Multi Dimensional",
    "Virtual Semi-Theater A", "Virtual Semi-Theater B", "Virtual Semi-Theater C", "Small Hall", "Large Hall",
    "Opera House", "Jazz Club", "Club/Disco", "Church", "Live House", "Arena", "Stadium", "Game", "Auto Format Decode"
};
static const char *sf_name(uint8_t n) { return n < 30 ? k_sf[n] : "?"; }

static const char *decode_mode_name(uint8_t v)
{
    switch (v) { case 0: return "auto"; case 1: return "AC3"; case 2: return "DTS"; case 3: return "MPEG"; case 4: return "PCM"; default: return "?"; }
}

/* BCD / disc-number helpers for CD changers (hifi-remote Sony_dsp_144.htm, BigDave control-a1.txt) */
static unsigned bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static unsigned cd_disc(uint8_t v)
{
    if ((v >> 4) > 9) return (unsigned)(16 * (v >> 4) + (v & 15) - 54);    /* 0x9B..0xFE = 101..200 */
    if (v == 0) return 100;
    return bcd(v);
}

/* ASCII payload (station / source / device names), trailing blanks trimmed. */
static void ascii(const uint8_t *p, size_t n, char *dst, size_t sz)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < sz && p[i] != 0; i++)   /* NUL ends the text */
        dst[k++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = 0;
}

/* Speaker configuration bitmap (C3 53 sets it, CB 70 byte 4 reports it; hifi-remote Sony_dsp_195 / 203):
 * 01 front small, 04 centre downgraded to small when the front is large, 08 no centre, 10 rear downgraded, 20 no rear, 40 no subwoofer */
static int spk_text(uint8_t b, char *o, size_t sz)
{
    const char *front = (b & 1) ? "small" : "large";
    const char *ctr = (b & 8) ? "none" : ((b & 1) || (b & 4)) ? "small" : "large";
    const char *rear = (b & 0x20) ? "none" : ((b & 1) || (b & 0x10)) ? "small" : "large";
    return snprintf(o, sz, "front %s, center %s, rear %s, sub %s", front, ctr, rear, (b & 0x40) ? "none" : "present");
}

/* the device code the amplifier believes is connected to an input (C8 70 byte 5): the S-Link address of that unit */
static const char *linked_dev(uint8_t v)
{
    switch (v) {
    case 0x90: return "CD player 1 (90)"; case 0x91: return "CD player 2 (91)"; case 0x92: return "CD player 3 (92)";
    case 0xB0: return "MD recorder (B0)"; case 0xC1: return "tuner (C1)"; case 0xFF: return "none";
    default: return "unit";
    }
}

static int amp_cmd(const uint8_t *d, size_t n, char *o, size_t sz)
{
    switch (d[1]) {
    case 0x06: return snprintf(o, sz, "mute ON");
    case 0x07: return snprintf(o, sz, "mute OFF");
    case 0x0C: return snprintf(o, sz, "5.1 input ON");
    case 0x0D: return snprintf(o, sz, "5.1 input OFF");
    case 0x0E: case 0x0F: return snprintf(o, sz, "status request");
    case 0x14: return snprintf(o, sz, "volume UP");
    case 0x15: return snprintf(o, sz, "volume DOWN");
    case 0x0A: return snprintf(o, sz, "tape loop IN");
    case 0x0B: return snprintf(o, sz, "tape loop OUT");
    case 0x22: return snprintf(o, sz, "query model/capacity info");
    case 0x2E: return snprintf(o, sz, "power ON");
    case 0x2F: return snprintf(o, sz, "power OFF");
    case 0x40: return n > 2 ? snprintf(o, sz, "set volume %u (not supported on DA50ES)", d[2]) : snprintf(o, sz, "query volume (not supported on DA50ES)");
    case 0x41: return snprintf(o, sz, "query video loop");
    case 0x42: return snprintf(o, sz, "query audio split");
    case 0x43: return snprintf(o, sz, "query input type");
    case 0x45: return snprintf(o, sz, "query decode mode");
    case 0x46: return snprintf(o, sz, "query display dimmer");
    case 0x47: return snprintf(o, sz, "query output level");
    case 0x80: return n > 2 ? snprintf(o, sz, "set volume %u (reported not working)", d[2]) : 0;
    case 0x81: return n > 2 ? snprintf(o, sz, "video loop %s", d[2] ? "ON" : "OFF") : 0;
    case 0x82: return n > 3 ? snprintf(o, sz, "audio split audio=%s video=%s", src_name(d[2]), src_name(d[3])) : 0;
    case 0x85: return n > 2 ? snprintf(o, sz, "decode mode %s", decode_mode_name(d[2])) : 0;
    case 0x86: return n > 2 ? snprintf(o, sz, "display dimmer %u", d[2]) : 0;
    case 0x87: return n > 2 ? snprintf(o, sz, "output level %s", d[2] ? "normal" : "low") : 0;
    case 0x88: {   /* C0 88 SS + 8 chars */
        char t[12];
        if (n < 4) return 0;
        ascii(d + 3, n - 3 > 8 ? 8 : n - 3, t, sizeof t);
        return snprintf(o, sz, "set label of input %s to \"%s\"", src_name(d[2]), t);
    }
    case 0x48: return n > 2 ? snprintf(o, sz, "query name of source %s", src_name(d[2])) : 0;
    case 0x50: return n > 2 ? snprintf(o, sz, "select source %s", src_name(d[2])) : 0;
    case 0x52: return n > 2 ? snprintf(o, sz, "2nd audio %s", src_name(d[2])) : 0;
    case 0x6A: return snprintf(o, sz, "query device name");
    case 0x6B: return snprintf(o, sz, "query device info");
    case 0x83: return n > 2 ? snprintf(o, sz, "input type %s", intype_name(d[2])) : 0;
    default:   return snprintf(o, sz, "cmd %02X", d[1]);
    }
}

static int amp_reply(const uint8_t *d, size_t n, char *o, size_t sz)
{
    char t[24];
    switch (d[1]) {
    case 0x06: return snprintf(o, sz, "muted");
    case 0x07: return snprintf(o, sz, "unmuted");
    case 0x0C: return snprintf(o, sz, "5.1 input ON");
    case 0x0D: return snprintf(o, sz, "5.1 input OFF");
    case 0x0E: return snprintf(o, sz, "unavailable / already done");
    case 0x0F: return snprintf(o, sz, "error");
    case 0x2E: return snprintf(o, sz, "power ON");
    case 0x2F: return snprintf(o, sz, "power OFF");
    case 0x43: return n > 3 ? snprintf(o, sz, "input type %s (%02X)", intype_name(d[2]), d[3]) : 0;
    case 0x61: return snprintf(o, sz, "model/info %s", n > 2 ? "(raw)" : "");
    case 0x6A: ascii(d + 2, n - 2, t, sizeof t); return snprintf(o, sz, "device name \"%s\"", t);
    case 0x6B: return snprintf(o, sz, "device info %02X (sent after the name; 02 on STR-DA50ES)", n > 2 ? d[2] : 0);
    case 0x1E: return snprintf(o, sz, "polling for an MD/CD player (followed by B0 22 / 90 22)");
    case 0x48:   /* C8 48 SS + 8 chars: label of input SS as shown by the amp */
        if (n < 4) return 0;
        ascii(d + 3, n - 3 > 8 ? 8 : n - 3, t, sizeof t);
        { const char *p = t; while (*p == ' ') p++;
          return snprintf(o, sz, "input %02X (%s) is labelled \"%s\"", d[2], src_name(d[2]), p); }
    case 0x05: return snprintf(o, sz, "ignored");
    case 0x40: return n > 2 ? snprintf(o, sz, "volume level %u", d[2]) : 0;      /* hifi-remote Sony_dsp_200: returns the volume (not on the DA50ES) */
    case 0x70: { /* C8 70 AA AV CC XX: audio src, video src, CC (bit0 power, bit1 mute, bit3 tape loop, bit4 5.1 input, bit5 auto-function off),
                  * XX = S-Link address of the unit the amplifier believes is connected to that input (FF = none) */
        if (n < 5) return 0;
        char fl[64] = "";
        snprintf(fl, sizeof fl, "%s%s%s", (d[4] & 0x08) ? " tape-loop" : "", (d[4] & 0x10) ? " 5.1-input" : "", (d[4] & 0x20) ? " auto-function-OFF" : "");
        if (n >= 6) return snprintf(o, sz, "input status audio=%s video=%s power=%s mute=%s%s%s; linked unit %02X = %s", src_name(d[2]), src_name(d[3]),
                                    (d[4] & 1) ? "ON" : "standby", (d[4] & 2) ? "ON" : "off", fl, (d[4] & 0xC4) ? " (+flags)" : "", d[5], linked_dev(d[5]));
        return snprintf(o, sz, "input status audio=%s video=%s power=%s mute=%s%s%s", src_name(d[2]), src_name(d[3]),
                        (d[4] & 1) ? "ON" : "standby", (d[4] & 2) ? "ON" : "off", fl, (d[4] & 0xC4) ? " (+flags)" : "");
    }
    case 0x71: return n > 2 ? snprintf(o, sz, "2nd audio %s", src_name(d[2])) : 0;
    case 0xE1: return n > 3 ? snprintf(o, sz, "status %02X, caused by an %s command (boehmel: bit 2 of the 2nd byte)", d[2], (d[3] & 4) ? "infrared" : "S-Link") : snprintf(o, sz, "status %02X", n > 2 ? d[2] : 0);
    default:   return snprintf(o, sz, "reply %02X", d[1]);
    }
}

static int tuner_cmd(const uint8_t *d, size_t n, char *o, size_t sz)
{
    switch (d[1]) {
    case 0x03: return n > 2 ? snprintf(o, sz, "band %s", d[2] ? "AM" : "FM") : 0;
    case 0x06: return snprintf(o, sz, "scan UP");
    case 0x07: return snprintf(o, sz, "scan DOWN");
    case 0x08: return snprintf(o, sz, "preset UP");
    case 0x09: return snprintf(o, sz, "preset DOWN");
    case 0x0A: return snprintf(o, sz, "mono");
    case 0x0B: return snprintf(o, sz, "stereo");
    case 0x0C: return snprintf(o, sz, "query station/dial");
    case 0x0E: return snprintf(o, sz, "query signal level");
    case 0x48: return n > 3 ? snprintf(o, sz, "query station name (bank %u, %s)", d[2], d[3] == 0xFF ? "current station" : "preset") : 0;
    case 0x2E: return snprintf(o, sz, "power ON");
    case 0x2F: return snprintf(o, sz, "power OFF");
    case 0x0F: return snprintf(o, sz, "status request");
    case 0x22: return snprintf(o, sz, "query capacity/model");
    case 0x50:
        if (n < 5) return 0;
        if (d[2] == 0) return snprintf(o, sz, "tune FM %u.%02u MHz", ((d[3] << 8) | d[4]) / 100, ((d[3] << 8) | d[4]) % 100);
        return snprintf(o, sz, "tune AM %u kHz", (d[3] << 8) | d[4]);
    case 0x51: return n > 3 ? snprintf(o, sz, "preset bank %u station %u", d[2], d[3]) : 0;
    case 0x4A: return n > 3 ? snprintf(o, sz, "query frequency of preset bank %u #%u", d[2], d[3]) : 0;
    case 0x52: return n > 2 ? snprintf(o, sz, "PTY search %u", d[2]) : 0;
    default:   return snprintf(o, sz, "cmd %02X", d[1]);
    }
}

static int tuner_reply(const uint8_t *d, size_t n, char *o, size_t sz)
{
    char t[16];
    switch (d[1]) {
    case 0x48:   /* C9 48 BB NN + 8 chars: station name (RDS) of preset NN in bank BB; NN=FF = station now tuned */
        if (n < 4) return 0;
        ascii(d + 4, n - 4 > 8 ? 8 : n - 4, t, sizeof t);
        return snprintf(o, sz, "station name \"%s\" (bank %u, %s)", t, d[2], d[3] == 0xFF ? "current station" : "preset");
    case 0x10: return snprintf(o, sz, "no name stored");
    case 0x4A:   /* C9 4A BB NN FL HH LL: frequency memorised in preset NN of bank BB (87.50 = empty preset); FL 00 = FM */
        if (n >= 7) {
            unsigned f = (d[5] << 8) | d[6];
            if (d[4] & 1) return snprintf(o, sz, "preset bank %u #%u = AM %u kHz", d[2], d[3], f);
            return snprintf(o, sz, "preset bank %u #%u = FM %u.%02u MHz%s", d[2], d[3], f / 100, f % 100,
                            f == 8750 ? " (empty)" : "");
        }
        return snprintf(o, sz, "preset data");
    case 0x70:   /* C9 70 BB NN FL HH LL: bank, preset, flags, frequency */
        if (n >= 7) {
            uint8_t fl = d[4];
            unsigned f = (d[5] << 8) | d[6];
            char fr[32], pr[40];
            if (fl & 1) snprintf(fr, sizeof fr, "AM %u kHz", f);
            else        snprintf(fr, sizeof fr, "FM %u.%02u MHz", f / 100, f % 100);
            if (d[3] == 0xFF) snprintf(pr, sizeof pr, "no preset");
            else              snprintf(pr, sizeof pr, "preset bank %u #%u", d[2], d[3]);
            /* FL bits: 01 AM, 08 signal locked, 10 searching down, 20 searching up (hifi-remote Sony_dsp_201; measured: 20 while seeking),
             * 40 stereo pilot, 80 mono forced */
            return snprintf(o, sz, "tuner %s, %s%s%s%s%s%s", fr, pr,
                            (fl & 0x20) ? ", SCANNING UP" : (fl & 0x10) ? ", SCANNING DOWN" : "", (fl & 0x08) ? ", locked" : "",
                            (fl & 0x40) ? ", stereo pilot" : "", (fl & 0x80) ? ", MONO forced" : "",
                            (fl & 0x06) ? " (+flags)" : "");
        }
        if (n < 4) return 0;
        return snprintf(o, sz, "tuner status band=%s preset=%u", d[2] ? "AM" : "FM", d[3]);
    case 0x71:   /* C9 71 BB NN LV ST: signal level (0 weak .. 12 strong seen), then ST 00 -> 01 after tuning */
        if (n >= 6) return snprintf(o, sz, "signal level %u (%s)", d[4], d[5] ? "settled" : "measuring");
        return snprintf(o, sz, "tuner event");
    case 0x1B: return snprintf(o, sz, "PTY search not supported");
    case 0x05: return snprintf(o, sz, "ignored");
    case 0xE1: return n > 3 ? snprintf(o, sz, "status %02X, caused by an %s command (boehmel: bit 2 of the 2nd byte)", d[2], (d[3] & 4) ? "infrared" : "S-Link") : snprintf(o, sz, "status %02X", n > 2 ? d[2] : 0);
    case 0x61:   /* C9 61 xx xx TT BB PP: on the DA50ES 07 03 1E 03 0A = 30 presets, 3 banks, 10 per bank (hifi-remote: # presets, max bank, max preset) */
        if (n >= 7) return snprintf(o, sz, "tuner capacity: %u presets, %u banks of %u (info %02X %02X)", d[4], d[5], d[6], d[2], d[3]);
        return snprintf(o, sz, "model/info");
    case 0x0E: return snprintf(o, sz, "unavailable / already done");
    case 0x0F: return snprintf(o, sz, "error");
    default:   return snprintf(o, sz, "reply %02X", d[1]);
    }
}

/* ---- surround / sound field / EQ (device C3, replies CB) ----
 * Codes from hifi-remote.com Sony_dsp_195 (decimal there), verified on STR-DA50ES. */
static const char *eq_pos(uint8_t v)  { return v == 0 ? "front" : v == 1 ? "center" : v == 2 ? "rear" : "?"; }
static const char *eq_band(uint8_t v) { return v == 0 ? "bass" : v == 1 ? "mid" : v == 2 ? "treble" : "?"; }

static const char *sur_param_name(uint8_t c)
{
    switch (c) {
    case 0x40: case 0x80: return "rear level";
    case 0x41: case 0x81: return "center level";
    case 0x42: case 0x82: return "subwoofer level";
    case 0x43: case 0x83: return "rear balance";
    case 0x44: case 0x84: return "effect size";
    case 0x45: case 0x85: return "reverb time";
    case 0x46: case 0x86: return "dynamic range compression";
    case 0x47: case 0x87: return "LFE mix level";
    case 0x4B: case 0x8B: return "wall type";
    case 0x55: case 0x95: return "speaker distance";
    case 0x56: case 0x96: return "rear placement";
    case 0x57: case 0x97: return "crossover frequencies";
    case 0x58: case 0x98: return "surround EQ";
    case 0x59: case 0x99: return "front balance";
    case 0x5A: case 0x9A: return "dialog trim";
    case 0x5B: case 0x9B: return "surround trim";
    case 0x5C: case 0x9C: return "subwoofer phase";
    default: return NULL;
    }
}

static int fmt_param(uint8_t code, uint8_t v, char *o, size_t sz)
{
    const char *nm = sur_param_name(code);
    if (!nm) return snprintf(o, sz, "%02X = %02X", code, v);
    if (code == 0x46 || code == 0x86) {                 /* DRC: 0 off, 1..9 = 0.1..0.9, 10 standard, 255 max */
        if (v == 0) return snprintf(o, sz, "%s = off", nm);
        if (v == 10) return snprintf(o, sz, "%s = standard", nm);
        if (v == 255) return snprintf(o, sz, "%s = maximum", nm);
        return snprintf(o, sz, "%s = 0.%u", nm, v);
    }
    return snprintf(o, sz, "%s = %d", nm, (int)(int8_t)v);   /* signed byte */
}

static int sur_cmd(const uint8_t *d, size_t n, char *o, size_t sz)
{
    uint8_t c = d[1];
    switch (c) {
    case 0x04: return snprintf(o, sz, "sound field ON");
    case 0x05: return snprintf(o, sz, "sound field OFF");
    case 0x06: return snprintf(o, sz, "EQ tone ON");
    case 0x07: return snprintf(o, sz, "EQ tone OFF");
    case 0x0A: return snprintf(o, sz, "genre toggle");
    case 0x0B: return snprintf(o, sz, "effect/mode toggle");
    case 0x0F: return snprintf(o, sz, "status request");
    case 0x10: return snprintf(o, sz, "rear level UP");
    case 0x11: return snprintf(o, sz, "rear level DOWN");
    case 0x12: return snprintf(o, sz, "center level UP");
    case 0x13: return snprintf(o, sz, "center level DOWN");
    case 0x14: return snprintf(o, sz, "subwoofer level UP");
    case 0x15: return snprintf(o, sz, "subwoofer level DOWN");
    case 0x22: return snprintf(o, sz, "surround type toggle");
    case 0x50: return n > 3 ? snprintf(o, sz, "query EQ %s %s", eq_pos(d[2]), eq_band(d[3])) : 0;
    case 0x51: return n > 2 ? snprintf(o, sz, "select sound field %u (%s)", d[2], sf_name(d[2])) : 0;
    case 0x54: return n > 2 ? snprintf(o, sz, "select EQ preset %u", d[2]) : 0;
    case 0x90:   /* C3 90 POS BAND FH FL GAIN SLOPE TYPE (TYPE 0 = gain, 1 = frequency) */
        if (n < 9) return 0;
        return snprintf(o, sz, "set EQ %s %s: freq %u Hz, gain %d dB, slope %u, type %s", eq_pos(d[2]),
                        eq_band(d[3]), (d[4] << 8) | d[5], (int)(int8_t)d[6], d[7], d[8] ? "freq" : "gain");
    case 0x53: return n > 2 ? snprintf(o, sz, "speaker sizes %02X (front %s, center %s, rear %s, sub %s)", d[2],
                                       (d[2] & 1) ? "small" : "large", (d[2] & 8) ? "none" : "present",
                                       (d[2] & 0x20) ? "none" : "present", (d[2] & 0x40) ? "none" : "present") : 0;
    case 0x8B: return n > 2 ? snprintf(o, sz, "wall type = %d", (int)(int8_t)d[2]) : 0;
    case 0x8E: return n > 2 ? snprintf(o, sz, "sound field preset %u", d[2] + 1) : 0;
    case 0x95: return n > 5 ? snprintf(o, sz, "speaker distance (%s) front %d center %d rear %d", d[2] ? "feet" : "m",
                                       (int)d[3] - 7, (int)d[4] - 7, (int)d[5] - 7) : 0;
    case 0x96: return n > 2 ? snprintf(o, sz, "rear placement %s%s", d[2] == 0 ? "side" : d[2] == 1 ? "middle" : d[2] == 2 ? "behind" : "high",
                                       n > 3 ? (d[3] ? ", high" : ", low") : "") : 0;
    case 0x97: return n > 4 ? snprintf(o, sz, "crossover front %u Hz center %u Hz rear %u Hz", d[2] * 10, d[3] * 10, d[4] * 10) : 0;
    case 0x98: return n > 7 ? snprintf(o, sz, "set surround EQ %s: freq %u Hz gain %d dB", eq_band(d[2]), (d[3] << 8) | d[4], (int)(int8_t)d[5]) : 0;
    case 0xA0: {
        if (n < 3) return 0;
        const char *tn = d[2] == 0 ? "off" : d[2] == 16 ? "auto" : d[2] == 32 ? "left" : d[2] == 33 ? "center" :
                         d[2] == 34 ? "right" : d[2] == 35 ? "right surround" : d[2] == 36 ? "left surround" :
                         d[2] == 37 ? "subwoofer" : "?";
        return snprintf(o, sz, "test tone %s", tn);
    }
    default: break;
    }
    if (c >= 0x40 && c <= 0x5C && sur_param_name(c)) return snprintf(o, sz, "query %s", sur_param_name(c));
    if (c >= 0x80 && c <= 0x9C && sur_param_name(c) && n > 2) { return fmt_param(c, d[2], o, sz); }
    return snprintf(o, sz, "cmd %02X", c);
}

static int sur_reply(const uint8_t *d, size_t n, char *o, size_t sz)
{
    uint8_t c = d[1];
    char t[64];
    switch (c) {
    case 0x70:   /* CB 70 FF PP SS: FF = sound field (00 = off), PP = sound field preset, SS = speaker configuration bitmap (only with a field on) */
        if (n >= 5) {
            if (d[2] == 0) return snprintf(o, sz, "sound field OFF (%02X %02X)", d[3], d[4]);
            char sp[64];
            spk_text(d[4], sp, sizeof sp);
            return snprintf(o, sz, "sound field %u: %s (preset %u; speakers %02X: %s)", d[2], sf_name(d[2]), d[3], d[4], sp);
        }
        return snprintf(o, sz, "surround status");
    case 0x4B: return n > 2 ? snprintf(o, sz, "wall type = %d", (int)(int8_t)d[2]) : 0;   /* -8 soft .. +8 hard */
    case 0x50:   /* CB 50 POS BAND FH FL GAIN SLOPE TY */
        if (n >= 9)
            return snprintf(o, sz, "EQ %s %s: %u Hz, gain %d dB, slope %u%s", eq_pos(d[2]), eq_band(d[3]),
                            (d[4] << 8) | d[5], (int)(int8_t)d[6], d[7],
                            d[8] == 3 ? " (set ok)" : "");
        return n > 2 ? snprintf(o, sz, "front volume %u", d[2]) : 0;
    case 0x0E: return snprintf(o, sz, "unavailable / already done");
    case 0x0F: return snprintf(o, sz, "error");
    case 0x61: return snprintf(o, sz, "model/info");
    default: break;
    }
    if (n >= 4 && sur_param_name(c)) {   /* CB cc VV TY: TY 01 = query reply, 03 = set reply */
        fmt_param(c, d[2], t, sizeof t);
        return snprintf(o, sz, "%s%s", t, d[3] == 3 ? " (set ok)" : d[3] == 1 ? "" : " (step)");
    }
    return snprintf(o, sz, "reply %02X", c);
}

/* CD/MD status byte S1 of the "70" reply (undeadscientist CD tables + caelor/sony-a1-hass for MD): bits 0-2 transport
 * (0 stop, 1 play, 2 pause, 4 record, 5 record pause), bit 4 power OFF, bit 5 no disc. Seen: 00 stop, 01 play, 02 pause, 03 door open,
 * 10 off, 23 on/no disc, 2F on/no disc (changer), 33 off/no disc, 3F off/no disc (changer) */
static void s1_text(uint8_t s1, char *o, size_t sz)
{
    unsigned tr = s1 & 7;
    const char *t = tr == 0 ? "stopped" : tr == 1 ? "playing" : tr == 2 ? "paused" : tr == 4 ? "recording" : tr == 5 ? "record pause" : tr == 3 ? "door open" : "idle";
    snprintf(o, sz, "%s%s%s", t, (s1 & 0x10) ? ", power OFF" : "", (s1 & 0x20) ? ", no disc" : "");
}

/* number of bits set in a "which discs/tracks exist" bitmap (bytes are LSB-first, see the CD response tables) */
static unsigned bits_set(const uint8_t *p, size_t n)
{
    unsigned c = 0;
    for (size_t i = 0; i < n; i++) for (int b = 0; b < 8; b++) c += (p[i] >> b) & 1;
    return c;
}

size_t slink_decode(const uint8_t *d, size_t len, char *out, size_t out_sz)
{
    if (len < 2 || out_sz < 8) return 0;
    uint8_t fam = d[0] >> 4, reply = (d[0] >> 3) & 1, sub = d[0] & 7;
    const char *who = NULL;
    char body[160] = "";
    int n = 0;

    if (fam == 0xC || fam == 0x7) {            /* C = AV1 codes, 7 = AV2/new-amp codes */
        const char *tag = fam == 0x7 ? "(7x) " : "";
        char name[24];
        if (sub == 0) {
            snprintf(name, sizeof name, "%sAMP", tag); who = name;
            n = reply ? amp_reply(d, len, body, sizeof body) : amp_cmd(d, len, body, sizeof body);
        } else if (sub == 1) {
            snprintf(name, sizeof name, "%sTUNER", tag); who = name;
            n = reply ? tuner_reply(d, len, body, sizeof body) : tuner_cmd(d, len, body, sizeof body);
        } else if (sub == 3) {
            snprintf(name, sizeof name, "%sSURROUND", tag); who = name;
            n = reply ? sur_reply(d, len, body, sizeof body) : sur_cmd(d, len, body, sizeof body);
        } else return 0;
        if (n <= 0) return 0;
        return (size_t)snprintf(out, out_sz, "%s %s: %s", who, reply ? "reply" : "cmd", body);
    }
    if (fam == 0x9 || fam == 0xB) {            /* CD (9x) / MD (Bx): from Automated Jukebox Control + BigDave; NOT verified here */
        bool md = fam == 0xB;
        unsigned unit = md ? sub + 1 : (sub < 3 ? sub + 1 : sub < 6 ? sub - 2 : 0);     /* CD 9x: 0/1/2 = players 1-3, 3/4/5 = same players for discs > 200, 7 = all */
        char bd[140] = "", t[24];
        n = 0;
        if (!reply) {
            switch (d[1]) {
            case 0x00: n = snprintf(bd, sizeof bd, "play"); break;
            case 0x01: n = snprintf(bd, sizeof bd, "stop"); break;
            case 0x02: n = snprintf(bd, sizeof bd, "pause"); break;
            case 0x03: n = snprintf(bd, sizeof bd, "toggle pause (pause cue: the first press pauses, the second plays the track)"); break;
            case 0x04: n = md ? snprintf(bd, sizeof bd, "eject") : 0; break;
            case 0x07: n = md ? snprintf(bd, sizeof bd, "record") : 0; break;
            case 0x08: n = snprintf(bd, sizeof bd, "next track"); break;
            case 0x09: n = snprintf(bd, sizeof bd, "previous track (or restart)"); break;
            case 0x0A: n = md ? snprintf(bd, sizeof bd, "record start") : 0; break;
            case 0x0E: n = md ? snprintf(bd, sizeof bd, "query stored") : snprintf(bd, sizeof bd, "query disc memory info"); break;
            case 0x0F: n = snprintf(bd, sizeof bd, "query mode/status"); break;
            case 0x10: n = snprintf(bd, sizeof bd, "fast forward"); break;
            case 0x11: n = snprintf(bd, sizeof bd, "fast rewind"); break;
            case 0x12: n = snprintf(bd, sizeof bd, md ? "slow forward" : "forward (about 60x)"); break;
            case 0x13: n = snprintf(bd, sizeof bd, md ? "slow rewind" : "rewind (about 60x)"); break;
            case 0x1F: n = snprintf(bd, sizeof bd, "normal / resume"); break;
            case 0x20: n = snprintf(bd, sizeof bd, "disable front panel"); break;
            case 0x21: n = snprintf(bd, sizeof bd, "enable front panel"); break;
            case 0x22: n = snprintf(bd, sizeof bd, md ? "query player type" : "query capacity"); break;
            case 0x23: n = md ? snprintf(bd, sizeof bd, "auto pause OFF") : 0; break;
            case 0x24: n = md ? snprintf(bd, sizeof bd, "auto pause ON") : snprintf(bd, sizeof bd, "report start of tracks"); break;
            case 0x25: n = snprintf(bd, sizeof bd, "verbose status ON (every second)"); break;
            case 0x26: n = snprintf(bd, sizeof bd, "verbose status OFF"); break;
            /* Sony MD recorders (William Hollingworth, MDS-JB920): commands that answer but whose function is unknown */
            case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: n = md ? snprintf(bd, sizeof bd, "unknown MD command (mdslink: no reply)") : 0; break;
            case 0x5C: n = md ? snprintf(bd, sizeof bd, "unknown MD command (mdslink: 5C 00 01 is answered by 4C)") : 0; break;
            case 0xB2: n = md ? snprintf(bd, sizeof bd, "unknown MD command (mdslink: no reply)") : 0; break;
            case 0x2E: n = snprintf(bd, sizeof bd, "power ON"); break;
            case 0x2F: n = snprintf(bd, sizeof bd, "power OFF"); break;
            case 0x40: n = md ? snprintf(bd, sizeof bd, "erase track") : (len > 2 ? snprintf(bd, sizeof bd, "get memo of disc %u", cd_disc(d[2])) : 0); break;
            case 0x27: n = md ? 0 : snprintf(bd, sizeof bd, "query CD Text presence"); break;
            case 0x32: n = md ? snprintf(bd, sizeof bd, "divide") : snprintf(bd, sizeof bd, "query artist/group mode"); break;
            case 0x33: n = md ? snprintf(bd, sizeof bd, "menu yes") : 0; break;
            case 0x34: n = md ? snprintf(bd, sizeof bd, "menu no") : 0; break;
            case 0x35: n = md ? snprintf(bd, sizeof bd, "undo") : 0; break;
            case 0x3E: n = md ? snprintf(bd, sizeof bd, "device keys disabled") : 0; break;
            case 0x3F: n = md ? snprintf(bd, sizeof bd, "device keys enabled") : 0; break;
            case 0x41: n = md ? (len > 3 ? snprintf(bd, sizeof bd, "move track %u to %u", d[2], d[3]) : 0) : (len > 2 ? snprintf(bd, sizeof bd, "get delete file of disc %u", cd_disc(d[2])) : 0); break;
            case 0x42: n = md ? (len > 2 ? snprintf(bd, sizeof bd, "combine track %u with the previous", d[2]) : 0) : (len > 2 ? snprintf(bd, sizeof bd, "get group %u name", bcd(d[2])) : 0); break;
            case 0x43: n = md ? (len > 2 ? snprintf(bd, sizeof bd, "combine with track %u", d[2]) : 0) : (len > 2 ? snprintf(bd, sizeof bd, "get disc list of group %u", bcd(d[2])) : 0); break;
            case 0x46: n = md ? snprintf(bd, sizeof bd, "split adjust") : (len > 2 ? snprintf(bd, sizeof bd, "get memo of disc %u in a slave changer", cd_disc(d[2])) : 0); break;
            case 0x4A: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, d[2] == 0 ? "get title of the current track (CD Text; older decks: BCD to hex)" : "get title of track %u (CD Text; older decks: BCD to hex)", bcd(d[2])) : 0); break;
            case 0x54: n = md ? (len > 2 ? snprintf(bd, sizeof bd, "remaining time of disc %u", d[2]) : 0) : 0; break;
            case 0x58: n = md ? snprintf(bd, sizeof bd, "get disc name") : 0; break;
            case 0x5A: n = md ? (len > 2 ? snprintf(bd, sizeof bd, "get name of track %u", d[2]) : 0) : 0; break;
            case 0x63: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, "get bar code of disc %u", cd_disc(d[2])) : 0); break;
            case 0x64: n = md ? 0 : snprintf(bd, sizeof bd, "get 'YYYY' info of the current track"); break;
            case 0x74: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, d[2] ? "group mode: group %u" : "group mode OFF", d[2]) : 0); break;
            case 0x76: n = md ? snprintf(bd, sizeof bd, "combine tracks a+b") : 0; break;
            case 0x78: n = md ? snprintf(bd, sizeof bd, "A-B erase: set point A") : 0; break;
            case 0x79: n = md ? snprintf(bd, sizeof bd, "A-B erase: set point B") : 0; break;
            case 0x7A: n = md ? snprintf(bd, sizeof bd, "adjust point A") : 0; break;
            case 0x7B: n = md ? snprintf(bd, sizeof bd, "adjust point B") : 0; break;
            case 0x7C: n = md ? snprintf(bd, sizeof bd, "A-B erase: confirm point A") : 0; break;
            case 0x7D: n = md ? snprintf(bd, sizeof bd, "confirm A-B erase") : 0; break;
            case 0x83: n = md ? 0 : snprintf(bd, sizeof bd, "set disc memo, part 2"); break;
            case 0x88: n = md ? 0 : snprintf(bd, sizeof bd, "programming (edit program 1)"); break;
            case 0x8A: n = md ? 0 : snprintf(bd, sizeof bd, "set program 1"); break;
            case 0x92: n = md ? 0 : snprintf(bd, sizeof bd, "get artist memos"); break;
            case 0x93: n = md ? 0 : snprintf(bd, sizeof bd, "set artist name"); break;
            case 0x94: n = md ? 0 : snprintf(bd, sizeof bd, "get artist data"); break;
            case 0x95: n = md ? 0 : (len > 3 ? snprintf(bd, sizeof bd, "assign artist %u to disc %u", d[3], cd_disc(d[2])) : 0); break;
            case 0x96: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, d[2] ? "artist mode: artist %u" : "artist mode OFF", d[2]) : 0); break;
            case 0x97: n = md ? snprintf(bd, sizeof bd, "disc info") : 0; break;
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xE8:
                n = md ? 0 : snprintf(bd, sizeof bd, "IR remote code over the bus (legacy Linux driver flag)"); break;
            case 0x44: n = len > 2 ? snprintf(bd, sizeof bd, "get info of disc %u (201 = current)", cd_disc(d[2])) : 0; break;
            case 0x45: n = len > 3 ? (d[3] == 0 ? snprintf(bd, sizeof bd, "get length of the current track of disc %u (track 00 = the one playing)", cd_disc(d[2])) : snprintf(bd, sizeof bd, "get length of disc %u track %u", cd_disc(d[2]), bcd(d[3]))) : 0; break;
            case 0x48: n = len > 2 ? snprintf(bd, sizeof bd, "get memo2 of disc %u", cd_disc(d[2])) : 0; break;
            case 0x50: n = len > 2 ? snprintf(bd, sizeof bd, "play disc %u track %u", cd_disc(d[2]), len > 3 ? bcd(d[3]) : 1) : 0; break;
            case 0x51: n = len > 2 ? snprintf(bd, sizeof bd, "cue disc %u track %u", cd_disc(d[2]), len > 3 ? bcd(d[3]) : 1) : 0; break;
            case 0x5E: n = snprintf(bd, sizeof bd, "fade out %u s", len > 2 ? d[2] : 30); break;
            case 0x5F: n = snprintf(bd, sizeof bd, "ramp up %u s", len > 2 ? d[2] : 30); break;
            case 0x6A: n = snprintf(bd, sizeof bd, "query model name"); break;
            case 0x72: n = md ? (len > 2 ? snprintf(bd, sizeof bd, "record date of track %u", d[2]) : 0) : snprintf(bd, sizeof bd, "query disc list"); break;
            case 0x80: n = md ? 0 : snprintf(bd, sizeof bd, "set disc memo"); break;
            case 0x90: n = md ? 0 : snprintf(bd, sizeof bd, "dump all memos"); break;
            case 0x98: n = md ? snprintf(bd, sizeof bd, "set disc title, first 14 chars") : (len > 2 ? snprintf(bd, sizeof bd, "get CD Text of disc %u%s", cd_disc(d[2]), len > 3 && d[3] == 1 ? " (first packet only)" : "") : 0); break;
            case 0x99: n = md ? snprintf(bd, sizeof bd, "set disc title, more chars") : 0; break;
            case 0x9A: n = md ? snprintf(bd, sizeof bd, "set track title, first 14 chars") : 0; break;
            case 0x9B: n = md ? snprintf(bd, sizeof bd, "set track title, more chars") : 0; break;
            default: break;
            }
            if (n <= 0) n = snprintf(bd, sizeof bd, "cmd %02X", d[1]);
        } else {
            switch (d[1]) {
            case 0x00: n = snprintf(bd, sizeof bd, "playing"); break;
            case 0x01: n = snprintf(bd, sizeof bd, "stopped"); break;
            case 0x02: n = snprintf(bd, sizeof bd, "paused"); break;
            case 0x04: n = md ? snprintf(bd, sizeof bd, "recording") : 0; break;
            case 0x03: n = snprintf(bd, sizeof bd, "TOC updated (also after eject)"); break;
            case 0x05: n = md ? snprintf(bd, sizeof bd, "eject / ignored") : snprintf(bd, sizeof bd, "no disc (or disc loaded backwards)"); break;
            case 0x06: n = snprintf(bd, sizeof bd, md ? "moving" : "unloading disc"); break;
            case 0x07: n = md ? snprintf(bd, sizeof bd, "record pause") : 0; break;
            case 0x08: n = snprintf(bd, sizeof bd, "ready (TOC read)"); break;
            case 0x09: n = snprintf(bd, sizeof bd, "start of track"); break;
            case 0x0C: n = snprintf(bd, sizeof bd, "about 30 s left on track"); break;
            case 0x0E: n = snprintf(bd, sizeof bd, "already done / unavailable"); break;
            case 0x0F: n = snprintf(bd, sizeof bd, "error (bad command)"); break;
            case 0x10: n = snprintf(bd, sizeof bd, "no memo"); break;
            case 0x11: n = snprintf(bd, sizeof bd, "no delete file"); break;
            case 0x12: case 0x13: n = snprintf(bd, sizeof bd, "no group"); break;
            case 0x14: n = snprintf(bd, sizeof bd, "invalid disc"); break;
            case 0x15: n = snprintf(bd, sizeof bd, "invalid track"); break;
            case 0x16: n = snprintf(bd, sizeof bd, md ? "no disc name on the disc" : "no name"); break;
            case 0x17: n = snprintf(bd, sizeof bd, "no track name"); break;
            case 0x19: n = snprintf(bd, sizeof bd, md ? "no group" : "no bar code (answer to 63)"); break;
            case 0x1A: n = md ? 0 : snprintf(bd, sizeof bd, "no 'YYYY' info (answer to 64)"); break;
            case 0x1B: n = md ? 0 : snprintf(bd, sizeof bd, "program/artist edit failed"); break;
            case 0x1C: n = md ? 0 : snprintf(bd, sizeof bd, "no memo (answer to 48)"); break;
            case 0x1D: n = snprintf(bd, sizeof bd, md ? "no CD text" : "no track title"); break;
            case 0x18: n = snprintf(bd, sizeof bd, "door open"); break;
            case 0x1F: n = snprintf(bd, sizeof bd, "memo/title written"); break;
            case 0x20: n = md ? snprintf(bd, sizeof bd, "invalid disc") : 0; break;
            case 0x2E: n = snprintf(bd, sizeof bd, "power ON"); break;
            case 0x2F: n = snprintf(bd, sizeof bd, "power OFF"); break;
            case 0x31: n = md ? snprintf(bd, sizeof bd, "unknown (answer to combine)") : 0; break;
            case 0x33: n = md ? snprintf(bd, sizeof bd, "complete") : 0; break;
            case 0x40: if (len > 3) { ascii(d + 3, len - 3, t, sizeof t); n = snprintf(bd, sizeof bd, "memo of disc %u: \"%s\"", cd_disc(d[2]), t); } break;
            case 0x42: if (len > 3) { ascii(d + 3, len - 3, t, sizeof t); n = snprintf(bd, sizeof bd, "group %u name \"%s\"", bcd(d[2]), t); } break;
            case 0x43: n = snprintf(bd, sizeof bd, "group disc list (discs 1-104)"); break;
            case 0x44: n = snprintf(bd, sizeof bd, "group disc list (discs 105-208)"); break;
            case 0x41: n = md ? 0 : (len > 3 ? snprintf(bd, sizeof bd, "disc %u: %u tracks available (not in the delete file)", cd_disc(d[2]), bits_set(d + 3, len - 3)) : 0); break;
            case 0x45: n = md ? 0 : snprintf(bd, sizeof bd, "group disc list (discs 209-312)"); break;
            case 0x46: n = md ? 0 : (len > 3 ? (ascii(d + 3, len - 3, t, sizeof t), snprintf(bd, sizeof bd, "memo of disc %u in the slave changer: \"%s\"", cd_disc(d[2]), t)) : 0); break;
            case 0x47: n = md ? 0 : snprintf(bd, sizeof bd, "CD Text disc detected"); break;
            case 0x48: if (len > 5) { ascii(d + 5, len - 5, t, sizeof t); n = snprintf(bd, sizeof bd, "%s of disc %u: \"%s\"", d[4] == 0 ? "memo" : "CD Text", cd_disc(d[2]), t); } break;
            case 0x49: if (len > 3) { ascii(d + 3, len - 3, t, sizeof t); n = snprintf(bd, sizeof bd, "memo/CD Text part %u: \"%s\"", d[2], t); } break;
            case 0x4A: if (len > 5 && !md) { ascii(d + 5, len - 5, t, sizeof t); n = snprintf(bd, sizeof bd, "CD Text title of track %u: \"%s\"", bcd(d[2]), t); } break;
            case 0x4C: n = md ? snprintf(bd, sizeof bd, "answer to 5C (unknown, mdslink)") : 0; break;
            case 0x4B: if (len > 3 && !md) { ascii(d + 3, len - 3, t, sizeof t); n = snprintf(bd, sizeof bd, "CD Text title part %u: \"%s\"", d[2], t); } break;
            case 0x50: n = len > 5 ? snprintf(bd, sizeof bd, "track info: disc %u track %u (%u:%02u)", cd_disc(d[2]), md ? d[3] : bcd(d[3]), bcd(d[4]), bcd(d[5])) : 0; break;
            case 0x51: n = len > 5 ? snprintf(bd, sizeof bd, "status: track %u index %u time %u:%02u", bcd(d[2]), d[3], bcd(d[4]), bcd(d[5])) : 0; break;
            case 0x52: n = len > 2 ? snprintf(bd, sizeof bd, "TOC read for disc %u (the front panel shows its track/time/memo)", cd_disc(d[2])) : 0; break;
            case 0x53: n = len > 2 ? snprintf(bd, sizeof bd, "no such disc %u", cd_disc(d[2])) : 0; break;
            case 0x54: n = md ? (len > 4 ? snprintf(bd, sizeof bd, "remaining time of disc %u: %u:%02u", d[2], bcd(d[3]), bcd(d[4])) : 0) : (len > 2 ? snprintf(bd, sizeof bd, "loading disc %u", cd_disc(d[2])) : 0); break;
            case 0x55: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, "disc %u has no memo", cd_disc(d[2])) : 0); break;
            case 0x58: n = len > 2 && !md ? snprintf(bd, sizeof bd, "now at disc %u (loading)", cd_disc(d[2])) : (md ? snprintf(bd, sizeof bd, "disc memo text line") : 0); break;
            case 0x59: n = md ? snprintf(bd, sizeof bd, "disc memo text (more)") : 0; break;
            case 0x5A: n = md ? snprintf(bd, sizeof bd, "track text") : 0; break;
            case 0x5D: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, "BCD %02X = %u", d[2], d[2]) : 0); break;
            case 0x5B: n = md ? snprintf(bd, sizeof bd, "track text (more)") : 0; break;
            case 0x60: n = len > 7 ? snprintf(bd, sizeof bd, "disc info: disc %u, %u tracks, length %u:%02u:%02u", cd_disc(d[2]), bcd(d[4]), bcd(d[5]), bcd(d[6]), bcd(d[7])) : snprintf(bd, sizeof bd, "disc info"); break;
            case 0x61:
                if (len > 3 && md) n = snprintf(bd, sizeof bd, "capacity %u disc(s), model id %02X", d[2], d[3]);
                else if (len > 3) n = snprintf(bd, sizeof bd, "capacity: %s (%02X), features %02X", d[2] == 0x01 ? "1 disc" : d[2] == 0x05 ? "5 discs" : d[2] == 0x64 ? "300 discs" :
                                               d[2] == 0x50 || d[2] == 0x51 ? "50(+1) discs (BigDave: speculated)" : d[2] == 0xC8 ? "400 discs" : d[2] == 0xFE ? "200 discs" : d[2] == 0 ? "100 discs" : "?", d[2], d[3]);
                else if (len > 2) n = snprintf(bd, sizeof bd, "capacity code %02X", d[2]);
                break;
            case 0x62: n = len > 5 ? snprintf(bd, sizeof bd, "track length: disc %u track %u = %u:%02u", cd_disc(d[2]), bcd(d[3]), bcd(d[4]), bcd(d[5])) : 0; break;
            case 0x63: n = md ? 0 : (len > 9 ? snprintf(bd, sizeof bd, "bar code of disc %u: %02X %02X %02X %02X %02X %02X %02X", cd_disc(d[2]), d[3], d[4], d[5], d[6], d[7], d[8], d[9]) : 0); break;
            case 0x64: n = md ? 0 : (len > 10 ? snprintf(bd, sizeof bd, "'YYYY' info of track %u: %02X %02X %02X %02X %02X %02X %02X %02X", bcd(d[2]), d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10]) : 0); break;
            case 0x6A: ascii(d + 2, len - 2, t, sizeof t); n = snprintf(bd, sizeof bd, "model name \"%s\"", t); break;
            case 0x70:   /* 70 S1 S2 S3 DISC TRACK. S1: see s1_text(). CD S2 (MSB first): 80 discs known, 40 all-disc mode, 30 repeat (10 all, 20 one),
                          * low nibble 1 shuffle / 4,5,6 program 1-3. MD S2: 01 shuffle, 02 program, 08 repeat all, 10 repeat one, 20 writable;
                          * MD S3: bits 2..0 input (1 analog, 3 optical, 5 coax), 80 mono */
                if (len > 6) {
                    char st[64]; s1_text(d[2], st, sizeof st);
                    if (md)
                        n = snprintf(bd, sizeof bd, "status: %s, disc %u track %u%s%s%s%s%s, input %s%s", st, d[5], d[6], (d[3] & 1) ? ", shuffle" : "", (d[3] & 2) ? ", program" : "",
                                     (d[3] & 8) ? ", repeat all" : "", (d[3] & 0x10) ? ", repeat 1" : "", (d[3] & 0x20) ? ", writable" : "",
                                     (d[4] & 7) == 1 ? "analog" : (d[4] & 7) == 3 ? "optical" : (d[4] & 7) == 5 ? "coax" : "?", (d[4] & 0x80) ? " mono" : "");
                    else {
                        unsigned lo = d[3] & 15, rp = (d[3] >> 4) & 3;
                        char md2[40] = "";
                        if (lo == 1) strcpy(md2, ", shuffle"); else if (lo >= 4 && lo <= 6) snprintf(md2, sizeof md2, ", program %u", lo - 3); else if (lo) snprintf(md2, sizeof md2, ", mode %X", lo);
                        n = snprintf(bd, sizeof bd, "status: %s, disc %u track %u (%s, %s, repeat %s%s)", st, cd_disc(d[5]), bcd(d[6]),
                                     (d[3] & 0x80) ? "discs known" : "scanning discs", (d[3] & 0x40) ? "all-disc mode" : "single disc", rp == 0 ? "off" : rp == 1 ? "all" : "one", md2);
                    }
                }
                break;
            case 0x71: n = md ? snprintf(bd, sizeof bd, "after TOC read / disc inserted") : (len > 2 ? snprintf(bd, sizeof bd, "disc memory info: %s%s%s", (d[2] & 1) ? "memo " : "", (d[2] & 2) ? "delete-file " : "", (d[2] & 0x30) ? "CD-Text" : (d[2] & 3) ? "" : "none") : 0); break;
            case 0x74: n = md ? 0 : snprintf(bd, sizeof bd, "disc list (discs 209-312)"); break;
            case 0x75: n = md ? 0 : snprintf(bd, sizeof bd, "disc list (discs 313-416)"); break;
            case 0x78: n = md ? 0 : snprintf(bd, sizeof bd, "group disc list (discs 313-416)"); break;
            case 0x80: n = md ? 0 : (len > 4 ? (ascii(d + 4, len - 4, t, sizeof t), snprintf(bd, sizeof bd, "memo of disc %u, part %u: \"%s\"", cd_disc(d[2]), d[3], t)) : 0); break;
            case 0x81: n = md ? 0 : (len > 4 ? (ascii(d + 4, len - 4, t, sizeof t), snprintf(bd, sizeof bd, "slave memo of disc %u, part %u: \"%s\"", cd_disc(d[2]), d[3], t)) : 0); break;
            case 0x83: n = md ? 0 : snprintf(bd, sizeof bd, "door closed"); break;
            case 0x92: n = md ? 0 : (len > 3 ? (ascii(d + 4, len - 4, t, sizeof t), snprintf(bd, sizeof bd, "artist %u name (part %u): \"%s\"", d[2], d[3], t)) : 0); break;
            case 0x93: n = md ? 0 : (len > 2 ? snprintf(bd, sizeof bd, "artist %u not set", d[2]) : 0); break;
            case 0x94: n = md ? 0 : snprintf(bd, sizeof bd, "artist data (packet %u: artist of 16 discs)", len > 2 ? d[2] : 0); break;
            case 0x96: n = md ? 0 : (len > 5 ? snprintf(bd, sizeof bd, "group %u, artist %u (max groups %u, max artists %u)", d[2], d[3], d[4], d[5]) : 0); break;
            case 0x72: n = snprintf(bd, sizeof bd, md ? "record date" : "disc list (discs 1-104)"); break;
            case 0x73: n = md ? 0 : snprintf(bd, sizeof bd, "disc list (discs 105-208)"); break;
            default: break;
            }
            if (n <= 0) n = snprintf(bd, sizeof bd, "reply %02X", d[1]);
        }
        if (!unit) return (size_t)snprintf(out, out_sz, "CD(all) %s: %s", reply ? "reply" : "cmd", bd);
        return (size_t)snprintf(out, out_sz, "%s%u %s: %s", md ? "MD" : "CD", unit, reply ? "reply" : "cmd", bd);
    }
    return 0;
}

const char *slink_sf_name(unsigned n) { return sf_name((uint8_t)n); }
