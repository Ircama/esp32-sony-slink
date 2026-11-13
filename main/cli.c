/* Line-oriented CLI over the ESP32-C3 built-in USB Serial/JTAG (CDC-ACM).
 *
 * Host -> device (one command per line, case-insensitive):
 *   "70 14"            raw S-Link frame, hex bytes (device, command, params...)
 *   VOLUP / VOLDN / MUTE / UNMUTE / PWRON / PWROFF ...   (see HELP)
 * Device -> host:
 *   "RX 70 14 ..."     frame received from the S-Link bus
 *   "OK" / "ERR ..."   result of a command
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/usb_serial_jtag.h"
#include "slink.h"
#include "cli.h"
#include "decode.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "state.h"
#include "settings.h"
#include "net.h"
#if SOC_WIFI_SUPPORTED
#include "ota.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"

#define TUNER_DEV ((s_dev & 0xF8) | 1)   /* C0 -> C1, 70 -> 71 */
#define SURR_DEV  ((s_dev & 0xF8) | 3)   /* C0 -> C3, 70 -> 73 */
#define CRLF "\r\n"
#define CLI_LINE_MAX 200

static SemaphoreHandle_t s_out_mtx;
static uint8_t s_dev = CONFIG_SLINK_DEFAULT_DEVICE;
static int s_repeat = 1;
static volatile bool s_decode = true;   /* DECODE ON/OFF: annotate RX lines (default on) */
/* last tuner status (C9 70 BB NN FL HH LL) and EQ readings (CB 50 ...), filled by rx_task */
/* last preset data read by PRESETS: C1 4A -> frequency, C1 48 BB NN -> name (or C9 10 = none) */
static struct { volatile uint8_t fl; volatile uint16_t f; volatile uint32_t seq, nm_seq; char name[10]; } s_pre;
static struct { volatile bool valid; volatile uint8_t bank, preset, fl, level; volatile uint16_t f; volatile uint32_t seq, lvl_seq, name_seq; char name[10]; } s_tun;
static struct { volatile bool valid; volatile uint16_t f; volatile int8_t g; volatile uint32_t seq; } s_eq[3][3];
static volatile bool s_echo;   /* ECHO ON/OFF: echo typed characters back (default off) */

#define MAX_SINKS 8
static cli_sink_t s_sinks[MAX_SINKS];

int cli_sink_add(cli_sink_t fn)
{
    int rc = -1;
    xSemaphoreTake(s_out_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SINKS && rc < 0; i++) if (s_sinks[i] == fn) rc = 0;
    for (int i = 0; i < MAX_SINKS && rc < 0; i++) if (!s_sinks[i]) { s_sinks[i] = fn; rc = 0; }
    xSemaphoreGive(s_out_mtx);
    return rc;
}

void cli_sink_remove(cli_sink_t fn)
{
    xSemaphoreTake(s_out_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SINKS; i++) if (s_sinks[i] == fn) s_sinks[i] = NULL;
    xSemaphoreGive(s_out_mtx);
}

static void vout(const char *fmt, va_list ap)
{
    char buf[1400];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n <= 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    xSemaphoreTake(s_out_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SINKS; i++) if (s_sinks[i]) s_sinks[i](buf, (size_t)n);
    if (usb_serial_jtag_is_connected()) {
        for (int off = 0; off < n; ) {
            int chunk = n - off > 128 ? 128 : n - off;
            int w = usb_serial_jtag_write_bytes(buf + off, chunk, pdMS_TO_TICKS(100));
            if (w <= 0) break;   /* host not reading */
            off += w;
        }
    }
    xSemaphoreGive(s_out_mtx);
}

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vout(fmt, ap);
    va_end(ap);
}

void cli_out(const char *fmt, ...)               /* the same output as the commands' own, for the other modules (ota.c) */
{
    va_list ap;
    va_start(ap, fmt);
    vout(fmt, ap);
    va_end(ap);
}

static void rx_task(void *arg)
{
    slink_frame_t f;
    for (;;) {
        if (!slink_receive(&f, portMAX_DELAY)) continue;
        char line[16 + 3 * SLINK_MAX_FRAME + 24 + 140];
        int n = snprintf(line, sizeof line, "RX");
        for (int i = 0; i < f.len; i++)
            n += snprintf(line + n, sizeof line - n, " %02X", f.data[i]);
        if (f.partial_bits) n += snprintf(line + n, sizeof line - n, " +%dbit", f.partial_bits);
        if (f.error) n += snprintf(line + n, sizeof line - n, " !timing");
        if (!f.partial_bits && !f.error) {
            state_feed(f.data, f.len);
            if (f.len >= 5 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x48 && f.data[3] == 0xFF) {  /* station name */
                int k = 0;
                for (int i = 4; i < f.len && i < 12 && f.data[i] && k < 8; i++)
                    s_tun.name[k++] = (f.data[i] >= 32 && f.data[i] < 127) ? (char)f.data[i] : '.';
                while (k && s_tun.name[k - 1] == ' ') k--;
                s_tun.name[k] = 0;
                s_tun.name_seq++;
            } else if (f.len >= 5 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x48) {          /* preset name */
                int k = 0;
                for (int i = 4; i < f.len && i < 12 && f.data[i] && k < 8; i++)
                    s_pre.name[k++] = (f.data[i] >= 32 && f.data[i] < 127) ? (char)f.data[i] : '.';
                while (k && s_pre.name[k - 1] == ' ') k--;
                s_pre.name[k] = 0;
                s_pre.nm_seq++;
            } else if (f.len == 2 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x10) {          /* "no name stored" */
                s_pre.name[0] = 0;
                s_pre.nm_seq++;
            } else if (f.len == 7 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x4A) {          /* preset frequency */
                s_pre.fl = f.data[4];
                s_pre.f = (uint16_t)((f.data[5] << 8) | f.data[6]);
                s_pre.seq++;
            } else if (f.len == 7 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x70) {          /* tuner status */
                s_tun.bank = f.data[2]; s_tun.preset = f.data[3]; s_tun.fl = f.data[4];
                s_tun.f = (uint16_t)((f.data[5] << 8) | f.data[6]);
                s_tun.valid = true; s_tun.seq++;
            } else if (f.len == 6 && (f.data[0] & 0xF7) == 0xC1 && f.data[1] == 0x71) {   /* signal level event */
                s_tun.level = f.data[4];
                if (f.data[5]) s_tun.lvl_seq++;      /* ST=01: measurement settled */
            } else if (f.len == 9 && (f.data[0] & 0xF7) == 0xC3 && f.data[1] == 0x50 &&
                       f.data[2] < 3 && f.data[3] < 3) {                                   /* EQ reading */
                int p = f.data[2], b = f.data[3];
                s_eq[p][b].f = (uint16_t)((f.data[4] << 8) | f.data[5]);
                s_eq[p][b].g = (int8_t)f.data[6];
                s_eq[p][b].valid = true; s_eq[p][b].seq++;
            }
        }
        if (s_decode && f.len >= 2) {
            char dec[120];
            if (slink_decode(f.data, f.len, dec, sizeof dec) > 0)
                n += snprintf(line + n, sizeof line - n, "   ; %s", dec);
        }
        out("%s\r\n", line);
    }
}

static bool do_send(const uint8_t *d, size_t n)
{
    int rc = slink_send(d, n, s_repeat);
    if (rc == 0) {
        char line[8 + 3 * SLINK_MAX_FRAME];
        int k = snprintf(line, sizeof line, "TX");
        for (size_t i = 0; i < n; i++) k += snprintf(line + k, sizeof line - k, " %02X", d[i]);
        out("%s\r\nOK\r\n", line);
    } else if (rc == -3) out("ERR line test active (BLINK OFF / LEVEL OFF first)\r\n");
    else if (rc == -2) out("ERR busy (line never idle)\r\n");
    else out("ERR bad frame\r\n");
    return rc == 0;
}

static bool send2(uint8_t dev, uint8_t c) { uint8_t b[2] = {dev, c}; return do_send(b, 2); }
static void send3(uint8_t dev, uint8_t c, uint8_t p) { uint8_t b[3] = {dev, c, p}; do_send(b, 3); }

static int input_code(const char *s);
static const struct { const char *name; uint8_t code; } k_inputs[] = {
    /* labels measured on STR-DA50ES (INPUTS cmd); 03/04 both "MD/DAT", 15/19 "DVD", 16/17 "TV/LD" */
    {"TUNER", 0x00}, {"PHONO", 0x01}, {"CD", 0x02}, {"DAT", 0x03}, {"MD", 0x04}, {"MDDAT", 0x04},
    {"TAPE", 0x05}, {"VIDEO1", 0x10}, {"VIDEO2", 0x11}, {"VIDEO3", 0x12},
    {"DVD", 0x15}, {"TV", 0x17}, {"LD", 0x17}, {"SAT", 0x16},
    {"DIGITAL", 0x07}, {"VIDEO4", 0x13},   /* not present on the DA50ES (answer error) */
};

/* input name or hexadecimal code -> code, -1 if unknown */
static int input_code(const char *s)
{
    for (size_t i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++) if (!strcmp(s, k_inputs[i].name)) return k_inputs[i].code;
    char *e; unsigned long v = strtoul(s, &e, 16);
    return (*s && *e == 0 && v <= 0xFF) ? (int)v : -1;
}


/* Motorised volume knob: no read-back, so the firmware keeps an ESTIMATE of the position, expressed in UP-steps
 * from the minimum (reset to unknown at boot and invalid after the knob is turned by hand). Measured on the
 * STR-DA50ES at 30 ms/frame: full travel = 420 steps UP (410-420) but ~445 steps DOWN (the knob is not symmetric:
 * a DOWN step moves ~94% of an UP step). Going to an end adds only a small margin so the motor does not spin idle. */
/* the three numbers are settings (VOLCFG, Settings tab of the web interface): defaults 420 / 445 / 10 */
#define VOL_UP_TRAVEL   ((int)g_set.vol_up)
#define VOL_DOWN_TRAVEL ((int)g_set.vol_down)
#define VOL_MARGIN      ((int)g_set.vol_margin)
static int s_vol_pos = -1;   /* UP-steps from minimum, -1 = unknown */

static int down_to_up(int n)  { return (n * VOL_UP_TRAVEL + VOL_DOWN_TRAVEL / 2) / VOL_DOWN_TRAVEL; }
static int up_to_down(int n)  { return (n * VOL_DOWN_TRAVEL + VOL_UP_TRAVEL / 2) / VOL_UP_TRAVEL; }

static int vol_send(uint8_t dev, uint8_t cmd, int n)
{
    uint8_t b[2] = {dev, cmd};
    while (n > 0) {
        int k = n > 250 ? 250 : n;
        int rc = slink_send(b, 2, k);
        if (rc != 0) return rc;
        n -= k;
    }
    return 0;
}

