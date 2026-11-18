/* Network layer for chips WITHOUT WiFi (ESP32-H2): only the USB serial port and BLE (Nordic UART) are available.
 * Same API as net.c; the web portal, the TCP console and MQTT do not exist here (their functions are inert stubs).
 *
 * Power: the biggest consumer of the WiFi chips (the WiFi radio) is simply not there. What is left is the CPU, the BLE radio
 * (advertising / one connection) and the USB Serial/JTAG block. The "ECO" setting scales the CPU between 32 and 48 MHz instead of
 * 96 MHz (dynamic frequency scaling, no light sleep: the S-Link receiver needs the GPIO interrupt latency, see net.c) and slows the
 * BLE advertising (ble_nus.c). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "net.h"
#include "svcmem.h"
#include "settings.h"
#include "state.h"
#include "cli.h"

static int s_pm_rc = -1;                    /* result of the last esp_pm_configure() (0 = ok) */

static void power_apply(void)
{
    esp_pm_config_t pm = { .max_freq_mhz = g_set.eco ? 48 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                           .min_freq_mhz = g_set.eco ? 32 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                           .light_sleep_enable = false };
    s_pm_rc = esp_pm_configure(&pm);
}

static bool s_ble_on;

void net_apply(void)
{
    power_apply();
    if (g_set.ble) {
        bool first = !s_ble_on;
        if (first) svc_mark(SVC_BLE);
        ble_nus_start();
        if (first) { s_ble_on = true; vTaskDelay(pdMS_TO_TICKS(250)); svc_done(SVC_BLE); }      /* the RAM the Bluetooth interface took (Statistics) */
    } else {
        ble_nus_stop();
        if (s_ble_on) { s_ble_on = false; svc_clear(SVC_BLE); }
    }
}

size_t net_svc_json(char *b, size_t n)
{
    uint8_t on[SVC_N] = {0};
    on[SVC_BLE] = ble_nus_running();
    return svc_json(b, n, on);
}

void net_init(void) { state_set_listener(net_notify_state); net_apply(); }

/* the same {"t":"net"} frame as net.c, for the Bluetooth page: no WiFi, no web portal, no TCP */
size_t net_json_get(char *b, size_t sz)
{
    char bm[20];
    ble_nus_mac(bm, sizeof bm);
    int n = snprintf(b, sz, "{\"t\":\"net\",\"ble\":%d,\"web\":0,\"app\":0,\"eco\":%d,\"bleConn\":%d,\"bleCustom\":%d,\"blePw\":%d,\"bleName\":\"%s\",\"bleMac\":\"%s\",\"wifiMac\":\"\",\"wifi\":\"none\",\"rssi\":0,\"ip\":\"\",\"dns\":\"\","
                            "\"ssid\":\"\",\"host\":\"%s\",\"mqtt\":\"\",\"mqttUser\":\"\",\"mqttOk\":0,\"tcp\":0,\"slke\":0,\"slkeTx\":0,\"slkeN\":0,\"https\":0}",
                     g_set.ble, g_set.eco, ble_nus_connected(), g_set.ble_name[0] != 0, g_set.ble_pass[0] != 0, ble_nus_name(), bm, g_set.host);
    return n > 0 && (size_t)n < sz ? (size_t)n : 0;
}

void net_status(char *b, size_t n)
{
    snprintf(b, n, "WIFI  not available (the ESP32-H2 has no WiFi radio: USB serial and BLE only)\r\n"
                   "BLE   %s%s\r\n"
                   "POWER eco %s: CPU %d MHz now (limits %d..%d MHz), power management %s",
             g_set.ble ? "ON" : "OFF", g_set.ble ? (ble_nus_connected() ? ", client connected" : ", advertising as S-Link-xxxx") : "",
             g_set.eco ? "ON" : "OFF", (int)esp_rom_get_cpu_ticks_per_us(), g_set.eco ? 32 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             g_set.eco ? 48 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, s_pm_rc == 0 ? "configured" : "NOT configured (check CONFIG_PM_ENABLE)");
}

