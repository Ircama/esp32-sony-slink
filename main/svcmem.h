#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* RAM taken by each interface / function, MEASURED on the chip: the free heap just before it starts and some time after (svc_mark / svc_done).
 * It includes the task stacks and the buffers it allocates at start; it does not include what open connections need later.
 * The numbers go to the Statistics page ("svc" in the stats frame). */
#ifdef __cplusplus
extern "C" {
#endif

enum { SVC_BLE, SVC_WIFI, SVC_MDNS, SVC_WEB, SVC_HTTPS, SVC_HUE, SVC_APP, SVC_SLKE, SVC_MQTT, SVC_MATTER, SVC_N };

void svc_mark(int id);                          /* before starting: remember the free heap */
void svc_done(int id);                          /* some time after: the difference is what the function took (internal RAM and PSRAM) */
void svc_clear(int id);                         /* stopped */
/* "svc":[[id,on,internal bytes,psram bytes],...], with the trailing comma; on[id] = running now */
size_t svc_json(char *b, size_t n, const uint8_t on[SVC_N]);

#ifdef __cplusplus
}
#endif
