#include <stdio.h>
#include <string.h>
#include "state.h"
#include "decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct { uint16_t f; uint8_t fl; char name[9]; bool have_f, have_n; } preset_t;

static struct {
    int power, mute;                    /* -1 unknown */
    int audio, video;                   /* input codes, -1 unknown */
    char labels[0x20][9];
    char amp_name[17];
    int sf;                             /* sound field 0..29, -1 unknown */
    struct { bool valid; uint16_t f; int8_t g; } eq[3][3];
    struct { bool valid; int8_t v; } par[9];   /* rear, center, sub, rear balance, effect, reverb, DRC, LFE, wall type */
    int sfp, spk, idev;                 /* CB 70: sound field preset, speaker configuration bitmap; C8 70: S-Link unit on the input (-1 unknown) */
    struct { bool valid; uint8_t bank, preset, fl, level; uint16_t f; char name[9]; } tun;
    preset_t pre[3][10];
    int cc, itype, snd;                 /* amp flags (C8 70 byte 4: bit1 mute, bit3 tape loop, bit4 5.1 input, bit5 auto-function off),
                                           input type (C8 43), 2nd audio source (C8 71); -1 unknown */
    uint8_t info[3][8], infon[3];       /* model/info frames C8/C9/CB 61 (amp, tuner, surround): raw bytes after the code */
} S = { .power = -1, .mute = -1, .audio = -1, .video = -1, .sf = -1, .cc = -1, .itype = -1, .snd = -1, .sfp = -1, .spk = -1, .idev = -1 };

static SemaphoreHandle_t s_mtx;
static void (*s_cb)(void);

void state_init(void) { s_mtx = xSemaphoreCreateMutex(); }
void state_set_listener(void (*cb)(void)) { s_cb = cb; }

bool state_tuner(unsigned *f, bool *am, char *name, size_t n)
{
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = S.tun.valid && S.tun.f > 0;
    if (ok) { *f = S.tun.f; *am = (S.tun.fl & 1) != 0; strncpy(name, S.tun.name, n - 1); name[n - 1] = 0; }
    xSemaphoreGive(s_mtx);
    return ok;
}

/* The amp's power-off reply is often garbled on the bus (the line drops while it shuts down), so the
 * reply is never decoded: record the power state when our own PWRON/PWROFF was sent. */
void state_set_power(int on)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ch = S.power != on;
    S.power = on;
    xSemaphoreGive(s_mtx);
    if (ch && s_cb) s_cb();
}

const char *state_input_name(int c)
{
    switch (c) {
    case 0x00: return "TUNER"; case 0x01: return "PHONO"; case 0x02: return "CD"; case 0x03: return "DAT";
    case 0x04: return "MD";    case 0x05: return "TAPE";  case 0x07: return "DIGITAL";
    case 0x10: return "VIDEO1"; case 0x11: return "VIDEO2"; case 0x12: return "VIDEO3"; case 0x13: return "VIDEO4";
    case 0x15: case 0x19: return "DVD"; case 0x16: return "SAT"; case 0x17: return "TV";
    default: return NULL;
    }
}

/* printable ASCII, leading/trailing blanks trimmed, JSON-hostile characters replaced; max cap-1 chars + NUL */
static void ascii_trim(const uint8_t *p, size_t n, char *dst, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] && k < cap - 1; i++) {
        char c = (p[i] >= 32 && p[i] < 127 && p[i] != '"' && p[i] != '\\') ? (char)p[i] : '.';
        if (k == 0 && c == ' ') continue;
        dst[k++] = c;
    }
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = 0;
}

/* C8/C9/CB 61 "model/info" frames, sent by the units at power-on and on request (C0/C1/C3 22): keep the raw bytes */
static void store_info(int k, const uint8_t *d, size_t n)
{
    size_t m = n > 2 ? n - 2 : 0;
    if (m > sizeof S.info[0]) m = sizeof S.info[0];
    memcpy(S.info[k], d + 2, m);
    S.infon[k] = (uint8_t)m;
}