bool net_have_ip(void) { return false; }
void net_notify_state(void) { ble_nus_state_changed(); }
void net_notify_ws_client(void) {}
int  net_twt_state(unsigned *interval_ms) { if (interval_ms) *interval_ms = 0; return NET_TWT_NO_CHIP; }

void web_start(void) {}
void web_stop(void) {}
bool web_running(void) { return false; }
bool web_https_running(void) { return false; }
void web_push(const char *json, size_t n) { (void)json; (void)n; }
int  web_clients(void) { return 0; }
void tcp_srv_start(void) {}
void tcp_srv_stop(void) {}
int  tcp_srv_clients(void) { return 0; }
void hue_start(void) {}
void hue_stop(void) {}
bool hue_running(void) { return false; }
void hue_register(void *h) { (void)h; }
void slinke_start(void) {}
void slinke_stop(void) {}
int  slinke_clients(void) { return 0; }
void mqtt_bridge_start(void) {}
void mqtt_bridge_stop(void) {}
bool mqtt_bridge_running(void) { return false; }
bool mqtt_bridge_connected(void) { return false; }
void mqtt_bridge_push(const char *json, size_t n) { (void)json; (void)n; }

/* statistics for the Bluetooth page ("@GET stats"): the same fields as stats.c that make sense without WiFi, OTA and temperature sensor */
size_t clients_json(char *b, size_t sz) { return (size_t)snprintf(b, sz, "{\"t\":\"clients\",\"c\":[]}"); }     /* no WiFi: no TCP/IP clients */

size_t stats_json(char *b, size_t sz)
{
    esp_chip_info_t ci; esp_chip_info(&ci);
    const esp_app_desc_t *ad = esp_app_get_description();
    char bm[20] = "", bn[48] = "", dg[200] = "", sv[200] = "";
    net_svc_json(sv, sizeof sv);
    ble_nus_mac(bm, sizeof bm); snprintf(bn, sizeof bn, "%s", ble_nus_name()); ble_nus_diag(dg, sizeof dg);
    const char *rs = "other";
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: rs = "power-on"; break;   case ESP_RST_SW: rs = "software"; break;       case ESP_RST_PANIC: rs = "panic"; break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: rs = "watchdog"; break;          case ESP_RST_BROWNOUT: rs = "brownout"; break;
    default: break;
    }
    int n = snprintf(b, sz,
        "{\"t\":\"stats\",\"inst\":0,\"slot\":\"-\",\"ostate\":\"unknown\",\"fimg\":0,\"fslot\":0,\"fflash\":0,\"up\":%lld,\"now\":%lld,\"tz\":%d,\"reset\":\"%s\",\"chip\":\"esp32h2\",\"rev\":%d,\"cores\":%d,"
        "\"fw\":\"%.31s\",\"built\":\"%.15s %.8s\",\"idf\":\"%.31s\",\"temp\":null,"
        "\"heap\":%u,\"heapMin\":%u,\"heapBlock\":%u,\"heapTotal\":%u,\"tasks\":%u,\"psram\":0,\"psramFree\":0,\"psramMin\":0,%s"
        "\"cpu\":%u,\"cpuMax\":%d,\"cpuMin\":%d,\"eco\":%d,\"twt\":0,\"twtMs\":0,\"host\":\"\",\"mac\":\"\",\"bleMac\":\"%s\",\"bleName\":\"%s\",\"bleDiag\":\"%s\"}",
        (long long)(esp_timer_get_time() / 1000000), (long long)cli_now(), cli_tz(), rs, ci.revision, ci.cores, ad->version, ad->date, ad->time, ad->idf_ver,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetNumberOfTasks(), sv, (unsigned)esp_rom_get_cpu_ticks_per_us(), g_set.eco ? 48 : 96, g_set.eco ? 32 : 96, g_set.eco, bm, bn, dg);
    return n > 0 && (size_t)n < sz ? (size_t)n : 0;
}
