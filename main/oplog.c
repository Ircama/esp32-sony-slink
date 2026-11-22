/* Functional log of the operations and counters of the time spent in each state (oplog.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "oplog.h"
#include "state.h"
#include "settings.h"
#include "cli.h"

typedef struct { uint32_t up; uint8_t ts, a; uint16_t v; } ev_t;            /* 8 bytes; ts = origin << 4 | type */
static volatile uint8_t s_org;                                                /* who gave the last command that changes the amplifier (a CLI_SRC_*) ... */
static volatile int64_t s_org_us;                                             /* ... and when */
void oplog_origin(uint8_t src) { s_org = src; s_org_us = esp_timer_get_time(); }
static uint8_t auto_src(uint8_t type)                                         /* a change seen within 4 s of one of our commands is attributed to it, any other comes from the bus: the remote control or the amplifier itself */
{
    if (type < OPL_POWER || type > OPL_TUNER) return 10;                      /* system events: the device */
    return esp_timer_get_time() - s_org_us < 4000000 && s_org_us ? s_org : 0;
}

static ev_t *s_ev;
static unsigned s_cap, s_head, s_len;
static bool s_psram;
static SemaphoreHandle_t s_mtx;
static struct { uint32_t magic, since, non; uint32_t sec[OPL_SLOTS]; } s_cnt;
static bool s_dirty;
static int s_pw = -1;                       /* power of the amplifier, and since when (uptime seconds): 0 = not known, only since the start of the bridge */
static uint32_t s_psince;
static bool s_pexact;
#define CNT_MAGIC 0x4F50434E

unsigned oplog_max_depth(void) { return s_psram ? 20000 : 300; }
unsigned oplog_len(void) { return s_len; }

static unsigned cat_of(uint8_t t)
{
    switch (t) {
    case OPL_POWER: return OPC_POWER;
    case OPL_INPUT: return OPC_INPUT;
    case OPL_MUTE: return OPC_MUTE;
    case OPL_VOLUME: return OPC_VOLUME;
    case OPL_SF: return OPC_SF;
    case OPL_TUNER: return OPC_TUNER;
    default: return OPC_SYSTEM;
    }
}

static int slot_of(int c)
{
    switch (c) {
    case 0x00: return 0; case 0x01: return 1; case 0x02: return 2; case 0x03: return 3; case 0x04: return 4; case 0x05: return 5; case 0x07: return 6;
    case 0x10: return 7; case 0x11: return 8; case 0x12: return 9; case 0x13: return 10;
    case 0x15: case 0x19: return 11; case 0x16: return 12; case 0x17: return 13;
    default: return 14;
    }
}

static ev_t *alloc_ring(unsigned n)
{
    ev_t *p = NULL;
    if (s_psram) p = heap_caps_calloc(n, sizeof(ev_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, sizeof(ev_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

static void apply_depth(unsigned n)                 /* a new ring of n events, keeping the newest ones */
{
    if (n > oplog_max_depth()) n = oplog_max_depth();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ev_t *nw = n ? alloc_ring(n) : NULL;
    unsigned keep = 0;
    if (n && nw && s_ev) {
        keep = s_len < n ? s_len : n;
        for (unsigned i = 0; i < keep; i++) nw[i] = s_ev[(s_head + s_cap - keep + i) % s_cap];
    }
    free(s_ev);
    s_ev = nw; s_cap = nw ? n : 0; s_len = keep; s_head = s_cap ? keep % s_cap : 0;
    xSemaphoreGive(s_mtx);
}

void oplog_add(uint8_t type, uint8_t a, uint16_t v) { oplog_add_src(type, a, v, auto_src(type)); }

void oplog_add_src(uint8_t type, uint8_t a, uint16_t v, uint8_t src)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (type == OPL_POWER) {
        s_pexact = s_pw != -1;                                               /* the first state learned is not a change: the amplifier was already so */
        s_psince = s_pexact ? (uint32_t)(esp_timer_get_time() / 1000000) : 0; s_pw = a;
        if (a == 1) { s_cnt.non++; s_dirty = true; }
    }
    if (s_ev && (g_set.oplog_mask & cat_of(type))) {
        s_ev[s_head] = (ev_t){ .up = (uint32_t)(esp_timer_get_time() / 1000000), .ts = (uint8_t)((src & 15) << 4 | (type & 15)), .a = a, .v = v };
        s_head = (s_head + 1) % s_cap;
        if (s_len < s_cap) s_len++;
    }
    xSemaphoreGive(s_mtx);
}

static void cnt_save(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READWRITE, &h) != ESP_OK) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    typeof(s_cnt) c = s_cnt; s_dirty = false;
    xSemaphoreGive(s_mtx);
    nvs_set_blob(h, "opcnt", &c, sizeof c);
    nvs_commit(h); nvs_close(h);
}
void oplog_flush(void) { if (s_mtx && s_dirty) cnt_save(); }

void oplog_reset_counters(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(&s_cnt, 0, sizeof s_cnt); s_cnt.magic = CNT_MAGIC;
    int64_t e = cli_now(); s_cnt.since = e > 0 ? (uint32_t)e : 0;
    xSemaphoreGive(s_mtx);
    s_dirty = true; cnt_save();
}

void oplog_clear(void) { if (!s_mtx) return; xSemaphoreTake(s_mtx, portMAX_DELAY); s_len = 0; s_head = 0; xSemaphoreGive(s_mtx); }

int oplog_set(int depth, int mask)
{
    if (!s_mtx) return -1;
    if (depth >= 0) { if ((unsigned)depth > oplog_max_depth()) depth = (int)oplog_max_depth(); g_set.oplog_depth = (uint16_t)depth; apply_depth((unsigned)depth); }
    if (mask >= 0) g_set.oplog_mask = (uint8_t)(mask & OPC_ALL);
    settings_save();
    return 0;
}

static void tick_task(void *arg)
{
    int64_t last = esp_timer_get_time(), saved = last, chg = 0;
    int lastx = -2, pend = 0; uint8_t psrc = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        uint32_t dt = (uint32_t)((now - last) / 1000000);
        if (!dt) continue;
        last += (int64_t)dt * 1000000;
        if (state_power() == 1) {
            int au = state_audio(), vi = state_video(), mu = state_mute();
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (!s_cnt.since) { int64_t e = cli_now(); if (e > 0) s_cnt.since = (uint32_t)e; }
            s_cnt.sec[15] += dt;
            if (vi >= 0x10) s_cnt.sec[slot_of(vi)] += dt;
            if (au >= 0 && !(vi >= 0x10 && au >= 0x10)) s_cnt.sec[slot_of(au)] += dt;          /* a video input brings its own audio: counted once */
            if (mu == 1) s_cnt.sec[16] += dt;
            s_dirty = true;
            xSemaphoreGive(s_mtx);
            int x = cli_vol_x10();                                               /* the volume: recorded when it has been still for 3 s */
            if (lastx == -2) lastx = x;
            else if (x != lastx) { lastx = x; chg = now; pend = 1; psrc = auto_src(OPL_VOLUME); }
            if (pend && now - chg >= 3000000 && x >= 0) { oplog_add_src(OPL_VOLUME, 0, (uint16_t)x, psrc); pend = 0; }
        } else pend = 0;
        if (s_dirty && now - saved > 1800LL * 1000000) { saved = now; cnt_save(); }
    }
}

