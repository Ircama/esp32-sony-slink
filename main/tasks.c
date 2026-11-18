/* The tasks for the Statistics page ("GET /api/tasks", "@GET tasks" over Bluetooth): name, priority, CPU, stack margin and state of every task.
 * The CPU is the share of one core in the window since the previous call (when it was between 0.8 s and 2 minutes ago, so a page that asks every few seconds
 * sees the load now), else since the start. The counters are 32 bits: the window is exact, the value since the start only in the first 71 minutes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "net.h"

typedef struct { TaskHandle_t h; configRUN_TIME_COUNTER_TYPE c; } snap_t;
static snap_t *s_snap;
static unsigned s_snap_n, s_snap_cap;
static configRUN_TIME_COUNTER_TYPE s_snap_total;
static int64_t s_snap_at;
static SemaphoreHandle_t s_mtx;

size_t tasks_json(char *b, size_t sz)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx || sz < 64) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = 0;
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *a = malloc(cap * sizeof *a);
    if (a) {
        configRUN_TIME_COUNTER_TYPE total = 0;
        UBaseType_t cnt = uxTaskGetSystemState(a, cap, &total);
        int64_t now = esp_timer_get_time();
        bool win = s_snap_n && now - s_snap_at >= 800000 && now - s_snap_at < 120000000;
        configRUN_TIME_COUNTER_TYPE dt = win ? (configRUN_TIME_COUNTER_TYPE)(total - s_snap_total) : total;
        n = (size_t)snprintf(b, sz, "{\"t\":\"tasks\",\"win\":%d,\"cores\":%d,\"tasks\":[", win ? (int)((now - s_snap_at) / 1000000) : -1, (int)portNUM_PROCESSORS);
        for (UBaseType_t i = 0; i < cnt && n < sz; i++) {
            configRUN_TIME_COUNTER_TYPE c = a[i].ulRunTimeCounter;
            if (win) for (unsigned k = 0; k < s_snap_n; k++) if (s_snap[k].h == a[i].xHandle) { c = (configRUN_TIME_COUNTER_TYPE)(c - s_snap[k].c); break; }
            double cpu = dt ? (double)c * 100.0 / (double)dt : 0;
            int w = snprintf(b + n, sz - n, "%s{\"n\":\"%s\",\"p\":%u,\"c\":%.1f,\"m\":%u,\"s\":%d}", i ? "," : "", a[i].pcTaskName, (unsigned)a[i].uxCurrentPriority,
                             cpu, (unsigned)(a[i].usStackHighWaterMark * sizeof(StackType_t)), (int)a[i].eCurrentState);
            if (w < 0 || (size_t)w >= sz - n) { n = 0; break; }                    /* does not fit: nothing rather than a broken frame */
            n += (size_t)w;
        }
        if (n && n + 3 <= sz) { b[n++] = ']'; b[n++] = '}'; b[n] = 0; } else n = 0;
        if (!s_snap || s_snap_cap < cnt) {
            snap_t *p = realloc(s_snap, (cnt + 8) * sizeof *p);
            if (p) { s_snap = p; s_snap_cap = cnt + 8; }
        }
        if (s_snap && s_snap_cap >= cnt) {
            for (UBaseType_t i = 0; i < cnt; i++) { s_snap[i].h = a[i].xHandle; s_snap[i].c = a[i].ulRunTimeCounter; }
            s_snap_n = cnt; s_snap_total = total; s_snap_at = now;
        }
        free(a);
    }
    xSemaphoreGive(s_mtx);
    return n;
}
