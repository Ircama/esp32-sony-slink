/* S-Link physical layer: RX via GPIO edge ISR, TX via RMT. */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "slink.h"

/* Single-pin mode: one GPIO, open-drain, reads and drives the S-Link line. */
#ifdef CONFIG_SLINK_SINGLE_PIN
#define RX_GPIO_DEFAULT CONFIG_SLINK_TX_GPIO
#define TX_INVERT 0
#define TX_OD 1
#else
#define RX_GPIO_DEFAULT CONFIG_SLINK_RX_GPIO
#ifdef CONFIG_SLINK_TX_INVERT
#define TX_INVERT 1
#else
#define TX_INVERT 0
#endif
#define TX_OD 0
#endif

#ifdef CONFIG_SLINK_LED_INVERT
#define LED_INV_DEFAULT true
#else
#define LED_INV_DEFAULT false
#endif

typedef struct { uint32_t dur; uint8_t was_low; } edge_t;

static QueueHandle_t s_edge_q;     /* ISR -> decoder */
static QueueHandle_t s_frame_q;    /* decoder -> app */
static volatile int64_t s_last_us;
static volatile bool s_tx_active;
static volatile int64_t s_last_low_us;   /* last time line was seen low */
static rmt_channel_handle_t s_tx_chan;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_tx_mutex;
static int64_t s_last_tx_start_us;

/* LED mirror: LED on while the S-Link line is low (in or out traffic). */
static volatile int  s_gpio = RX_GPIO_DEFAULT;   /* S-Link bus pin (runtime selectable) */
static volatile int  s_led_gpio = -1;
static volatile bool s_led_inv;

/* Manual line tests (BLINK / LEVEL). */
static volatile uint32_t s_st_edges, s_st_syncs, s_st_bits, s_st_frames;   /* diagnostics */
static volatile uint32_t s_st_long, s_st_long_max;   /* low pulses longer than LONG_PULSE_US (a sync is 2400 us): wake-up pulses */
#define LONG_PULSE_US 6000
static volatile uint32_t s_dbg_dur[48];   /* last edge durations (diagnostics) */
static volatile uint8_t  s_dbg_lvl[48];
static volatile int s_dbg_n;
static volatile uint32_t s_br[6];   /* decoder branch counters (diagnostics) */
static volatile uint32_t s_gap_us = SLINK_FRAME_MIN_US;   /* min time between frame starts (GAP cmd) */
static volatile bool s_loopback;         /* decode our own frames as RX (line self-test) */
static volatile bool s_manual;           /* a test is driving the line */
static volatile uint32_t s_blink_req;    /* bumped on every new request */
static TaskHandle_t s_blink_task;
static struct { int l1, d1, l2, d2, n; } s_blink;

/* On every edge: duration of the state that just ended. */
static void IRAM_ATTR edge_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    int lvl = gpio_get_level(s_gpio);   /* new level */
    edge_t e = { .dur = (uint32_t)(now - s_last_us),
                 .was_low = lvl ? 1 : 0 };            /* new high => it was low */
    s_last_us = now;
    s_last_low_us = now;
    s_st_edges++;
    { int k = s_dbg_n; s_dbg_dur[k % 48] = e.dur; s_dbg_lvl[k % 48] = lvl; s_dbg_n = k + 1; }
    int led = s_led_gpio;
    if (led >= 0) gpio_set_level(led, ((lvl == 0) ^ s_led_inv) ? 1 : 0);
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_edge_q, &e, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void flush(slink_frame_t *f, int bitcnt, bool *in_frame)
{
    f->partial_bits = (uint8_t)bitcnt;
    if (f->len || bitcnt || f->error) { s_st_frames++; xQueueSend(s_frame_q, f, 0); }
    memset(f, 0, sizeof *f);
    *in_frame = false;
}