void state_feed(const uint8_t *d, size_t n)
{
    if (!s_mtx || n < 2) return;
    if (!(d[0] & 8) || ((d[0] >> 4) != 0xC && (d[0] >> 4) != 0x7)) return;     /* replies only */
    int who = d[0] & 7, c = d[1];
    bool ch = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (who == 0) {                                                            /* amplifier */
        switch (c) {
        case 0x2E: ch = S.power != 1; S.power = 1; break;
        case 0x2F: ch = S.power != 0; S.power = 0; break;
        case 0x06: ch = S.mute != 1; S.mute = 1; break;
        case 0x07: ch = S.mute != 0; S.mute = 0; break;
        case 0x70:
            if (n >= 5) {
                int pw = d[4] & 1, mu = (d[4] >> 1) & 1;
                int idev = n >= 6 ? d[5] : -1;
                ch = S.power != pw || S.mute != mu || S.audio != d[2] || S.video != d[3] || S.cc != d[4] || S.idev != idev;
                S.power = pw; S.mute = mu; S.audio = d[2]; S.video = d[3]; S.cc = d[4]; S.idev = idev;
            }
            break;
        case 0x43: if (n >= 3) { ch = S.itype != d[2]; S.itype = d[2]; } break;      /* input type: 1 optical, 2 coax, 4 analog */
        case 0x71: if (n >= 3) { ch = S.snd != d[2]; S.snd = d[2]; } break;          /* 2nd audio source */
        case 0x61: store_info(0, d, n); ch = true; break;
        case 0x48:
            if (n >= 4 && d[2] < 0x20) { ascii_trim(d + 3, n - 3, S.labels[d[2]], 9); ch = true; }
            break;
        case 0x6A:
            ascii_trim(d + 2, n - 2 > 16 ? 16 : n - 2, S.amp_name, sizeof S.amp_name); ch = true; break;
        default: break;
        }
    } else if (who == 1) {                                                     /* tuner */
        if (c == 0x70 && n >= 7) {
            uint16_t nf = (uint16_t)((d[5] << 8) | d[6]);
            if (nf != S.tun.f || ((d[4] ^ S.tun.fl) & 1)) S.tun.name[0] = 0;       /* another station: the old RDS name is stale */
            S.tun.valid = true; S.tun.bank = d[2]; S.tun.preset = d[3]; S.tun.fl = d[4];
            S.tun.f = nf; ch = true;
        } else if (c == 0x71 && n >= 6) {
            S.tun.level = d[4]; ch = true;
        } else if (c == 0x48 && n >= 5) {
            if (d[3] == 0xFF) { ascii_trim(d + 4, n - 4, S.tun.name, sizeof S.tun.name); ch = true; }
            else if (d[2] >= 1 && d[2] <= 3 && d[3] >= 1 && d[3] <= 10) {
                preset_t *p = &S.pre[d[2] - 1][d[3] - 1];
                ascii_trim(d + 4, n - 4, p->name, sizeof p->name); p->have_n = true; ch = true;
            }
        } else if (c == 0x61) {
            store_info(1, d, n); ch = true;
        } else if (c == 0x10) {
            /* "no name stored": nothing to record (name stays empty) */
        } else if (c == 0x4A && n >= 7 && d[2] >= 1 && d[2] <= 3 && d[3] >= 1 && d[3] <= 10) {
            preset_t *p = &S.pre[d[2] - 1][d[3] - 1];
            p->fl = d[4]; p->f = (uint16_t)((d[5] << 8) | d[6]); p->have_f = true; p->have_n = false; p->name[0] = 0; ch = true;
        }
    } else if (who == 3) {                                                     /* surround */
        if (c == 0x70 && n >= 3) {
            ch = S.sf != d[2]; S.sf = d[2];
            if (n >= 5) {                        /* CB 70 FF PP SS: preset and speaker bitmap (the bitmap is only reported with a field on) */
                int spk = d[2] ? d[4] : -1;
                ch = ch || S.sfp != d[3] || S.spk != spk;
                S.sfp = d[3]; S.spk = spk;
            }
            if (d[2] == 0) {                     /* field off: rear/sub/balance levels and the rear EQ answer "unavailable" */
                for (int i = 0; i < 4; i++) if (S.par[i].valid) { S.par[i].valid = false; ch = true; }
                for (int b = 0; b < 3; b++) if (S.eq[2][b].valid) { S.eq[2][b].valid = false; ch = true; }
            }
        }
        else if (c == 0x61) { store_info(2, d, n); ch = true; }
        else if (c == 0x50 && n >= 9 && d[2] < 3 && d[3] < 3) {
            S.eq[d[2]][d[3]].valid = true;
            S.eq[d[2]][d[3]].f = (uint16_t)((d[4] << 8) | d[5]);
            S.eq[d[2]][d[3]].g = (int8_t)d[6]; ch = true;
        } else if (n >= 4 && ((c >= 0x40 && c <= 0x47) || (c >= 0x80 && c <= 0x87))) {
            S.par[c & 7].valid = true; S.par[c & 7].v = (int8_t)d[2]; ch = true;
        } else if (n >= 4 && (c == 0x4B || c == 0x8B)) {           /* wall type -8 soft .. +8 hard */
            S.par[8].valid = true; S.par[8].v = (int8_t)d[2]; ch = true;
        }
    }
    xSemaphoreGive(s_mtx);
    if (ch && s_cb) s_cb();
}