/* move to an end (0 = minimum, otherwise maximum); returns steps sent or -1 on error */
static int vol_goto_end(bool max)
{
    int need;
    if (max) need = (s_vol_pos < 0 ? VOL_UP_TRAVEL : VOL_UP_TRAVEL - s_vol_pos) + VOL_MARGIN;
    else     need = (s_vol_pos < 0 ? VOL_DOWN_TRAVEL : up_to_down(s_vol_pos)) + VOL_MARGIN;
    if (vol_send(s_dev, max ? 0x14 : 0x15, need) != 0) return -1;
    s_vol_pos = max ? VOL_UP_TRAVEL : 0;
    return need;
}

/* FM station table (frequency + RDS name), filled by FMSCAN and kept in NVS so it survives the resets that
 * happen every time the serial port is opened. The S-Link protocol has no "tune by name": we look the name up here. */
#define ST_MAX 48
typedef struct { uint16_t f; char name[9]; } station_t;   /* f in 10 kHz units (9850 = 98.50 MHz) */
static station_t s_st[ST_MAX];
static int s_st_n;

static void st_load(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    size_t len = sizeof s_st;
    if (nvs_get_u8(h, "stn", &n) == ESP_OK && nvs_get_blob(h, "st", s_st, &len) == ESP_OK && n <= ST_MAX) s_st_n = n;
    nvs_close(h);
}

static void st_save(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "stn", (uint8_t)s_st_n);
    nvs_set_blob(h, "st", s_st, sizeof s_st);
    nvs_commit(h);
    nvs_close(h);
}

static void st_add(unsigned f, const char *name)
{
    if (s_st_n >= ST_MAX) return;
    while (*name == ' ') name++;                       /* RDS names often start with blanks */
    s_st[s_st_n].f = (uint16_t)f;
    strncpy(s_st[s_st_n].name, name, 8);
    s_st[s_st_n].name[8] = 0;
    s_st_n++;
}

/* Presets stored by the bridge ("FAV"). The tuner's own 30 presets cannot be written over S-Link (only from the front
 * panel), so the bridge keeps its own list: frequency + band + name, in NVS. Recalling one is a direct tune. */
#define FAV_MAX 30
typedef struct { uint16_t f; uint8_t am; char name[9]; } fav_t;   /* f: FM in 10 kHz units, AM in kHz */
static fav_t s_fav[FAV_MAX];
static int s_fav_n;

static void fav_load(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    size_t len = sizeof s_fav;
    if (nvs_get_u8(h, "favn", &n) == ESP_OK && nvs_get_blob(h, "fav", s_fav, &len) == ESP_OK && n <= FAV_MAX) s_fav_n = n;
    nvs_close(h);
}

static void fav_save(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "favn", (uint8_t)s_fav_n);
    nvs_set_blob(h, "fav", s_fav, sizeof s_fav);
    nvs_commit(h);
    nvs_close(h);
}

/* case-insensitive search: exact name first, then substring. Returns number of matches, first in *first. */
static int st_find(const char *q, int *first)
{
    char u[10];
    int k = 0, matches = 0;
    *first = -1;
    for (const char *s = q; *s && k < 9; s++) u[k++] = (char)toupper((unsigned char)*s);
    u[k] = 0;
    while (k && u[k - 1] == ' ') u[--k] = 0;
    for (int pass = 0; pass < 2 && matches == 0; pass++)
        for (int i = 0; i < s_st_n; i++) {
            char nm[10];
            for (int j = 0; j < 9; j++) nm[j] = (char)toupper((unsigned char)s_st[i].name[j]);
            int e = (int)strlen(nm);
            while (e && nm[e - 1] == ' ') nm[--e] = 0;
            if (pass == 0 ? !strcmp(nm, u) : strstr(nm, u) != NULL) { if (*first < 0) *first = i; matches++; }
        }
    return matches;
}

/* ask the amp for the label of every input code; answers print decoded as RX lines */
static void cmd_inputs(void)
{
    static const uint8_t codes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x10, 0x11, 0x12,
                                    0x13, 0x15, 0x16, 0x17, 0x19};
    for (size_t i = 0; i < sizeof codes; i++) {
        uint8_t b[3] = {s_dev, 0x48, codes[i]};
        if (slink_send(b, 3, 1) != 0) { out("ERR busy" CRLF); break; }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    out("OK inputs queried (codes the amp does not have answer 'error')" CRLF);
}

/* PRESETS [bank]: frequency (C1 4A BB NN) and name (C1 48 BB NN) of the 10 presets of one bank, or of all 3.
 * Same sequence the original remote runs at setup ("TUNER PRESET NAME"). Works with any input selected. */
static void cmd_presets(int only_bank)
{
    int shown = 0;
    for (int bank = 1; bank <= 3; bank++) {
        if (only_bank && bank != only_bank) continue;
        for (int nn = 1; nn <= 10; nn++) {
            uint32_t s0 = s_pre.seq, n0 = s_pre.nm_seq;
            uint8_t q[4] = {TUNER_DEV, 0x4A, (uint8_t)bank, (uint8_t)nn};
            slink_send(q, 4, 1);
            for (int i = 0; i < 10 && s_pre.seq == s0; i++) vTaskDelay(pdMS_TO_TICKS(50));
            bool hf = s_pre.seq != s0;
            unsigned f = s_pre.f; uint8_t fl = s_pre.fl;
            q[1] = 0x48;
            slink_send(q, 4, 1);
            for (int i = 0; i < 10 && s_pre.nm_seq == n0; i++) vTaskDelay(pdMS_TO_TICKS(50));
            bool hn = s_pre.nm_seq != n0;
            if (!hf) { out("%d/%-2d  (no answer)" CRLF, bank, nn); continue; }
            if (fl & 1) out("%d/%-2d  AM %4u kHz", bank, nn, f);
            else        out("%d/%-2d  FM %3u.%02u MHz%s", bank, nn, f / 100, f % 100, f == 8750 ? " (empty)" : "");
            out("  %s" CRLF, (hn && s_pre.name[0]) ? s_pre.name : "-");
            shown++;
        }
    }
    out("OK %d presets read (87.50 MHz = never set; names exist only if stored with the amp panel)" CRLF, shown);
}

static void help(void)
{
    out("S-Link bridge. Commands (case-insensitive):" CRLF
        "  <hex bytes>        raw frame, e.g. 'C0 2E' (max %d bytes)" CRLF
        "  DEV <hex>          default device for mnemonics (now %02X; C0 verified on STR-DA50ES)" CRLF
        "  REPEAT <n>         send each frame n times (now %d)   GAP <ms> min time between frames (now %d)" CRLF
        "  PWRON | PWROFF | MUTE | UNMUTE | STATUS (amp+tuner+surround) | AMPNAME | INPUTS" CRLF
        "  PRESETS [bank]     frequency + name of the tuner presets (C1 4A / C1 48), all 3 banks if omitted" CRLF
        "  MODEL | SETUP      model info (22, 6A, 6B) / full remote-style download: names, labels, presets" CRLF
        "  VOL UP|DOWN [n<=250] | MIN | QMIN [limit%%] | MAX | SET <0-100%%> | POS [steps]  motor knob (position = estimate; QMIN = quick calibration)" CRLF
        "  WIFIPWR [tx ps listen]    WiFi power: tx 2-20 dBm|AUTO, ps AUTO|NONE|MIN|MAX, listen 1-10 beacons|AUTO (saved)" CRLF
        "  INPUT AV <audio> <video>  audio-only input (TUNER PHONO CD MD TAPE) + a different video input (VIDEO1-3 DVD SAT TV)" CRLF
        "  VOLCFG [up down margin]   knob travel in steps (default 420 / 445 / 10; saved)" CRLF
        "  CFG [PASS] | CFG SET <key> <value> | CFG APPLY   settings kept in flash (web backup/restore); STATION CLEAR|ADD|SAVE, FAV CLEAR|PUT: station tables" CRLF
        "  UITITLE [APP|PAGE \"text\"]   header / page title of the web interface (saved)" CRLF
        "  INPUT <name|hex>   TUNER PHONO CD MD DAT TAPE VIDEO1-3 DVD TV" CRLF
        "  TUNER FM|AM|SCAN+|SCAN-|PRESET+|PRESET-|MONO|STEREO  TUNE FM <MHz>|AM <kHz>  PRESET <bank 1-3> <n 1-10>" CRLF
        "  FAV [ADD [FM <MHz>|AM <kHz>] | DEL <n> | <n>]  presets kept by the bridge (max 30; the amp's own presets can only be stored from its panel)" CRLF
        "  FMSCAN             list all FM stations with level and RDS name (amp must be on TUNER input)" CRLF
        "  NAME [FORCE]       RDS name of the tuned FM station (on demand; gives way to other commands; a failure is remembered 2 min per frequency)" CRLF
        "  STATIONS | TUNE <name>|#n|101.5   stored FM stations (built by FMSCAN, kept in flash); tune by name/number/MHz" CRLF
        "  EQCURVE            draw the active EQ curve (frequency / dB) for front and rear" CRLF
        "  EQ                 read bass/mid/treble front+rear;  EQ F|R BASS|MID|TREBLE GAIN <-10..10> | FREQ <Hz>" CRLF
        "  SF ON|OFF|<1-29>   sound field;  REAR|SUB|BAL|DRC|LFE <value>  surround levels" CRLF,
        SLINK_MAX_FRAME, s_dev, s_repeat, slink_get_gap_ms());
    out("  PAD [ODPU|OD|PP]   bus pin mode: open-drain + internal pull-up (default) | open-drain | push-pull (saved)" CRLF
        "  SLINK <gpio>       GPIO connected to S-Link (now %d);  LED <gpio> [INV] | LED OFF  traffic LED" CRLF
        "  ECHO ON|OFF (now %s)   DECODE ON|OFF (now %s)   LOOPBACK ON|OFF (own frames as RX)" CRLF
        "  LINE | BLINK HIGH|LOW <ms> HIGH|LOW <ms> [n] | BLINK OFF | LEVEL HIGH|LOW|OFF   (line tests)" CRLF
        "  BLE ON|OFF   WEB ON|OFF   APP ON|OFF   enable/disable BLE UART, web portal, WiFi app interface (saved)" CRLF
        "  ALLOFF             power off every Sony unit on the bus (CD 1-3, MD, tuner, then the amplifier): the remote's SYSTEM/STANDBY \"ALL OFF\"" CRLF
        "  ECO ON|OFF         low-power profile: WiFi max modem sleep, CPU 40-80 MHz, 11 dBm TX (default ON, saved)" CRLF
        "  WIFI <ssid> [pass] | WIFI CLEAR   MQTT <mqtt://host> [user] [pass] | MQTT CLEAR   HOSTNAME <name>   NET (status)" CRLF
        "  OTA [UPDATE [url] [FORCE] | URL [url|DEFAULT] | PASS [text|OFF] | TOKEN [text|OFF] | ROLLBACK]   firmware update over WiFi: from the GitHub release or a URL; PASS protects the upload (POST /api/ota); (WiFi chips)" CRLF
        "  HELP" CRLF
        "Replies: 'TX ..' echo, 'OK', 'ERR ..', 'RX ..  ; decoded text' for every bus frame." CRLF,
        slink_get_gpio(), s_echo ? "ON" : "OFF", s_decode ? "ON" : "OFF");
}