/* Decoder: low marks classified by width, MSB first. */
static void decoder_task(void *arg)
{
    slink_frame_t f;
    memset(&f, 0, sizeof f);
    bool in_frame = false;
    int bitcnt = 0;
    uint8_t cur = 0;

    for (;;) {
        edge_t e;
        /* tick may be 10 ms: pdMS_TO_TICKS(5) would be 0 (busy loop, frames cut) */
        TickType_t wt = pdMS_TO_TICKS(5);
        if (wt < 1) wt = 1;
        bool got = xQueueReceive(s_edge_q, &e, wt) == pdTRUE;

        if (s_tx_active) {           /* ignore our own echo */
            s_br[0]++;
            in_frame = false;
            bitcnt = 0; cur = 0;
            memset(&f, 0, sizeof f);
            continue;
        }
        if (!got) {                  /* idle timeout ends a frame */
            s_br[1]++;
            /* a 1-tick timeout may expire early: confirm with real elapsed time */
            if (in_frame && (esp_timer_get_time() - s_last_us) > 5000) {
                flush(&f, bitcnt, &in_frame); bitcnt = 0; cur = 0;
            }
            continue;
        }
        if (!e.was_low) {            /* a high gap just ended */
            s_br[2]++;
            if (e.dur > SLINK_RX_END_GAP && in_frame) {
                flush(&f, bitcnt, &in_frame); bitcnt = 0; cur = 0;
            }
            continue;
        }
        /* a low mark just ended */
        if (e.dur >= LONG_PULSE_US) { s_st_long++; if (e.dur > s_st_long_max) s_st_long_max = e.dur; }
        if (e.dur >= 1800) {         /* sync (nominal 2400) */
            if (in_frame) flush(&f, bitcnt, &in_frame);
            in_frame = true; bitcnt = 0; cur = 0; s_st_syncs++;
            continue;
        }
        if (!in_frame) { s_br[3]++; continue; }     /* garbage before sync */
        if (e.dur < 300) { s_br[4]++; f.error = true; continue; }
        s_st_bits++;
        cur = (uint8_t)((cur << 1) | (e.dur > 900 ? 1 : 0));
        if (++bitcnt == 8) {
            if (f.len < SLINK_MAX_FRAME) f.data[f.len++] = cur;
            bitcnt = 0; cur = 0;
        }
    }
}

/* ---- LED mirror ---- */
int slink_led_set(int gpio, bool invert)
{
    int old = s_led_gpio;
    s_led_gpio = -1;
    if (old >= 0) { gpio_set_level(old, invert ? 1 : 0); }
    if (gpio < 0) return 0;
    if (gpio == s_gpio || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return -1;
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    s_led_inv = invert;
    gpio_set_level(gpio, ((gpio_get_level(s_gpio) == 0) ^ invert) ? 1 : 0);
    s_led_gpio = gpio;
    return 0;
}

/* ---- manual line tests ---- */
/* The RMT idle level is latched by a transmission: one tiny symbol sets the line. */
static void line_set(int lvl)
{
    rmt_symbol_word_t one = { .duration0 = 10, .level0 = lvl ? 1 : 0,
                              .duration1 = 10, .level1 = lvl ? 1 : 0 };
    rmt_transmit_config_t c = { .loop_count = 0, .flags.eot_level = lvl ? 1 : 0 };
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    rmt_transmit(s_tx_chan, s_enc, &one, sizeof one, &c);
    rmt_tx_wait_all_done(s_tx_chan, 100);
    xSemaphoreGive(s_tx_mutex);
}

/* Sleep ms, abort early (return true) if a newer request arrived. */
static bool sleep_or_abort(int ms, uint32_t req)
{
    while (ms > 0) {
        int step = ms > 10 ? 10 : ms;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        ms -= step;
        if (s_blink_req != req) return true;
    }
    return false;
}

static void blink_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t req = s_blink_req;
        if (s_blink.n < 0) continue;                  /* stop request: already released */
        s_tx_active = true;                           /* decoder ignores our own edges */
        for (int i = 0; s_blink.n == 0 || i < s_blink.n; i++) {
            line_set(s_blink.l1);
            if (sleep_or_abort(s_blink.d1, req)) break;
            line_set(s_blink.l2);
            if (sleep_or_abort(s_blink.d2, req)) break;
        }
        if (s_blink_req == req) {                     /* finished normally: release */
            line_set(1);
            s_manual = false;
        }
        xQueueReset(s_edge_q);
        s_last_low_us = esp_timer_get_time();
        s_tx_active = false;
    }
}

/* n cycles (0 = until BLINK OFF); l=1 high (released), l=0 low. */
int slink_blink(int l1, int d1, int l2, int d2, int n)
{
    if (d1 < 1 || d2 < 1 || n < 0) return -1;
    if (!s_blink_task) return -2;
    s_manual = true;
    s_blink.l1 = l1; s_blink.d1 = d1; s_blink.l2 = l2; s_blink.d2 = d2; s_blink.n = n;
    s_blink_req++;
    xTaskNotifyGive(s_blink_task);
    return 0;
}

/* Hold the line high (released) or low until slink_test_off(). */
int slink_level(int lvl)
{
    s_blink.n = -1;
    s_blink_req++;                                    /* abort any running blink */
    s_manual = true;
    vTaskDelay(pdMS_TO_TICKS(30));
    s_tx_active = true;
    line_set(lvl);
    return 0;
}