#define P(...) do { if (n < sz) n += (size_t)snprintf(b + n, sz - n, __VA_ARGS__); } while (0)

size_t state_json(char *b, size_t sz, int vol_pct, int vol_x10)
{
    static const char *pn[9] = {"rear", "center", "sub", "bal", "fx", "rvb", "drc", "lfe", "wall"};
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = 0;
    const char *an = state_input_name(S.audio);
    P("{\"t\":\"state\",\"power\":%d,\"mute\":%d,\"in\":%d,\"inName\":\"%s\",\"video\":%d,\"vol\":%d,\"vol10\":%d,\"name\":\"%s\",",
      S.power, S.mute, S.audio, an ? an : "", S.video, vol_pct, vol_x10, S.amp_name);
    P("\"cc\":%d,\"itype\":%d,\"snd\":%d,\"idev\":%d,\"sfp\":%d,\"spk\":%d,\"info\":{", S.cc, S.itype, S.snd, S.idev, S.sfp, S.spk);
    static const char *ik[3] = {"amp", "tun", "sur"};
    for (int k = 0; k < 3; k++) {
        P("%s\"%s\":\"", k ? "," : "", ik[k]);
        for (int i = 0; i < S.infon[k]; i++) P("%s%02X", i ? " " : "", S.info[k][i]);
        P("\"");
    }
    P("},");
    P("\"labels\":{");
    bool first = true;
    for (int i = 0; i < 0x20; i++)
        if (S.labels[i][0]) { P("%s\"%d\":\"%s\"", first ? "" : ",", i, S.labels[i]); first = false; }
    P("},\"sf\":%d,\"sfName\":\"%s\",", S.sf, S.sf >= 0 ? slink_sf_name((unsigned)S.sf) : "");
    P("\"par\":{");
    first = true;
    for (int i = 0; i < 9; i++)
        if (S.par[i].valid) { P("%s\"%s\":%d", first ? "" : ",", pn[i], S.par[i].v); first = false; }
    P("},\"eq\":[");
    for (int p = 0; p < 3; p++) {
        P("%s[", p ? "," : "");
        for (int k = 0; k < 3; k++) {
            if (S.eq[p][k].valid) P("%s{\"f\":%u,\"g\":%d}", k ? "," : "", S.eq[p][k].f, S.eq[p][k].g);
            else P("%snull", k ? "," : "");
        }
        P("]");
    }
    P("],\"tuner\":");
    if (S.tun.valid)
        P("{\"band\":\"%s\",\"f\":%u,\"preset\":%u,\"bank\":%u,\"locked\":%d,\"stereo\":%d,\"mono\":%d,\"scan\":%d,\"level\":%u,\"name\":\"%s\"}",
          (S.tun.fl & 1) ? "AM" : "FM", S.tun.f, S.tun.preset, S.tun.bank, !!(S.tun.fl & 8), !!(S.tun.fl & 0x40),
          !!(S.tun.fl & 0x80), !!(S.tun.fl & 0x30), S.tun.level, S.tun.name);   /* 0x20 searching up, 0x10 searching down */
    else P("null");
    P("}");
    xSemaphoreGive(s_mtx);
    return n < sz ? n : sz - 1;
}

size_t state_presets_json(char *b, size_t sz)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = 0;
    P("{\"t\":\"presets\",\"presets\":[");
    bool first = true;
    for (int bk = 0; bk < 3; bk++)
        for (int i = 0; i < 10; i++) {
            preset_t *p = &S.pre[bk][i];
            if (!p->have_f) continue;
            P("%s{\"b\":%d,\"n\":%d,\"f\":%u,\"fl\":%u,\"name\":\"%s\"}", first ? "" : ",", bk + 1, i + 1, p->f, p->fl,
              p->have_n ? p->name : "");
            first = false;
        }
    P("]}");
    xSemaphoreGive(s_mtx);
    return n < sz ? n : sz - 1;
}
