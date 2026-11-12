#pragma once
#include <stddef.h>
#include <stdint.h>

/* Structured picture of the amplifier, built from the frames seen on the bus (replies C8/C9/CB).
 * Feeds the web portal (WebSocket) and the MQTT bridge. */
void state_init(void);
void state_feed(const uint8_t *d, size_t n);                 /* call for every received frame */
void state_set_power(int on);                                /* optimistic update after our own PWRON/PWROFF */
void state_set_listener(void (*cb)(void));                   /* called (from the rx task) after a change */
size_t state_json(char *buf, size_t sz, int vol_pct, int vol_x10); /* {"t":"state",...} ; volume -1 = unknown; vol_x10 = tenths of a percent */
size_t state_presets_json(char *buf, size_t sz);             /* {"t":"presets","presets":[{"b":1,"n":1,"f":8750,..}]} */
const char *state_input_name(int code);                      /* "TUNER", "MD", ... or NULL */
bool state_tuner(unsigned *f, bool *am, char *name, size_t n); /* tuner now: f (FM 10 kHz units / AM kHz), band, RDS name; false if unknown */