/* BLINK OFF / LEVEL OFF: stop immediately and release the line. */
void slink_test_off(void)
{
    s_blink.n = -1;
    s_blink_req++;
    vTaskDelay(pdMS_TO_TICKS(30));
    line_set(1);
    s_manual = false;
    xQueueReset(s_edge_q);
    s_last_low_us = esp_timer_get_time();
    s_tx_active = false;
}

bool slink_test_active(void) { return s_manual; }

static bool s_hw_up;
static volatile int s_pad = SLINK_PAD_OD_PULLUP;   /* pad electrical mode, see slink.h */

/* Pad mode of the bus pin: open-drain with or without the internal pull-up, or push-pull. Only touches the pad (the RMT keeps the output
 * path of the pin in single-pin builds, so gpio_config() must not be used on it). */
static void pad_apply(int g)
{
#if TX_OD
    if (s_pad == SLINK_PAD_PUSHPULL) gpio_ll_od_disable(&GPIO, g); else gpio_ll_od_enable(&GPIO, g);
    gpio_ll_input_enable(&GPIO, g);
    gpio_set_pull_mode(g, s_pad == SLINK_PAD_OD_PULLUP ? GPIO_PULLUP_ONLY : GPIO_FLOATING);   /* pull-up: idle high even when unplugged */
    gpio_set_intr_type(g, GPIO_INTR_ANYEDGE);
#else
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << g,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_pad == SLINK_PAD_OD_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&in);
#endif
}

/* Configure RMT + pad + RX interrupt on the given pin (single-pin: pin = bus). */
static int hw_start(int g)
{
    if (!s_tx_chan) {
        rmt_tx_channel_config_t tc = {
            .gpio_num = (TX_OD ? g : CONFIG_SLINK_TX_GPIO),
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000,
            .mem_block_symbols = 48,
            .trans_queue_depth = 1,
            .flags.invert_out = TX_INVERT,
            .flags.init_level = 1,   /* idle = line released */
        };
        if (rmt_new_tx_channel(&tc, &s_tx_chan) != ESP_OK) return -1;
        rmt_copy_encoder_config_t ec = {};
        if (rmt_new_copy_encoder(&ec, &s_enc) != ESP_OK) return -1;
    } else if (TX_OD) {
        /* channel kept: only move its output to the new pin (channel must be disabled) */
        if (rmt_tx_switch_gpio(s_tx_chan, g, TX_INVERT) != ESP_OK) return -1;
    }
    if (rmt_enable(s_tx_chan) != ESP_OK) return -1;

    /* Idle level is only latched by a transmission: send a dummy all-high symbol
     * with eot_level=1 so the line is released (not held low) from now on. */
    {
        rmt_symbol_word_t rel = { .duration0 = 10, .level0 = 1, .duration1 = 10, .level1 = 1 };
        rmt_transmit_config_t rc0 = { .loop_count = 0, .flags.eot_level = 1 };
        rmt_transmit(s_tx_chan, s_enc, &rel, sizeof rel, &rc0);
        rmt_tx_wait_all_done(s_tx_chan, 100);
    }

    pad_apply(g);
    s_gpio = g;
    s_last_us = esp_timer_get_time();
    gpio_isr_handler_add(g, edge_isr, NULL);
    s_hw_up = true;
    return 0;
}

static void hw_stop(void)
{
    int g = s_gpio;
    gpio_isr_handler_remove(g);
    gpio_intr_disable(g);
    rmt_disable(s_tx_chan);          /* channel kept for the next hw_start() */
    gpio_reset_pin(g);               /* hi-Z, no pull */
    s_hw_up = false;
}

/* Move the S-Link bus to another GPIO (single-pin mode only). */
int slink_set_gpio(int g)
{
#ifndef CONFIG_SLINK_SINGLE_PIN
    (void)g;
    return -2;                        /* dual-pin build: pins fixed by menuconfig */
#else
    if (!GPIO_IS_VALID_OUTPUT_GPIO(g) || g == s_led_gpio) return -1;
    if (g == s_gpio) return 0;
    if (s_manual) slink_test_off();
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    s_tx_active = true;
    hw_stop();
    int rc = hw_start(g);
    if (rc != 0) {                    /* roll back to the previous pin */
        int old = s_gpio;
        hw_start(old);
        rc = -3;
    }
    xQueueReset(s_edge_q);
    s_last_low_us = esp_timer_get_time();
    s_tx_active = false;
    xSemaphoreGive(s_tx_mutex);
    return rc;
#endif
}

int slink_get_gpio(void) { return s_gpio; }