/* ASCII plot of the 3-band EQ: x = frequency 100 Hz..20 kHz (log), y = gain +10..-10 dB.
 * '#' = band centre, '*' = straight line between band centres (schematic, not the real filter shape). */
#define EQ_W 61
static int eq_x(unsigned f)
{
    if (f < 100) f = 100;
    if (f > 20000) f = 20000;
    double t = (log10((double)f) - 2.0) / (log10(20000.0) - 2.0);
    return (int)(t * (EQ_W - 1) + 0.5);
}

static void draw_eq(int p)
{
    static const char *bn[3] = {"bass", "mid", "treble"};
    unsigned fr[3]; int g[3], x[3];
    out("EQ %s curve (3 bands):" CRLF, p == 0 ? "FRONT" : "REAR");
    for (int b = 0; b < 3; b++) {
        fr[b] = s_eq[p][b].f; g[b] = s_eq[p][b].g; x[b] = eq_x(fr[b]);
        out("  %-6s %5u Hz  %+d dB" CRLF, bn[b], fr[b], g[b]);
    }
    int row_of_col[EQ_W];
    for (int c = 0; c < EQ_W; c++) {
        double gv;
        if (c <= x[0]) gv = g[0];
        else if (c >= x[2]) gv = g[2];
        else {
            int a = c < x[1] ? 0 : 1;
            int span = x[a + 1] - x[a];
            gv = span ? g[a] + (double)(g[a + 1] - g[a]) * (c - x[a]) / span : g[a + 1];
        }
        row_of_col[c] = (int)(gv < 0 ? gv - 0.5 : gv + 0.5);
    }
    char line[EQ_W + 16];
    for (int db = 10; db >= -10; db--) {
        int n = snprintf(line, sizeof line, "%+3d |", db);
        for (int c = 0; c < EQ_W; c++) {
            char ch = db == 0 ? '-' : ' ';
            if (row_of_col[c] == db) ch = '*';
            for (int b = 0; b < 3; b++) if (c == x[b] && g[b] == db) ch = '#';
            line[n++] = ch;
        }
        line[n] = 0;
        out("%s" CRLF, line);
    }
    char ax[EQ_W + 16], lb[EQ_W + 24];
    memset(ax, '-', sizeof ax); memset(lb, ' ', sizeof lb);
    static const unsigned tk[] = {100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    int n0 = 5;
    for (size_t i = 0; i < sizeof tk / sizeof tk[0]; i++) {
        int cx = n0 + eq_x(tk[i]);
        ax[cx] = '+';
        char t[8];
        int tl = tk[i] >= 1000 ? snprintf(t, sizeof t, "%uk", tk[i] / 1000) : snprintf(t, sizeof t, "%u", tk[i]);
        for (int k = 0; k < tl && cx + k < (int)sizeof lb - 2; k++) lb[cx + k] = t[k];
    }
    ax[n0 + EQ_W] = 0; lb[n0 + EQ_W + 3] = 0;
    out("%s" CRLF "%s Hz" CRLF CRLF, ax, lb);
}

static bool parse_hex_line(char *line)
{
    uint8_t d[SLINK_MAX_FRAME];
    size_t n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p || v > 0xFF || n >= SLINK_MAX_FRAME) return false;
        d[n++] = (uint8_t)v;
        p = end;
        if (*p && *p != ' ' && *p != ',') return false;
    }
    if (!n) return false;
    do_send(d, n);
    return true;
}

static void handle_net(char *raw, char **tok, int nt);

static void handle_cmd(char *line);
static int split_args(char *raw, char **av, int max);
static unsigned s_last_fm = 9825, s_last_am = 720;   /* last frequency tuned per band (98.25 MHz / 720 kHz measured) */

static bool cmd_waiting(void);                   /* another command is queued behind the one running (defined with the queue) */

/* the volume estimate is not part of the amp state, so no frame announces its change: push it to the web/MQTT
 * clients explicitly after every VOL command */
static void handle(char *line)
{
    while (*line == ' ') line++;
    bool vol = (line[0] == 'V' || line[0] == 'v') && (line[1] == 'O' || line[1] == 'o') && (line[2] == 'L' || line[2] == 'l') &&
               (line[3] == ' ' || line[3] == 0);
    handle_cmd(line);
    if (vol) net_notify_state();
}

