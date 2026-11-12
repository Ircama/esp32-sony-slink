#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* S-Link timing (microseconds) */
#define SLINK_MARK_SYNC       2400
#define SLINK_MARK_ONE        1200
#define SLINK_MARK_ZERO        600
#define SLINK_MARK_DELIM       600
#define SLINK_LINE_READY      3000   /* line must be idle (high) this long before TX */
#define SLINK_FRAME_MIN_US   30000   /* min time from frame start to next TX */
#define SLINK_RX_END_GAP      2000   /* high gap that ends a frame */
#define SLINK_MAX_FRAME         24

typedef struct {
    uint8_t len;
    uint8_t data[SLINK_MAX_FRAME];
    uint8_t partial_bits;   /* leftover bits (frame not byte-aligned) */
    bool    error;          /* timing error inside frame */
} slink_frame_t;

int  slink_init(void);
/* Send a frame. Returns 0 on success, -1 bad args, -2 line busy timeout */
int  slink_send(const uint8_t *data, size_t len, int repeat);
/* Wait up to timeout_ms for a received frame. */
bool slink_receive(slink_frame_t *f, uint32_t timeout_ms);
int slink_line_level(void);   /* 1 = idle/high, 0 = low */
int  slink_led_set(int gpio, bool invert);        /* gpio<0: off. LED on while line is low */
int  slink_blink(int l1, int d1, int l2, int d2, int n);
int  slink_level(int lvl);
void slink_test_off(void);
bool slink_test_active(void);
int  slink_set_gpio(int gpio);   /* 0 ok, -1 invalid pin, -2 unsupported (dual-pin), -3 failed (rolled back) */
int  slink_get_gpio(void);
/* electrical mode of the bus pin */
enum { SLINK_PAD_OD_PULLUP = 0,    /* open-drain, internal pull-up on (default: idle high even with nothing connected) */
       SLINK_PAD_OD = 1,           /* open-drain, no internal pull-up (the bus / an external resistor provides the high level) */
       SLINK_PAD_PUSHPULL = 2 };   /* push-pull, no pull: drives 3.3 V onto the line, only for a 3.3 V bus */
int  slink_set_pad(int mode);       /* 0 ok, -1 invalid */
int  slink_get_pad(void);
bool slink_get_loopback(void);
void slink_set_loopback(bool on);   /* decode own TX frames as RX: verifies the pin/line */
void slink_stats(uint32_t *edges, uint32_t *syncs, uint32_t *bits, uint32_t *frames);
void slink_long_pulses(uint32_t *count, uint32_t *max_us);   /* low pulses > 6 ms seen on the bus (wake-up pulses) and the longest, in us */
int slink_dbg_edges(uint32_t *dur, uint8_t *lvl, int max);
void slink_branches(uint32_t *o6);
void slink_set_gap_ms(int ms);   /* min time between frame starts (default 30) */
int  slink_get_gap_ms(void);
