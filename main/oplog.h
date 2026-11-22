#pragma once
#include <stddef.h>
#include <stdint.h>

/* Functional log of the operations (power, inputs, mute, volume, sound field, radio, system) and the time the amplifier spent in each state.
 * The events are kept in a ring buffer of 8 bytes each: in the PSRAM when there is one (the depth is a setting), otherwise in a small one in the internal RAM.
 * The log is lost at a restart; the counters of the time (power on, radio, every input, mute) are saved to the flash every half hour and before a restart. */

enum { OPL_BOOT = 1, OPL_POWER, OPL_INPUT, OPL_MUTE, OPL_VOLUME, OPL_SF, OPL_TUNER, OPL_TIME, OPL_REBOOT, OPL_OTA };
/* what the user can choose to record (bits of the mask) */
enum { OPC_POWER = 1, OPC_INPUT = 2, OPC_MUTE = 4, OPC_VOLUME = 8, OPC_SF = 16, OPC_TUNER = 32, OPC_SYSTEM = 64, OPC_ALL = 127 };

#define OPL_SLOTS 17          /* time counters: 0-14 the inputs (tuner, phono, cd, dat, md, tape, digital, video1-4, dvd, sat, tv, other), 15 power on, 16 muted */

void     oplog_init(void);                                    /* after settings_load(), state_init(), nvs_flash_init() */
void     oplog_add(uint8_t type, uint8_t a, uint16_t v);       /* records an event (when its category is selected); thread safe. The origin is the one of the last command (CLI_SRC_*) if it was given a moment ago, else the bus */
void     oplog_add_src(uint8_t type, uint8_t a, uint16_t v, uint8_t src);
void     oplog_origin(uint8_t src);                            /* a command that changes the amplifier is being executed for this source */
size_t   oplog_json(char *b, size_t sz, unsigned max_events);  /* {"t":"oplog",...}: settings, counters and the newest events (oldest first) */
void     oplog_text(void (*out)(const char *fmt, ...), unsigned n);   /* the newest n events as text lines (console) */
int      oplog_set(int depth, int mask);                      /* depth in events (0 = off), mask of OPC_*; -1 leaves one as it is. Saved. 0 = ok */
void     oplog_clear(void);
void     oplog_reset_counters(void);
void     oplog_flush(void);                                   /* saves the counters now (before a restart) */
unsigned oplog_max_depth(void);                               /* the largest depth this chip can hold */
unsigned oplog_len(void);