int slink_get_pad(void) { return s_pad; }
int slink_set_pad(int mode)
{
    if (mode < SLINK_PAD_OD_PULLUP || mode > SLINK_PAD_PUSHPULL) return -1;
    if (!s_tx_mutex || !s_hw_up) { s_pad = mode; return 0; }       /* before start-up: applied by hw_start() */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    s_pad = mode;
    pad_apply(s_gpio);
    xSemaphoreGive(s_tx_mutex);
    return 0;
}

int slink_init(void)
{
    s_edge_q   = xQueueCreate(256, sizeof(edge_t));
    s_frame_q  = xQueueCreate(8, sizeof(slink_frame_t));
    s_tx_mutex = xSemaphoreCreateMutex();
    if (xTaskCreate(blink_task, "slink_blink", 4096, NULL, 4, &s_blink_task) != pdPASS) return -2;

    gpio_install_isr_service(0);
    if (hw_start(s_gpio) != 0) return -1;

    xTaskCreate(decoder_task, "slink_rx", 4096, NULL, 10, NULL);
#ifdef CONFIG_SLINK_LED_GPIO
    slink_led_set(CONFIG_SLINK_LED_GPIO, LED_INV_DEFAULT);
#endif
    return 0;
}

static inline rmt_symbol_word_t sym(uint32_t low_us)
{
    /* logical: line low for low_us, then released (high) for the delimiter */
    rmt_symbol_word_t s = { .duration0 = low_us, .level0 = 0,
                            .duration1 = SLINK_MARK_DELIM, .level1 = 1 };
    return s;
}

static bool wait_line_ready(void)
{
    int64_t start = esp_timer_get_time();
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (gpio_get_level(s_gpio) == 0) s_last_low_us = now;
        if (now - s_last_low_us >= SLINK_LINE_READY &&
            now - s_last_tx_start_us >= (int64_t)s_gap_us) return true;
        if (now - start > 500000) return false;
        vTaskDelay(1);
    }
}

int slink_send(const uint8_t *data, size_t len, int repeat)
{
    if (!data || len == 0 || len > SLINK_MAX_FRAME) return -1;
    if (s_manual) return -3;   /* BLINK/LEVEL test active */
    if (repeat < 1) repeat = 1;

    rmt_symbol_word_t syms[1 + 8 * SLINK_MAX_FRAME];
    size_t n = 0;
    syms[n++] = sym(SLINK_MARK_SYNC);
    for (size_t i = 0; i < len; i++)
        for (int b = 7; b >= 0; b--)
            syms[n++] = sym(((data[i] >> b) & 1) ? SLINK_MARK_ONE : SLINK_MARK_ZERO);

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    int rc = 0;
    for (int r = 0; r < repeat; r++) {
        if (!wait_line_ready()) { rc = -2; break; }
        rmt_transmit_config_t cfg = { .loop_count = 0, .flags.eot_level = 1 };  /* release line when done */
        s_tx_active = !s_loopback;
        s_last_tx_start_us = esp_timer_get_time();
        rmt_transmit(s_tx_chan, s_enc, syms, n * sizeof(syms[0]), &cfg);
        rmt_tx_wait_all_done(s_tx_chan, 500);
        vTaskDelay(pdMS_TO_TICKS(2));
        if (!s_loopback) xQueueReset(s_edge_q);
        s_last_low_us = esp_timer_get_time();
        s_tx_active = false;
    }
    xSemaphoreGive(s_tx_mutex);
    return rc;
}

bool slink_receive(slink_frame_t *f, uint32_t timeout_ms)
{
    return xQueueReceive(s_frame_q, f, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

int slink_line_level(void) { return gpio_get_level(s_gpio); }

void slink_set_loopback(bool on) { s_loopback = on; }
bool slink_get_loopback(void) { return s_loopback; }

void slink_long_pulses(uint32_t *count, uint32_t *max_us) { *count = s_st_long; *max_us = s_st_long_max; }
void slink_stats(uint32_t *edges, uint32_t *syncs, uint32_t *bits, uint32_t *frames)
{ *edges = s_st_edges; *syncs = s_st_syncs; *bits = s_st_bits; *frames = s_st_frames; }

/* Copy the last n edge records (oldest first): duration in us and the new level. */
int slink_dbg_edges(uint32_t *dur, uint8_t *lvl, int max)
{
    int total = s_dbg_n, n = total < 48 ? total : 48;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        int idx = (total - n + i) % 48;
        dur[i] = s_dbg_dur[idx];
        lvl[i] = s_dbg_lvl[idx];
    }
    return n;
}

void slink_branches(uint32_t *o) { for (int i = 0; i < 6; i++) o[i] = s_br[i]; }

void slink_set_gap_ms(int ms) { s_gap_us = (uint32_t)(ms < 0 ? 0 : ms) * 1000u; }
int slink_get_gap_ms(void) { return (int)(s_gap_us / 1000u); }
