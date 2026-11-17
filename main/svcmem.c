#include <stdio.h>
#include "esp_heap_caps.h"
#include "svcmem.h"

static int32_t s_bi[SVC_N], s_bp[SVC_N];        /* free heap before the start: internal RAM, PSRAM */
static int32_t s_i[SVC_N], s_p[SVC_N];          /* what the function took */

static int32_t free_int(void) { return (int32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static int32_t free_ps(void) { return (int32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

void svc_mark(int id)
{
    if (id < 0 || id >= SVC_N) return;
    s_bi[id] = free_int();
    s_bp[id] = free_ps();
}

void svc_done(int id)
{
    if (id < 0 || id >= SVC_N) return;
    int32_t i = s_bi[id] - free_int(), p = s_bp[id] - free_ps();
    s_i[id] = i > 0 ? i : 0;                    /* another task may have freed memory meanwhile: never negative */
    s_p[id] = p > 0 ? p : 0;
}

void svc_clear(int id)
{
    if (id < 0 || id >= SVC_N) return;
    s_i[id] = s_p[id] = 0;
}

size_t svc_json(char *b, size_t n, const uint8_t on[SVC_N])
{
    size_t k = 0;
    int w = snprintf(b, n, "\"svc\":[");
    if (w < 0 || (size_t)w >= n) return 0;
    k = (size_t)w;
    for (int i = 0; i < SVC_N; i++) {
        w = snprintf(b + k, n - k, "%s[%d,%d,%d,%d]", i ? "," : "", i, on[i] ? 1 : 0, (int)s_i[i], (int)s_p[i]);
        if (w < 0 || (size_t)w >= n - k) return 0;
        k += (size_t)w;
    }
    w = snprintf(b + k, n - k, "],");
    if (w < 0 || (size_t)w >= n - k) return 0;
    return k + (size_t)w;
}