static void handle_cmd(char *line)
{
    char raw[CLI_LINE_MAX];
    strncpy(raw, line, sizeof raw - 1); raw[sizeof raw - 1] = 0;
    for (char *c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
    while (*line == ' ') line++;
    if (!*line) return;

    char *tok[8] = {0};
    char tmp[96];
    strncpy(tmp, line, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    int nt = 0;
    for (char *s = strtok(tmp, " \t"); s && nt < 8; s = strtok(NULL, " \t")) tok[nt++] = s;
    const char *c0 = tok[0];

    if (!strcmp(c0, "HELP") || !strcmp(c0, "?")) help();
    else if (!strcmp(c0, "DEV")) {
        if (nt > 1) s_dev = (uint8_t)strtoul(tok[1], NULL, 16);
        out("OK dev=%02X\r\n", s_dev);
    }
    else if (!strcmp(c0, "REPEAT") && nt > 1) {
        int r = atoi(tok[1]); s_repeat = r < 1 ? 1 : (r > 250 ? 250 : r); out("OK repeat=%d\r\n", s_repeat);
    }
    else if (!strcmp(c0, "NAME")) {
        /* RDS station name of the tuned station. The amp does not push it: it must be asked for, with the CURRENT
         * bank (read from the tuner status; the board forgets it at every reset) and only while the TUNER input is on. */
        /* This command can run for ~15 s on a station without RDS and the worker executes one command at a time: it gives way as soon as another
         * command is queued (a click on power or volume must not wait for it), and a failure is remembered for 2 minutes per frequency, so the
         * periodic automatic asks of the web page do not keep the worker busy ("NAME FORCE" asks again at once). */
        static unsigned none_f; static int64_t none_us;
        bool force = nt > 1 && !strcasecmp(tok[1], "FORCE");
        if (!force && none_f && none_f == s_tun.f && esp_timer_get_time() - none_us < 120000000LL) {
            out("NAME: no RDS name at %u.%02u MHz (tried a moment ago; NAME FORCE asks again)" CRLF, s_tun.f / 100, s_tun.f % 100); return;
        }
        uint8_t ask[2] = {TUNER_DEV, 0x0F};
        uint32_t s0 = s_tun.seq;
        slink_send(ask, 2, 1);
        for (int i = 0; i < 15 && s_tun.seq == s0; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_tun.seq == s0) { out("NAME: the tuner does not answer - select the TUNER input first (INPUT TUNER)" CRLF); return; }
        if (s_tun.fl & 1) { out("NAME: an AM station is tuned - RDS names exist only on FM" CRLF); return; }
        /* Measured on the STR-DA50ES: a tuner left on a station (tuned by the front panel, a seek or at power-on) answers the name query
         * with "unavailable" (C9 0E) indefinitely, but gives the name right after a direct tune (C1 50) to the same frequency.
         * So after a failed round, re-tune once per frequency and ask again. */
        static unsigned retuned_f;
        for (int attempt = 1; attempt <= 9; attempt++) {     /* RDS can need a few seconds after tuning */
            if (cmd_waiting()) { out("NAME: interrupted by another command" CRLF); return; }
            if (attempt == 4 && retuned_f != s_tun.f) {
                retuned_f = s_tun.f;
                uint8_t re[5] = {TUNER_DEV, 0x50, 0, (uint8_t)(s_tun.f >> 8), (uint8_t)s_tun.f};
                slink_send(re, 5, 1);
                for (int w = 0; w < 15 && !cmd_waiting(); w++) vTaskDelay(pdMS_TO_TICKS(100));
            }
            uint32_t ns = s_tun.name_seq;
            uint8_t nq[4] = {TUNER_DEV, 0x48, s_tun.bank, 0xFF};
            slink_send(nq, 4, 1);
            for (int i = 0; i < 12 && s_tun.name_seq == ns && !cmd_waiting(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            if (s_tun.name_seq == ns && cmd_waiting()) { out("NAME: interrupted by another command" CRLF); return; }
            if (s_tun.name_seq != ns) { out("NAME \"%s\" (%u.%02u MHz)" CRLF, s_tun.name, s_tun.f / 100, s_tun.f % 100); return; }
            for (int i = 0; i < 5 && !cmd_waiting(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
        none_f = s_tun.f; none_us = esp_timer_get_time();
        out("NAME: no RDS name after 9 tries (one re-tune) at %u.%02u MHz (%s%s): weak signal or a station without RDS" CRLF,
            s_tun.f / 100, s_tun.f % 100, (s_tun.fl & 0x08) ? "locked" : "not locked",
            (s_tun.fl & 0x40) ? ", stereo" : "");
    }
    else if (!strcmp(c0, "STATUS")) {
        /* amp / tuner / surround status; replies print decoded as RX lines */
        uint8_t q[3][2] = {{s_dev, 0x0F}, {TUNER_DEV, 0x0F}, {SURR_DEV, 0x0F}};
        for (int i = 0; i < 3; i++) { slink_send(q[i], 2, 1); vTaskDelay(pdMS_TO_TICKS(450)); }
        out("OK status requested" CRLF);
    }
    else if (!strcmp(c0, "FMSCAN")) {
        /* needs the amp on the TUNER input; walks the FM band with the tuner's own search, then re-tunes
         * every stop directly to read its signal level, and finally restores the original frequency */
        uint8_t tune[5] = {TUNER_DEV, 0x50, 0, 0x21, 0x34};      /* below the band: stops at 87.50 */
        uint8_t scan[2] = {TUNER_DEV, 0x06}, ask[2] = {TUNER_DEV, 0x0F};
        uint32_t s0 = s_tun.seq;
        slink_send(ask, 2, 1);
        for (int i = 0; i < 15 && s_tun.seq == s0; i++) vTaskDelay(pdMS_TO_TICKS(100));
        bool had = s_tun.valid && s_tun.seq != s0;
        unsigned orig = s_tun.f; uint8_t orig_fl = s_tun.fl;
        if (!had || (orig_fl & 1)) { out("ERR tuner not answering or on AM (select input TUNER, band FM)" CRLF); return; }
        s_st_n = 0;                                      /* rebuild the station table */
        slink_send(tune, 5, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        unsigned last = s_tun.f, found = 0;
        int64_t t_end = esp_timer_get_time() + 900LL * 1000000;
        out("FMSCAN start (up to 15 min; will restore %u.%02u MHz)" CRLF, orig / 100, orig % 100);
        while (esp_timer_get_time() < t_end) {
            uint32_t seq = s_tun.seq;
            slink_send(scan, 2, 1);
            vTaskDelay(pdMS_TO_TICKS(1200));
            for (int i = 0; i < 80; i++) {
                if (s_tun.seq != seq && !(s_tun.fl & 0x20)) break;
                slink_send(ask, 2, 1);
                vTaskDelay(pdMS_TO_TICKS(700));
            }
            vTaskDelay(pdMS_TO_TICKS(800));
            unsigned f = s_tun.f;
            if (f <= last) break;                        /* wrapped to the band start */
            uint32_t ls = s_tun.lvl_seq;
            uint8_t re[5] = {TUNER_DEV, 0x50, 0, (uint8_t)(f >> 8), (uint8_t)f};
            slink_send(re, 5, 1);                        /* direct tune on the stop: gives signal level */
            for (int i = 0; i < 30 && s_tun.lvl_seq == ls; i++) vTaskDelay(pdMS_TO_TICKS(100));
            bool lv = s_tun.lvl_seq != ls;
            vTaskDelay(pdMS_TO_TICKS(1200));             /* let lock / stereo bits settle */
            slink_send(ask, 2, 1);
            vTaskDelay(pdMS_TO_TICKS(700));
            found++;
            uint32_t ns = s_tun.name_seq;
            uint8_t nq[4] = {TUNER_DEV, 0x48, s_tun.bank, 0xFF};   /* RDS station name of the tuned station */
            slink_send(nq, 4, 1);
            for (int i = 0; i < 20 && s_tun.name_seq == ns; i++) vTaskDelay(pdMS_TO_TICKS(100));
            bool hn = s_tun.name_seq != ns;
            if (hn || (s_tun.fl & 0x08)) st_add(f, hn ? s_tun.name : "");     /* keep named or locked stops */
            out("STATION %u.%02u MHz level %d%s%s%s%s%s%s" CRLF, f / 100, f % 100, lv ? (int)s_tun.level : -1,
                (s_tun.fl & 0x08) ? " locked" : " unlocked", (s_tun.fl & 0x40) ? " stereo" : " no-pilot",
                (s_tun.fl & 0x80) ? " mono-forced" : "", hn ? " name \"" : "", hn ? s_tun.name : "", hn ? "\"" : "");
            last = s_tun.f > f ? s_tun.f : f;
        }
        uint8_t back[5] = {TUNER_DEV, 0x50, 0, (uint8_t)(orig >> 8), (uint8_t)orig};
        slink_send(back, 5, 1);
        vTaskDelay(pdMS_TO_TICKS(1500));
        st_save();
        out("OK FMSCAN done, %u stops, %d stored (STATIONS / TUNE <name>); tuner restored to %u.%02u MHz" CRLF, found, s_st_n, orig / 100, orig % 100);
    }
    else if (!strcmp(c0, "EQCURVE")) {
        /* read the six bands, then draw gain (dB) against frequency (log axis) */
        for (int p = 0; p < 3; p += 2)
            for (int b = 0; b < 3; b++) {
                s_eq[p][b].valid = false;
                uint8_t q[4] = {SURR_DEV, 0x50, (uint8_t)p, (uint8_t)b};
                slink_send(q, 4, 1);
                for (int i = 0; i < 10 && !s_eq[p][b].valid; i++) vTaskDelay(pdMS_TO_TICKS(100));
            }
        for (int p = 0; p < 3; p += 2) {
            if (!(s_eq[p][0].valid && s_eq[p][1].valid && s_eq[p][2].valid)) {
                out("EQ %s: not readable (rear needs the sound field ON)" CRLF, p == 0 ? "FRONT" : "REAR");
                continue;
            }
            draw_eq(p);
        }
    }
    else if (!strcmp(c0, "EQ")) {
        if (nt == 1) {   /* read all front/rear bands */
            for (int p = 0; p < 3; p += 2)
                for (int b = 0; b < 3; b++) {
                    uint8_t q[4] = {SURR_DEV, 0x50, (uint8_t)p, (uint8_t)b};
                    slink_send(q, 4, 1);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            out("OK EQ read (rear needs the sound field ON; band frequencies in docs/en/INPUTS_AND_FRAMES.md)" CRLF);
        } else if (nt >= 5) {
            int p = !strcmp(tok[1], "F") || !strcmp(tok[1], "FRONT") ? 0 : (!strcmp(tok[1], "R") || !strcmp(tok[1], "REAR") ? 2 : -1);
            int b = !strcmp(tok[2], "BASS") ? 0 : (!strcmp(tok[2], "MID") ? 1 : (!strcmp(tok[2], "TREBLE") ? 2 : -1));
            bool gain = !strcmp(tok[3], "GAIN"), freq = !strcmp(tok[3], "FREQ");
            if (p < 0 || b < 0 || (!gain && !freq)) { out("ERR usage: EQ F|R BASS|MID|TREBLE GAIN <-10..10> | FREQ <Hz>" CRLF); return; }
            uint32_t seq = s_eq[p][b].seq;
            uint8_t q[4] = {SURR_DEV, 0x50, (uint8_t)p, (uint8_t)b};
            slink_send(q, 4, 1);
            for (int i = 0; i < 20 && s_eq[p][b].seq == seq; i++) vTaskDelay(pdMS_TO_TICKS(100));
            if (s_eq[p][b].seq == seq) { out("ERR EQ not readable now (rear/center need the sound field ON)" CRLF); return; }
            unsigned fr = s_eq[p][b].f; int g = s_eq[p][b].g;
            if (gain) { g = atoi(tok[4]); if (g < -10) g = -10; if (g > 10) g = 10; }
            else { fr = (unsigned)atoi(tok[4]); if (fr > 20000) fr = 20000; }   /* amp snaps to its nearest step */
            uint8_t sb[9] = {SURR_DEV, 0x90, (uint8_t)p, (uint8_t)b, (uint8_t)(fr >> 8), (uint8_t)fr, (uint8_t)g, 1,
                             (uint8_t)(freq ? 1 : 0)};
            do_send(sb, 9);
        } else out("ERR usage: EQ | EQ F|R BASS|MID|TREBLE GAIN <dB> | FREQ <Hz>" CRLF);
    }
    else if (!strcmp(c0, "SF")) {
        if (nt < 2) { out("ERR usage: SF ON|OFF|<1-29>" CRLF); return; }
        if (!strcmp(tok[1], "ON")) send2(SURR_DEV, 0x04);
        else if (!strcmp(tok[1], "OFF") || !strcmp(tok[1], "0")) send2(SURR_DEV, 0x05);
        else { int n = atoi(tok[1]); if (n < 1 || n > 29) out("ERR sound field 1-29 on the DA50ES" CRLF); else send3(SURR_DEV, 0x51, (uint8_t)n); }
    }
    else if ((!strcmp(c0, "REAR") || !strcmp(c0, "SUB") || !strcmp(c0, "BAL") || !strcmp(c0, "DRC") ||
              !strcmp(c0, "LFE")) && nt > 1) {
        uint8_t code = !strcmp(c0, "REAR") ? 0x80 : !strcmp(c0, "SUB") ? 0x82 : !strcmp(c0, "BAL") ? 0x83
                       : !strcmp(c0, "DRC") ? 0x86 : 0x87;
        int v = atoi(tok[1]);
        send3(SURR_DEV, code, (uint8_t)v);   /* signed dB (DRC: 0 off,1-9 = 0.1-0.9,10 std,255 max) */
    }
    else if (!strcmp(c0, "LINE"))   out("LINE %d" CRLF, slink_line_level());
    else if (!strcmp(c0, "INPUTS")) {
        cmd_inputs();
    }
    else if (!strcmp(c0, "PRESETS")) cmd_presets(nt > 1 ? atoi(tok[1]) : 0);
    else if (!strcmp(c0, "MODEL")) {
        /* C0/C1/C3 22: model info of amp, tuner, surround (the amp sends these at power-on) */
        send2(s_dev, 0x22); vTaskDelay(pdMS_TO_TICKS(300));
        send2(TUNER_DEV, 0x22); vTaskDelay(pdMS_TO_TICKS(300));
        send2(SURR_DEV, 0x22); vTaskDelay(pdMS_TO_TICKS(300));
        send2(s_dev, 0x6A); vTaskDelay(pdMS_TO_TICKS(300));
        send2(s_dev, 0x6B);
    }
    else if (!strcmp(c0, "SETUP")) {
        /* the remote's setup download: FUNCTION NAME (6A, 6B, 48 labels), then TUNER PRESET NAME (4A/48) */
        send2(s_dev, 0x6A); vTaskDelay(pdMS_TO_TICKS(400));
        send2(s_dev, 0x6B); vTaskDelay(pdMS_TO_TICKS(400));
        cmd_inputs();
        cmd_presets(0);
    }
    else if (!strcmp(c0, "DECODE")) {
        if (nt > 1 && !strcmp(tok[1], "ON")) s_decode = true;
        else if (nt > 1 && !strcmp(tok[1], "OFF")) s_decode = false;
        else { out("DECODE %s (usage: DECODE ON|OFF)" CRLF, s_decode ? "ON" : "OFF"); return; }
        out("OK decode %s" CRLF, s_decode ? "on" : "off");
    }
    else if (!strcmp(c0, "EDGES")) {
        uint32_t d[48]; uint8_t l[48];
        int n = slink_dbg_edges(d, l, 48);
        for (int i = 0; i < n; i++) out("%s%lu%s", l[i] ? "H<" : "L<", (unsigned long)d[i], i % 8 == 7 ? CRLF : " ");
        out(CRLF);
    }
    else if (!strcmp(c0, "STATS")) {
        uint32_t e, sy, b, f, br[6];
        slink_branches(br);
        out("BR txskip=%lu idle=%lu gap=%lu nofr=%lu short=%lu" CRLF, (unsigned long)br[0], (unsigned long)br[1],
            (unsigned long)br[2], (unsigned long)br[3], (unsigned long)br[4]);
        slink_stats(&e, &sy, &b, &f);
        out("STATS edges=%lu syncs=%lu bits=%lu frames=%lu gpio=%d line=%d" CRLF,
            (unsigned long)e, (unsigned long)sy, (unsigned long)b, (unsigned long)f,
            slink_get_gpio(), slink_line_level());
    }
    else if (!strcmp(c0, "LOOPBACK") && nt < 2) out("LOOPBACK %s (usage: LOOPBACK ON|OFF)" CRLF, slink_get_loopback() ? "ON" : "OFF");
    else if (!strcmp(c0, "LOOPBACK") && nt > 1) {
        slink_set_loopback(!strcmp(tok[1], "ON"));
        out("OK loopback %s (own frames are shown as RX)" CRLF, !strcmp(tok[1], "ON") ? "on" : "off");
    }
    else if (!strcmp(c0, "ECHO")) {
        if (nt > 1 && !strcmp(tok[1], "ON")) s_echo = true;
        else if (nt > 1 && !strcmp(tok[1], "OFF")) s_echo = false;
        else { out("ERR usage: ECHO ON|OFF (now %s)" CRLF, s_echo ? "ON" : "OFF"); return; }
        out("OK echo %s" CRLF, s_echo ? "on" : "off");
    }
    else if (!strcmp(c0, "SLINK")) {
        if (nt < 2) out("SLINK gpio=%d" CRLF, slink_get_gpio());
        else {
            int rc = slink_set_gpio(atoi(tok[1]));
            if (rc == 0) out("OK slink gpio=%d" CRLF, slink_get_gpio());
            else if (rc == -1) out("ERR invalid gpio (or used by LED)" CRLF);
            else if (rc == -2) out("ERR not available in dual-pin build" CRLF);
            else out("ERR gpio setup failed, kept gpio=%d" CRLF, slink_get_gpio());
        }
    }
    else if (!strcmp(c0, "PAD")) {
        /* electrical mode of the bus pin: PAD [ODPU|OD|PP] (saved) */
        static const char *const nm[3] = {"open-drain, internal pull-up ON", "open-drain, internal pull-up OFF", "push-pull (drives 3.3 V)"};
        if (nt > 1) {
            int m = !strcmp(tok[1], "ODPU") || !strcmp(tok[1], "0") ? 0 : !strcmp(tok[1], "OD") || !strcmp(tok[1], "1") ? 1 : !strcmp(tok[1], "PP") || !strcmp(tok[1], "2") ? 2 : -1;
            if (m < 0 || slink_set_pad(m) != 0) { out("ERR usage: PAD [ODPU|OD|PP]  (0 = open-drain + internal pull-up, 1 = open-drain, 2 = push-pull)" CRLF); return; }
            g_set.pad = (unsigned char)m; settings_save();
            if (m == 2) out("WARNING push-pull drives 3.3 V onto the line: only for a 3.3 V bus, never on a 5 V bus" CRLF);
        }
        out("PAD %d %s" CRLF, slink_get_pad(), nm[slink_get_pad()]);
    }
    else if (!strcmp(c0, "BLINK") && nt > 1) {
        if (!strcmp(tok[1], "OFF")) { slink_test_off(); out("OK blink off" CRLF); }
        else if (nt >= 5 && (!strcmp(tok[1], "HIGH") || !strcmp(tok[1], "LOW")) &&
                 (!strcmp(tok[3], "HIGH") || !strcmp(tok[3], "LOW"))) {
            int n = nt > 5 ? atoi(tok[5]) : 0;
            int rc = slink_blink(!strcmp(tok[1], "HIGH"), atoi(tok[2]),
                                 !strcmp(tok[3], "HIGH"), atoi(tok[4]), n);
            if (rc) out("ERR bad blink params" CRLF);
            else out("OK blink %s %s ms, %s %s ms, %d times (0=until BLINK OFF)" CRLF,
                     tok[1], tok[2], tok[3], tok[4], n);
        } else out("ERR usage: BLINK HIGH|LOW <ms> HIGH|LOW <ms> [count] | BLINK OFF" CRLF);
    }
    else if (!strcmp(c0, "LEVEL") && nt > 1) {
        if (!strcmp(tok[1], "OFF")) { slink_test_off(); out("OK level off" CRLF); }
        else if (!strcmp(tok[1], "HIGH")) { slink_level(1); out("OK line held HIGH (released) - LEVEL OFF to end" CRLF); }
        else if (!strcmp(tok[1], "LOW"))  { slink_level(0); out("OK line held LOW - LEVEL OFF to end" CRLF); }
        else out("ERR usage: LEVEL HIGH|LOW|OFF" CRLF);
    }
    else if (!strcmp(c0, "LED")) {
        if (nt < 2) out("ERR usage: LED <gpio> [INV] | LED OFF" CRLF);
        else if (!strcmp(tok[1], "OFF")) { slink_led_set(-1, false); out("OK led off" CRLF); }
        else {
            int g = atoi(tok[1]); bool inv = nt > 2 && !strcmp(tok[2], "INV");
            if (slink_led_set(g, inv) == 0) out("OK led gpio=%d%s" CRLF, g, inv ? " (active-low)" : "");
            else out("ERR invalid LED gpio" CRLF);
        }
    }
    else if (!strcmp(c0, "MUTE"))   send2(s_dev, 0x06);
    else if (!strcmp(c0, "UNMUTE")) send2(s_dev, 0x07);
    else if (!strcmp(c0, "VOLUP"))  send2(s_dev, 0x14);
    else if (!strcmp(c0, "VOLDN"))  send2(s_dev, 0x15);
    else if (!strcmp(c0, "PWRON"))  { if (send2(s_dev, 0x2E)) state_set_power(1); }
    else if (!strcmp(c0, "PWROFF")) { if (send2(s_dev, 0x2F)) state_set_power(0); }
    else if (!strcmp(c0, "ALLOFF")) {
        /* what the RM-TP501's SYSTEM/STANDBY key does with "ALL OFF" (manual p. 21): power off every Sony unit; units that are
         * not on the bus simply do not answer. CD players 1-3, MD recorder and tuner first, the amplifier last. */
        const uint8_t others[] = {0x90, 0x91, 0x92, 0xB0, (uint8_t)TUNER_DEV};   /* TUNER_DEV follows the default device ID */
        for (size_t i = 0; i < sizeof others; i++) { send2(others[i], 0x2F); vTaskDelay(pdMS_TO_TICKS(120)); }
        if (send2(s_dev, 0x2F)) state_set_power(0);
        out("OK all off (CD 1-3, MD, tuner, amplifier)" CRLF);
    }
    else if (!strcmp(c0, "AMPNAME"))send2(s_dev, 0x6A);
    else if (!strcmp(c0, "VOL") && nt > 1) {
        /* motorised volume knob: steps only; the position below is an estimate (see vol_goto_end) */
        if (!strcmp(tok[1], "POS")) {
            if (nt > 2) { int v = atoi(tok[2]); s_vol_pos = v < 0 ? 0 : (v > VOL_UP_TRAVEL ? VOL_UP_TRAVEL : v); }
            if (s_vol_pos < 0) out("VOL position unknown (use VOL MIN, or VOL POS <steps> if you know it)" CRLF);
            else out("VOL position ~%d/%d up-steps (~%d%%) - estimate" CRLF, s_vol_pos, VOL_UP_TRAVEL, s_vol_pos * 100 / VOL_UP_TRAVEL);
            return;
        }
        if (!strcmp(tok[1], "QMIN")) {
            /* quick calibration: the volume is assumed to be at or below the safety limit (percent, default 70): drive down only as far
             * as needed to reach the minimum from there, then the position is known */
            int lim = nt > 2 ? atoi(tok[2]) : 70;
            if (lim < 1) lim = 1;
            if (lim > 100) lim = 100;
            int need = up_to_down(lim * VOL_UP_TRAVEL / 100) + VOL_MARGIN;
            if (vol_send(s_dev, 0x15, need) != 0) { out("ERR volume send failed" CRLF); return; }
            s_vol_pos = 0;
            out("OK volume MIN quick (%d steps sent, assumes the volume was at most %d%%)" CRLF, need, lim);
            return;
        }
        if (!strcmp(tok[1], "MIN") || !strcmp(tok[1], "MAX")) {
            bool mx = !strcmp(tok[1], "MAX");
            int sent = vol_goto_end(mx);
            if (sent < 0) out("ERR volume send failed" CRLF);
            else out("OK volume %s (%d steps sent)" CRLF, mx ? "MAX" : "MIN", sent);
            return;
        }
        if (!strcmp(tok[1], "SET") && nt > 2) {
            /* percent with decimals: the whole travel is only ~420 steps, so near the minimum (where the pot is steepest)
             * 1 % is already ~4 steps and a fraction of a percent matters: VOL SET 2.4 = 10 steps up from the minimum */
            double pct = atof(tok[2]);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            if (pct <= 0.0 || pct >= 100.0) {
                int sent = vol_goto_end(pct >= 100.0);
                if (sent < 0) out("ERR volume send failed" CRLF);
                else out("OK volume %d%% (%d steps sent)" CRLF, pct >= 100.0 ? 100 : 0, sent);
                return;
            }
            if (s_vol_pos < 0 && vol_goto_end(false) < 0) { out("ERR volume send failed" CRLF); return; }
            int target = (int)(pct * VOL_UP_TRAVEL / 100.0 + 0.5), delta = target - s_vol_pos;
            int steps = delta >= 0 ? delta : up_to_down(-delta);          /* DOWN steps are ~6% shorter */
            if (steps != 0 && vol_send(s_dev, delta > 0 ? 0x14 : 0x15, steps) != 0) {
                out("ERR volume send failed" CRLF); return;
            }
            s_vol_pos = target;
            out("OK volume ~%.1f%% (%d steps %s, estimate)" CRLF, pct, steps, delta >= 0 ? "up" : "down");
            return;
        }
        int n = nt > 2 ? atoi(tok[2]) : 1;
        if (n < 1) n = 1;
        if (n > 250) n = 250;
        uint8_t b[2] = {s_dev, 0};
        if (!strcmp(tok[1], "UP")) b[1] = 0x14;
        else if (!strcmp(tok[1], "DOWN") || !strcmp(tok[1], "DN")) b[1] = 0x15;
        else { out("ERR usage: VOL UP|DOWN [1-250] | MIN | MAX | SET <0-100> | POS [steps]" CRLF); return; }
        int64_t t0 = esp_timer_get_time();
        int rc = slink_send(b, 2, n);
        if (rc == 0) {
            if (s_vol_pos >= 0) {
                s_vol_pos += b[1] == 0x14 ? n : -down_to_up(n);
                if (s_vol_pos < 0) s_vol_pos = 0;
                if (s_vol_pos > VOL_UP_TRAVEL) s_vol_pos = VOL_UP_TRAVEL;
            }
            out("OK %d x volume %s in %ld ms (gap %d ms)" CRLF, n, b[1] == 0x14 ? "UP" : "DOWN",
                (long)((esp_timer_get_time() - t0) / 1000), slink_get_gap_ms());
        } else out("ERR volume send failed (%d)" CRLF, rc);
    }
    else if (!strcmp(c0, "GAP")) {
        if (nt > 1) slink_set_gap_ms(atoi(tok[1]));
        out("GAP %d ms (min time between frame starts; default 30)" CRLF, slink_get_gap_ms());
    }
    else if (!strcmp(c0, "INPUT") && nt > 3 && !strcmp(tok[1], "AV")) {
        /* INPUT AV <audio> <video>: audio and video from different inputs. Measured on the STR-DA50ES: selecting a video input (10-19) sets audio AND video,
         * selecting an audio-only input (TUNER, PHONO, CD, DAT/MD, TAPE: 00-05) changes only the audio and the video stays. So: video first, audio second. */
        int a = input_code(tok[2]), v = input_code(tok[3]);
        if (a < 0 || v < 0) { out("ERR unknown input" CRLF); return; }
        if (a > 0x05 || v < 0x10) { out("ERR INPUT AV <audio-only input: TUNER PHONO CD MD TAPE> <video input: VIDEO1-3 DVD SAT TV>" CRLF); return; }
        send3(s_dev, 0x50, (uint8_t)v);
        vTaskDelay(pdMS_TO_TICKS(700));
        send3(s_dev, 0x50, (uint8_t)a);
    }
    else if (!strcmp(c0, "INPUT") && nt > 1) {
        for (size_t i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++)
            if (!strcmp(tok[1], k_inputs[i].name)) { send3(s_dev, 0x50, k_inputs[i].code); return; }
        char *e; unsigned long v = strtoul(tok[1], &e, 16);
        if (*e == 0 && v <= 0xFF) send3(s_dev, 0x50, (uint8_t)v); else out("ERR unknown input\r\n");
    }
    else if (!strcmp(c0, "TUNER") && nt > 1) {
        const char *a = tok[1];
        /* this tuner rejects the band command (C1 03 -> C9 0F): the band is changed by tuning directly in it */
        if (!strcmp(a, "FM") || !strcmp(a, "AM")) {
            bool am = a[0] == 'A';
            unsigned f = am ? s_last_am : s_last_fm;
            uint8_t b[5] = {TUNER_DEV, 0x50, am ? 1 : 0, (uint8_t)(f >> 8), (uint8_t)f};
            do_send(b, 5);
        }
        else if (!strcmp(a, "SCAN+"))   send2(TUNER_DEV, 0x06);
        else if (!strcmp(a, "SCAN-"))   send2(TUNER_DEV, 0x07);
        else if (!strcmp(a, "PRESET+")) send2(TUNER_DEV, 0x08);
        else if (!strcmp(a, "PRESET-")) send2(TUNER_DEV, 0x09);
        else if (!strcmp(a, "MONO"))    send2(TUNER_DEV, 0x0A);
        else if (!strcmp(a, "STEREO"))  send2(TUNER_DEV, 0x0B);
        else out("ERR unknown tuner cmd\r\n");
    }
    else if (!strcmp(c0, "STATION") && nt > 1) {
        /* STATION CLEAR | STATION ADD <10 kHz units> ["name"] | STATION SAVE: rebuild the FM station table (configuration restore) */
        if (!strcmp(tok[1], "CLEAR")) { s_st_n = 0; out("OK stations cleared (STATION ADD ..., then STATION SAVE)" CRLF); }
        else if (!strcmp(tok[1], "ADD")) {
            char *av[6]; int na = split_args(raw, av, 6);
            unsigned f = na > 2 ? (unsigned)atoi(av[2]) : 0;
            if (f < 8750 || f > 10800) { out("ERR usage: STATION ADD <frequency in 10 kHz units, 8750..10800> [name]" CRLF); return; }
            if (s_st_n >= ST_MAX) { out("ERR station table full" CRLF); return; }
            st_add(f, na > 3 ? av[3] : "");
            out("OK station %d" CRLF, s_st_n);
        } else if (!strcmp(tok[1], "SAVE")) { st_save(); out("OK %d stations saved" CRLF, s_st_n); }
        else out("ERR usage: STATION CLEAR | ADD <f> [name] | SAVE" CRLF);
    }
    else if (!strcmp(c0, "STATIONS")) {
        if (!s_st_n) { out("No stations stored: run FMSCAN (amp on TUNER input)" CRLF); return; }
        for (int i = 0; i < s_st_n; i++)
            out("%2d  %3u.%02u MHz  %s" CRLF, i + 1, s_st[i].f / 100, s_st[i].f % 100, s_st[i].name[0] ? s_st[i].name : "(no name)");
        out("OK %d stations (TUNE <name or number>, TUNE FM <MHz>)" CRLF, s_st_n);
    }
    else if (!strcmp(c0, "TUNE") && nt >= 2 && strcmp(tok[1], "FM") && strcmp(tok[1], "AM")) {
        /* TUNE 101.5 (MHz) | TUNE #n (from STATIONS) | TUNE <name part> (from the FMSCAN table) */
        const char *arg = line;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        unsigned fr = 0;
        if (isdigit((unsigned char)tok[1][0]) && strchr(tok[1], '.')) fr = (unsigned)(atof(tok[1]) * 100.0 + 0.5);
        else if (tok[1][0] == '#' && atoi(tok[1] + 1) >= 1 && atoi(tok[1] + 1) <= s_st_n) fr = s_st[atoi(tok[1] + 1) - 1].f;
        else {
            int first, m = st_find(arg, &first);
            if (m == 0) { out("ERR no station named '%s' (STATIONS lists them; run FMSCAN to build the table)" CRLF, arg); return; }
            fr = s_st[first].f;
            if (m > 1) {
                out("NOTE %d stations match:", m);
                for (int i = 0; i < s_st_n; i++) {
                    char nm[10], q[10];
                    int k = 0;
                    for (const char *s2 = arg; *s2 && k < 9; s2++) q[k++] = (char)toupper((unsigned char)*s2);
                    q[k] = 0;
                    while (k && q[k - 1] == ' ') q[--k] = 0;
                    for (int j = 0; j < 9; j++) nm[j] = (char)toupper((unsigned char)s_st[i].name[j]);
                    if (strstr(nm, q)) out(" %u.%02u", s_st[i].f / 100, s_st[i].f % 100);
                }
                out(" -> tuning the first (use TUNE FM <MHz> for another)" CRLF);
            }
        }
        if (fr < 8750 || fr > 10800) { out("ERR frequency out of the FM band" CRLF); return; }
        uint8_t b[5] = {TUNER_DEV, 0x50, 0, (uint8_t)(fr >> 8), (uint8_t)fr};
        do_send(b, 5);
    }
    else if (!strcmp(c0, "TUNE") && nt > 2) {
        uint8_t b[5] = {TUNER_DEV, 0x50, 0, 0, 0};
        unsigned v;
        if (!strcmp(tok[1], "FM")) { v = (unsigned)(atof(tok[2]) * 100.0 + 0.5); b[2] = 0; }
        else if (!strcmp(tok[1], "AM")) { v = (unsigned)atoi(tok[2]); b[2] = 1; }
        else { out("ERR TUNE FM|AM\r\n"); return; }
        if (b[2]) s_last_am = v; else s_last_fm = v;
        b[3] = (uint8_t)(v >> 8); b[4] = (uint8_t)v;
        do_send(b, 5);
    }
    else if (!strcmp(c0, "FAV")) {
        /* FAV | FAV ADD [FM <MHz> | AM <kHz>] | FAV DEL <n> | FAV <n> (tune to preset n) */
        if (nt < 2) {
            if (!s_fav_n) out("No presets stored: tune a station, then FAV ADD" CRLF);
            for (int i = 0; i < s_fav_n; i++) {
                if (s_fav[i].am) out("%2d  AM %4u kHz  %s" CRLF, i + 1, s_fav[i].f, s_fav[i].name);
                else out("%2d  FM %3u.%02u MHz  %s" CRLF, i + 1, s_fav[i].f / 100, s_fav[i].f % 100, s_fav[i].name);
            }
            out("OK fav %d stored (FAV ADD | FAV DEL <n> | FAV <n>)" CRLF, s_fav_n);
        } else if (!strcmp(tok[1], "ADD")) {
            unsigned f = 0; bool am = false; char nm[9] = {0};
            if (nt > 3 && (!strcmp(tok[2], "FM") || !strcmp(tok[2], "AM"))) {
                am = tok[2][0] == 'A';
                f = am ? (unsigned)atoi(tok[3]) : (unsigned)(atof(tok[3]) * 100.0 + 0.5);
                if (!am) for (int i = 0; i < s_st_n; i++) if (s_st[i].f == f) { strncpy(nm, s_st[i].name, 8); break; }   /* name from the scan table */
            } else if (!state_tuner(&f, &am, nm, sizeof nm)) { out("ERR no station tuned (select the TUNER input first)" CRLF); return; }
            if (f == 0 || (!am && (f < 8750 || f > 10800)) || (am && (f < 531 || f > 1602))) { out("ERR frequency out of range" CRLF); return; }
            for (int i = 0; i < s_fav_n; i++)
                if (s_fav[i].f == f && s_fav[i].am == am) { out("OK fav already %d" CRLF, i + 1); return; }
            if (s_fav_n >= FAV_MAX) { out("ERR presets full (max %d): FAV DEL <n>" CRLF, FAV_MAX); return; }
            s_fav[s_fav_n].f = (uint16_t)f; s_fav[s_fav_n].am = am;
            const char *p = nm; while (*p == ' ') p++;
            strncpy(s_fav[s_fav_n].name, p, 8); s_fav[s_fav_n].name[8] = 0;
            s_fav_n++;
            fav_save();
            out("OK fav added %d" CRLF, s_fav_n);
        } else if (!strcmp(tok[1], "CLEAR")) {
            s_fav_n = 0; fav_save();
            out("OK fav cleared" CRLF);
        } else if (!strcmp(tok[1], "PUT")) {
            /* FAV PUT FM|AM <10 kHz units | kHz> ["name"]: append a preset with an explicit name (configuration restore) */
            char *av[6]; int na = split_args(raw, av, 6);
            bool am = na > 2 && !strcasecmp(av[2], "AM");
            unsigned f = na > 3 ? (unsigned)atoi(av[3]) : 0;
            if (na < 4 || f == 0 || (!am && (f < 8750 || f > 10800)) || (am && (f < 531 || f > 1602))) { out("ERR usage: FAV PUT FM|AM <frequency in 10 kHz | kHz> [name]" CRLF); return; }
            if (s_fav_n >= FAV_MAX) { out("ERR presets full (max %d)" CRLF, FAV_MAX); return; }
            s_fav[s_fav_n].f = (uint16_t)f; s_fav[s_fav_n].am = am;
            memset(s_fav[s_fav_n].name, 0, sizeof s_fav[s_fav_n].name);
            if (na > 4) strncpy(s_fav[s_fav_n].name, av[4], 8);
            s_fav_n++; fav_save();
            out("OK fav put %d" CRLF, s_fav_n);
        } else if (!strcmp(tok[1], "DEL") && nt > 2) {
            int n = atoi(tok[2]);
            if (n < 1 || n > s_fav_n) { out("ERR no preset %d" CRLF, n); return; }
            for (int i = n - 1; i < s_fav_n - 1; i++) s_fav[i] = s_fav[i + 1];
            s_fav_n--;
            fav_save();
            out("OK fav removed %d" CRLF, n);
        } else if (isdigit((unsigned char)tok[1][0])) {
            int n = atoi(tok[1]);
            if (n < 1 || n > s_fav_n) { out("ERR no preset %d" CRLF, n); return; }
            fav_t *v = &s_fav[n - 1];
            if (v->am) s_last_am = v->f; else s_last_fm = v->f;
            uint8_t b[5] = {TUNER_DEV, 0x50, v->am, (uint8_t)(v->f >> 8), (uint8_t)v->f};
            do_send(b, 5);
        } else out("ERR usage: FAV | FAV ADD [FM <MHz>|AM <kHz>] | FAV DEL <n> | FAV <n>" CRLF);
    }
    else if (!strcmp(c0, "OTA")) {
#if SOC_WIFI_SUPPORTED
        char *av[6]; int na = split_args(raw, av, 6);
        ota_cli(av, na);
#else
        out("ERR not available: this chip (ESP32-H2) has no WiFi; update the firmware over USB" CRLF);
#endif
    }
    else if (!strcmp(c0, "PRESET") && nt > 2) {
        uint8_t b[4] = {TUNER_DEV, 0x51, (uint8_t)atoi(tok[1]), (uint8_t)atoi(tok[2])};
        do_send(b, 4);
    }
    else if (!strcmp(c0, "BLE") || !strcmp(c0, "WEB") || !strcmp(c0, "APP") || !strcmp(c0, "WIFI") ||
             !strcmp(c0, "MQTT") || !strcmp(c0, "HOSTNAME") || !strcmp(c0, "NET") || !strcmp(c0, "ECO") ||
             !strcmp(c0, "VOLCFG") || !strcmp(c0, "UITITLE") || !strcmp(c0, "CFG") || !strcmp(c0, "WIFIPWR")) handle_net(raw, tok, nt);
    else if (!parse_hex_line(line)) out("ERR unknown command (try HELP)\r\n");
}

/* ---- network / interface settings (persistent) ---- */

/* split a line into arguments, honouring "double quotes" (SSIDs and passwords are case-sensitive and may contain blanks) */
static int split_args(char *raw, char **av, int max)
{
    int n = 0;
    char *p = raw;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') { av[n++] = ++p; while (*p && *p != '"') p++; if (*p) *p++ = 0; }
        else { av[n++] = p; while (*p && *p != ' ' && *p != '\t') p++; if (*p) *p++ = 0; }
    }
    return n;
}

static bool parse_onoff(const char *t, bool *v)
{
    if (!strcmp(t, "ON") || !strcmp(t, "1")) { *v = true; return true; }
    if (!strcmp(t, "OFF") || !strcmp(t, "0")) { *v = false; return true; }
    return false;
}

static void handle_net(char *raw, char **tok, int nt)
{
    const char *c0 = tok[0];
#if !SOC_WIFI_SUPPORTED
    if (!strcmp(c0, "WIFI") || !strcmp(c0, "WEB") || !strcmp(c0, "APP") || !strcmp(c0, "MQTT") || !strcmp(c0, "HOSTNAME") || !strcmp(c0, "WIFIPWR")) {
        out("ERR not available: this chip (ESP32-H2) has no WiFi; use the USB serial port or BLE" CRLF);
        return;
    }
#endif
    char *av[6];
    int na = split_args(raw, av, 6);
    char st[600];
    bool v;
    if (!strcmp(c0, "BLE") || !strcmp(c0, "WEB") || !strcmp(c0, "APP") || !strcmp(c0, "ECO")) {
        bool *flag = !strcmp(c0, "BLE") ? &g_set.ble : !strcmp(c0, "WEB") ? &g_set.web : !strcmp(c0, "ECO") ? &g_set.eco : &g_set.app;
        const char *what = !strcmp(c0, "BLE") ? "UART over BLE (Nordic UART Service)"
                         : !strcmp(c0, "WEB") ? "web portal (http://<host>.local/)"
                         : !strcmp(c0, "ECO") ? "low-power profile (WiFi max modem sleep, CPU 40-80 MHz, 11 dBm TX; commands may lag ~0.3 s)"
                         : "WiFi application interface (raw TCP console + MQTT/Home Assistant)";
        if (nt < 2) { out("%s %s - %s. Usage: %s ON|OFF" CRLF, c0, *flag ? "ON" : "OFF", what, c0); return; }
        if (!parse_onoff(tok[1], &v)) { out("ERR usage: %s ON|OFF" CRLF, c0); return; }
        *flag = v;
        settings_save();
        net_apply();
        out("OK %s %s (saved, persistent)" CRLF, c0, v ? "ON" : "OFF");
        if (v && flag != &g_set.ble && flag != &g_set.eco && !g_set.ssid[0]) out("NOTE no WiFi network set: WIFI <ssid> <password>" CRLF);
    }
    else if (!strcmp(c0, "WIFI")) {
        if (na >= 2 && !strcasecmp(av[1], "CLEAR")) {
            g_set.ssid[0] = 0; g_set.pass[0] = 0; settings_save(); net_apply();
            out("OK wifi credentials cleared" CRLF);
        } else if (na >= 2) {
            if (strlen(av[1]) > 32 || (na > 2 && (strlen(av[2]) > 63 || (strlen(av[2]) && strlen(av[2]) < 8)))) {
                out("ERR SSID max 32 chars, password 8-63 chars (use \"quotes\" for blanks)" CRLF); return;
            }
            strncpy(g_set.ssid, av[1], sizeof g_set.ssid - 1); g_set.ssid[sizeof g_set.ssid - 1] = 0;
            strncpy(g_set.pass, na > 2 ? av[2] : "", sizeof g_set.pass - 1); g_set.pass[sizeof g_set.pass - 1] = 0;
            settings_save(); net_apply();
            out("OK wifi \"%s\" saved%s" CRLF, g_set.ssid,
                (g_set.web || g_set.app) ? " (connecting...)" : " (enable it with WEB ON or APP ON)");
        } else {
            net_status(st, sizeof st);
            out("%s" CRLF "Usage: WIFI <ssid> [password] | WIFI CLEAR  (quote values with blanks)" CRLF, st);
        }
    }
    else if (!strcmp(c0, "MQTT")) {
        if (na >= 2 && !strcasecmp(av[1], "CLEAR")) {
            g_set.mqtt_uri[0] = g_set.mqtt_user[0] = g_set.mqtt_pass[0] = 0; settings_save(); net_apply();
            out("OK mqtt broker cleared" CRLF);
        } else if (na >= 2) {
            if (strncmp(av[1], "mqtt://", 7)) { out("ERR URI must start with mqtt:// (plain TCP; TLS is not supported)" CRLF); return; }
            if (strlen(av[1]) >= sizeof g_set.mqtt_uri || (na > 2 && strlen(av[2]) >= sizeof g_set.mqtt_user) ||
                (na > 3 && strlen(av[3]) >= sizeof g_set.mqtt_pass)) { out("ERR value too long" CRLF); return; }
            strcpy(g_set.mqtt_uri, av[1]);
            strcpy(g_set.mqtt_user, na > 2 ? av[2] : "");
            strcpy(g_set.mqtt_pass, na > 3 ? av[3] : "");
            settings_save(); net_apply();
            out("OK mqtt broker %s saved (active when APP is ON)" CRLF, g_set.mqtt_uri);
        } else {
            out("MQTT broker: %s%s%s" CRLF "Usage: MQTT <mqtt://host[:port]> [user] [password] | MQTT CLEAR" CRLF,
                g_set.mqtt_uri[0] ? g_set.mqtt_uri : "(not set)", g_set.mqtt_user[0] ? " user " : "", g_set.mqtt_user);
        }
    }
    else if (!strcmp(c0, "CFG")) {
        /* the settings kept in the ESP32 flash, for the web "backup": CFG [PASS] lists them (passwords only with PASS),
         * CFG SET <key> <value> changes one (saved), CFG APPLY applies them (WiFi / BLE / web / app) */
        if (na >= 2 && !strcasecmp(av[1], "SET")) {
            if (na < 4) { out("ERR usage: CFG SET <key> <value>" CRLF); return; }
            const char *k = av[2], *vv = av[3]; long n = atol(vv);
            if (!strcmp(k, "ble")) g_set.ble = n != 0;
            else if (!strcmp(k, "web")) g_set.web = n != 0;
            else if (!strcmp(k, "app")) g_set.app = n != 0;
            else if (!strcmp(k, "eco")) g_set.eco = n != 0;
            else if (!strcmp(k, "pad") && n >= 0 && n <= 2) g_set.pad = (unsigned char)n;
            else if (!strcmp(k, "txp") && (n == 0 || (n >= 2 && n <= 20))) g_set.txp = (unsigned char)n;
            else if (!strcmp(k, "wps") && n >= 0 && n <= 3) g_set.wps = (unsigned char)n;
            else if (!strcmp(k, "lis") && n >= 0 && n <= 10) g_set.lis = (unsigned char)n;
            else if (!strcmp(k, "vol_up") && n >= 50 && n <= 2000) g_set.vol_up = (unsigned short)n;
            else if (!strcmp(k, "vol_down") && n >= 50 && n <= 2000) g_set.vol_down = (unsigned short)n;
            else if (!strcmp(k, "vol_margin") && n >= 0 && n <= 200) g_set.vol_margin = (unsigned short)n;
            else if (!strcmp(k, "ssid") && strlen(vv) < sizeof g_set.ssid) strcpy(g_set.ssid, vv);
            else if (!strcmp(k, "pass") && strlen(vv) < sizeof g_set.pass) strcpy(g_set.pass, vv);
            else if (!strcmp(k, "host") && vv[0] && strlen(vv) < sizeof g_set.host) strcpy(g_set.host, vv);
            else if (!strcmp(k, "mqtt_uri") && strlen(vv) < sizeof g_set.mqtt_uri) strcpy(g_set.mqtt_uri, vv);
            else if (!strcmp(k, "mqtt_user") && strlen(vv) < sizeof g_set.mqtt_user) strcpy(g_set.mqtt_user, vv);
            else if (!strcmp(k, "mqtt_pass") && strlen(vv) < sizeof g_set.mqtt_pass) strcpy(g_set.mqtt_pass, vv);
            else if (!strcmp(k, "title_app") && strlen(vv) < sizeof g_set.title_app) strcpy(g_set.title_app, vv);
            else if (!strcmp(k, "title_page") && strlen(vv) < sizeof g_set.title_page) strcpy(g_set.title_page, vv);
            else if (!strcmp(k, "ota_push")) g_set.ota_push = n != 0;
            else if (!strcmp(k, "ota_pull")) g_set.ota_pull = n != 0;
            else if (!strcmp(k, "ota_url") && strlen(vv) < sizeof g_set.ota_url) strcpy(g_set.ota_url, vv);
            else if (!strcmp(k, "ota_token") && strlen(vv) < sizeof g_set.ota_token) strcpy(g_set.ota_token, vv);
            else if (!strcmp(k, "ota_pass") && strlen(vv) < sizeof g_set.ota_pass) strcpy(g_set.ota_pass, vv);
            else { out("ERR cfg: unknown key or invalid value (%s)" CRLF, k); return; }
            settings_save();
            out("OK cfg %s" CRLF, k);
        } else if (na >= 2 && !strcasecmp(av[1], "APPLY")) {
            slink_set_pad(g_set.pad);
            net_apply();
            out("OK cfg applied" CRLF);
        } else {
            bool pw = na >= 2 && !strcasecmp(av[1], "PASS");
            out("CFG ble %d" CRLF "CFG web %d" CRLF "CFG app %d" CRLF "CFG eco %d" CRLF "CFG pad %d" CRLF, g_set.ble, g_set.web, g_set.app, g_set.eco, (int)g_set.pad);
            out("CFG txp %d" CRLF "CFG wps %d" CRLF "CFG lis %d" CRLF, (int)g_set.txp, (int)g_set.wps, (int)g_set.lis);
            out("CFG vol_up %d" CRLF "CFG vol_down %d" CRLF "CFG vol_margin %d" CRLF, (int)g_set.vol_up, (int)g_set.vol_down, (int)g_set.vol_margin);
            out("CFG host \"%s\"" CRLF "CFG ssid \"%s\"" CRLF "CFG mqtt_uri \"%s\"" CRLF "CFG mqtt_user \"%s\"" CRLF, g_set.host, g_set.ssid, g_set.mqtt_uri, g_set.mqtt_user);
            out("CFG title_app \"%s\"" CRLF "CFG title_page \"%s\"" CRLF, g_set.title_app, g_set.title_page);
            out("CFG ota_url \"%s\"" CRLF "CFG ota_push %d" CRLF "CFG ota_pull %d" CRLF, g_set.ota_url, g_set.ota_push, g_set.ota_pull);
            if (pw) out("CFG pass \"%s\"" CRLF "CFG mqtt_pass \"%s\"" CRLF "CFG ota_pass \"%s\"" CRLF "CFG ota_token \"%s\"" CRLF, g_set.pass, g_set.mqtt_pass, g_set.ota_pass, g_set.ota_token);
            out("OK cfg end%s" CRLF, pw ? " (with passwords)" : "");
        }
    }
    else if (!strcmp(c0, "VOLCFG")) {
        /* knob travel of the motorised volume: VOLCFG [up down margin] (steps; saved) */
        if (na >= 4) {
            int u = atoi(av[1]), d = atoi(av[2]), m = atoi(av[3]);
            if (u < 50 || u > 2000 || d < 50 || d > 2000 || m < 0 || m > 200) { out("ERR usage: VOLCFG <up 50-2000> <down 50-2000> <margin 0-200>" CRLF); return; }
            g_set.vol_up = (unsigned short)u; g_set.vol_down = (unsigned short)d; g_set.vol_margin = (unsigned short)m;
            settings_save();
        }
        out("VOLCFG up=%d down=%d margin=%d" CRLF, (int)g_set.vol_up, (int)g_set.vol_down, (int)g_set.vol_margin);
    }
    else if (!strcmp(c0, "WIFIPWR")) {
        /* WiFi power: WIFIPWR [<tx dBm 2-20|AUTO> <ps AUTO|NONE|MIN|MAX> <listen 1-10|AUTO>] (saved, applied at once) */
        if (na >= 4) {
            int tx = !strcasecmp(av[1], "AUTO") ? 0 : atoi(av[1]);
            int ps = !strcasecmp(av[2], "AUTO") ? 0 : !strcasecmp(av[2], "NONE") ? 1 : !strcasecmp(av[2], "MIN") ? 2 : !strcasecmp(av[2], "MAX") ? 3 : -1;
            int li = !strcasecmp(av[3], "AUTO") ? 0 : atoi(av[3]);
            if ((tx != 0 && (tx < 2 || tx > 20)) || ps < 0 || li < 0 || li > 10) { out("ERR usage: WIFIPWR <tx 2-20|AUTO> <ps AUTO|NONE|MIN|MAX> <listen 1-10|AUTO>" CRLF); return; }
            g_set.txp = (unsigned char)tx; g_set.wps = (unsigned char)ps; g_set.lis = (unsigned char)li;
            settings_save(); net_apply();
        }
        static const char *const psn[] = {"AUTO", "NONE", "MIN", "MAX"};
        char tb[8], lb[8];
        if (g_set.txp) snprintf(tb, sizeof tb, "%d", (int)g_set.txp); else strcpy(tb, "AUTO");
        if (g_set.lis) snprintf(lb, sizeof lb, "%d", (int)g_set.lis); else strcpy(lb, "AUTO");
        out("WIFIPWR tx=%s ps=%s listen=%s" CRLF, tb, psn[g_set.wps & 3], lb);
    }
    else if (!strcmp(c0, "UITITLE")) {
        /* titles of the web interface: UITITLE [APP|PAGE "text"] (empty text = default) */
        if (na >= 2) {
            char *dst = !strcasecmp(av[1], "APP") ? g_set.title_app : !strcasecmp(av[1], "PAGE") ? g_set.title_page : NULL;
            if (!dst) { out("ERR usage: UITITLE [APP|PAGE \"text\"]" CRLF); return; }
            const char *t = na > 2 ? av[2] : "";
            if (strlen(t) > 32) { out("ERR title too long (max 32 bytes)" CRLF); return; }
            size_t k = 0;
            for (; t[k]; k++) dst[k] = (t[k] == '"' || (unsigned char)t[k] < 32) ? '\'' : t[k];
            dst[k] = 0;
            settings_save();
        }
        out("UITITLE APP \"%s\"" CRLF "UITITLE PAGE \"%s\"" CRLF, g_set.title_app, g_set.title_page);
    }
    else if (!strcmp(c0, "HOSTNAME")) {
        if (na < 2) { out("HOSTNAME %s (reachable as %s.local)" CRLF, g_set.host, g_set.host); return; }
        size_t l = strlen(av[1]);
        bool ok = l >= 1 && l <= 30;
        for (size_t i = 0; ok && i < l; i++) ok = isalnum((unsigned char)av[1][i]) || av[1][i] == '-';
        if (!ok) { out("ERR hostname: 1-30 letters, digits or dash" CRLF); return; }
        for (size_t i = 0; i <= l; i++) g_set.host[i] = (char)tolower((unsigned char)av[1][i]);
        settings_save(); net_apply();
        out("OK hostname %s (saved)" CRLF, g_set.host);
    }
    else {   /* NET */
        net_status(st, sizeof st);
        out("%s" CRLF, st);
    }
}

/* ---- command queue: every transport submits lines, one worker executes them in order ---- */
static QueueHandle_t s_cmdq;
static bool cmd_waiting(void) { return s_cmdq && uxQueueMessagesWaiting(s_cmdq) > 0; }

bool cli_submit(const char *line)
{
    if (!s_cmdq || !line) return false;
    size_t n = strlen(line);
    if (n == 0) return true;
    if (n >= CLI_LINE_MAX) { out("ERR line too long" CRLF); return false; }
    char buf[CLI_LINE_MAX];
    memcpy(buf, line, n + 1);
    /* Several clients (web pages, TCP, BLE) refresh the same state: an identical idempotent query that was submitted a moment ago
     * is dropped instead of queued behind the long commands (STATUS 1.4 s, INPUTS 4.5 s, EQ 3 s) and overflowing the queue. */
    static char last[4][CLI_LINE_MAX]; static int64_t last_us[4]; static int li;
    static const char *const idem[] = {"STATUS", "INPUTS", "AMPNAME", "MODEL", "SETUP", "PRESETS", "EQ", "LINE", "NET", NULL};
    char u[CLI_LINE_MAX];
    size_t k = 0; for (const char *p = line; *p && k < sizeof u - 1; p++) u[k++] = (char)toupper((unsigned char)*p);
    while (k && u[k - 1] == ' ') k--;
    u[k] = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; idem[i]; i++) if (!strcmp(u, idem[i])) {
        for (int j = 0; j < 4; j++) if (!strcmp(last[j], u) && now - last_us[j] < 1500000) return true;     /* duplicate: already on its way */
        strcpy(last[li], u); last_us[li] = now; li = (li + 1) & 3;
        break;
    }
    if (xQueueSend(s_cmdq, buf, pdMS_TO_TICKS(80)) != pdTRUE) { out("ERR busy (a long command is running)" CRLF); return false; }
    return true;
}

static void worker_task(void *arg)
{
    char line[CLI_LINE_MAX];
    for (;;)
        if (xQueueReceive(s_cmdq, line, portMAX_DELAY) == pdTRUE) handle(line);
}

int cli_vol_x10(void) { return s_vol_pos < 0 ? -1 : (s_vol_pos * 1000 + VOL_UP_TRAVEL / 2) / VOL_UP_TRAVEL; }

int cli_vol_pct(void) { return s_vol_pos < 0 ? -1 : (s_vol_pos * 100 + VOL_UP_TRAVEL / 2) / VOL_UP_TRAVEL; }

size_t cli_stations_json(char *b, size_t sz)
{
    size_t n = (size_t)snprintf(b, sz, "{\"t\":\"stations\",\"stations\":[");
    for (int i = 0; i < s_st_n && n < sz; i++) {
        char nm[9];
        int k = 0;
        for (int j = 0; j < 8 && s_st[i].name[j]; j++) {
            char c = s_st[i].name[j];
            nm[k++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '.';
        }
        nm[k] = 0;
        n += (size_t)snprintf(b + n, sz - n, "%s{\"f\":%u,\"name\":\"%s\"}", i ? "," : "", s_st[i].f, nm);
    }
    if (n < sz) n += (size_t)snprintf(b + n, sz - n, "]}");
    return n < sz ? n : sz - 1;
}

size_t cli_favs_json(char *b, size_t sz)
{
    size_t n = (size_t)snprintf(b, sz, "{\"t\":\"favs\",\"favs\":[");
    for (int i = 0; i < s_fav_n && n < sz; i++) {
        char nm[9];
        int k = 0;
        for (int j = 0; j < 8 && s_fav[i].name[j]; j++) {
            char c = s_fav[i].name[j];
            nm[k++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '.';
        }
        nm[k] = 0;
        n += (size_t)snprintf(b + n, sz - n, "%s{\"f\":%u,\"am\":%d,\"name\":\"%s\"}", i ? "," : "", s_fav[i].f, s_fav[i].am, nm);
    }
    if (n < sz) n += (size_t)snprintf(b + n, sz - n, "]}");
    return n < sz ? n : sz - 1;
}

static void cli_task(void *arg)
{
    char line[CLI_LINE_MAX];
    size_t n = 0;
    out("S-Link bridge ready. Type HELP.\r\n");
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            if (s_echo && ch == '\r') out("\r\n");
            if (n) { line[n] = 0; cli_submit(line); n = 0; }
        } else if (ch == 0x08 || ch == 0x7F) {          /* backspace */
            if (n) { n--; if (s_echo) out("\b \b"); }
        } else if (n < sizeof line - 1) {
            line[n++] = (char)ch;
            if (s_echo) out("%c", ch);
        }
    }
}

void cli_start(void)
{
    s_out_mtx = xSemaphoreCreateMutex();
    s_cmdq = xQueueCreate(24, CLI_LINE_MAX);
    esp_err_t ne = nvs_flash_init();
    if (ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    st_load();
    fav_load();
    settings_load();
    state_init();
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 1024;
    usb_serial_jtag_driver_install(&cfg);
    xTaskCreate(worker_task, "slink_cmd", 7168, NULL, 5, NULL);
    xTaskCreate(cli_task, "slink_cli", 3072, NULL, 5, NULL);
}

void cli_start_rx(void)
{
    xTaskCreate(rx_task, "slink_out", 5120, NULL, 5, NULL);
}

void cli_report_error(const char *msg) { out("%s\r\n", msg); }
