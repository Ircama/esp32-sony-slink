#pragma once
#include <stddef.h>
#include <stdbool.h>

/* Matter (matter.cpp): the amplifier as a Matter "dimmable light" for Google Home, Apple Home, SmartThings, Home Assistant ...
 *     on / off  -> power on / power off      level -> volume: 100 % of the level is the "maximum volume" (MATTER MAX, default 30 % of the knob)
 * ESP32-S3 only, and only in the build that has the esp-matter stack (SLINK_MATTER, see docs/MATTER.md). Every other build gets the empty versions below,
 * so the settings and the commands are the same everywhere. */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef SLINK_MATTER
void   matter_start(void);                          /* at boot: starts the stack when MATTER is on (does its own task) */
bool   matter_available(void);                      /* this build has Matter */
bool   matter_running(void);
int    matter_fabrics(void);                        /* ecosystems that have commissioned the device (0 = not paired yet) */
void   matter_report(void);                         /* the amplifier changed: tell the controllers (any task, does not block) */
size_t matter_info(char *buf, size_t n);            /* human readable: state, name, pairing codes (CRLF separated) */
size_t matter_json(char *buf, size_t n);            /* ',"mtr..."' fields for the {"t":"net"} frame, with a leading comma */
void   matter_open(void);                           /* open the commissioning window for 15 minutes */
void   matter_new_code(void);                       /* new passcode + discriminator (takes effect at the next start) */
void   matter_factory_reset(void);                  /* forget every ecosystem (the device restarts) */
void   matter_restart_soon(void);                   /* restart in about 1.5 s (a task: the command can answer first) */
#else
static inline void   matter_start(void) {}
static inline bool   matter_available(void) { return false; }
static inline bool   matter_running(void) { return false; }
static inline int    matter_fabrics(void) { return 0; }
static inline void   matter_report(void) {}
static inline size_t matter_info(char *b, size_t n) { (void)b; (void)n; return 0; }
static inline size_t matter_json(char *b, size_t n) { (void)b; (void)n; return 0; }
static inline void   matter_open(void) {}
static inline void   matter_new_code(void) {}
static inline void   matter_factory_reset(void) {}
static inline void   matter_restart_soon(void) {}
#endif

#ifdef __cplusplus
}
#endif