void oplog_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    nvs_handle_t h; size_t n = sizeof s_cnt;
    if (nvs_open("slink", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_blob(h, "opcnt", &s_cnt, &n) != ESP_OK || n != sizeof s_cnt) memset(&s_cnt, 0, sizeof s_cnt); nvs_close(h); }
    if (s_cnt.magic != CNT_MAGIC) { memset(&s_cnt, 0, sizeof s_cnt); s_cnt.magic = CNT_MAGIC; }
    apply_depth(g_set.oplog_depth);
    oplog_add(OPL_BOOT, (uint8_t)esp_reset_reason(), 0);
    xTaskCreate(tick_task, "slink_oplog", 3072, NULL, 2, NULL);
}

size_t oplog_json(char *b, size_t sz, unsigned max_events)
{
    if (!s_mtx || sz < 400) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t o = 0;
    #define P(...) do { if (o < sz) { int w_ = snprintf(b + o, sz - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; } } while (0)
    P("{\"t\":\"oplog\",\"up\":%u,\"cap\":%u,\"len\":%u,\"depth\":%u,\"max\":%u,\"mask\":%u,\"psram\":%d,\"since\":%u,\"non\":%u,\"pw\":%d,\"psince\":%u,\"pexact\":%d,\"cnt\":[",
      (unsigned)(esp_timer_get_time() / 1000000), s_cap, s_len, (unsigned)g_set.oplog_depth, oplog_max_depth(), (unsigned)g_set.oplog_mask, s_psram, (unsigned)s_cnt.since, (unsigned)s_cnt.non, s_pw, (unsigned)s_psince, s_pexact);
    for (int i = 0; i < OPL_SLOTS; i++) P("%s%u", i ? "," : "", (unsigned)s_cnt.sec[i]);
    P("],\"ev\":[");
    unsigned room = o + 8 < sz ? (unsigned)((sz - o - 8) / 28) : 0, n = s_len;
    if (n > max_events) n = max_events;
    if (n > room) n = room;
    for (unsigned i = 0; i < n; i++) {
        const ev_t *e = &s_ev[(s_head + s_cap - n + i) % s_cap];
        P("%s[%u,%u,%u,%u,%u]", i ? "," : "", (unsigned)e->up, e->ts & 15, e->a, e->v, e->ts >> 4);
    }
    P("]}");
    #undef P
    xSemaphoreGive(s_mtx);
    return o < sz ? o : sz - 1;
}

void oplog_text(void (*out)(const char *fmt, ...), unsigned n)
{
    static const char *NM[] = { "?", "BOOT", "POWER", "INPUT", "MUTE", "VOLUME", "SOUNDFIELD", "TUNER", "TIME", "REBOOT", "OTA" };
    static const char *SRC[] = { "bus/remote", "usb", "tcp", "web", "websocket", "ble", "mqtt", "hue", "matter", "slink-e", "device" };
    ev_t tmp[60];
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);                                        /* copied first: the output may wait, and the receiving task writes here */
    if (n > s_len) n = s_len;
    if (n > 60) n = 60;
    for (unsigned i = 0; i < n; i++) tmp[i] = s_ev[(s_head + s_cap - n + i) % s_cap];
    xSemaphoreGive(s_mtx);
    for (unsigned i = 0; i < n; i++) {
        unsigned ty = tmp[i].ts & 15, sr = tmp[i].ts >> 4;
        out("%02u:%02u:%02u  %-10s %u %u  from %s\r\n", (unsigned)(tmp[i].up / 3600), (unsigned)(tmp[i].up / 60 % 60), (unsigned)(tmp[i].up % 60), ty < 11 ? NM[ty] : "?", tmp[i].a, tmp[i].v, sr < 11 ? SRC[sr] : "?");
    }
}
