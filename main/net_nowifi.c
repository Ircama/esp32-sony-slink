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
#include "net.h"
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

void net_apply(void)
{
    power_apply();
    if (g_set.ble) ble_nus_start(); else ble_nus_stop();
}

void net_init(void) { net_apply(); }

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
void net_notify_state(void) {}
void net_notify_ws_client(void) {}
int  net_twt_state(unsigned *interval_ms) { if (interval_ms) *interval_ms = 0; return NET_TWT_NO_CHIP; }

void web_start(void) {}
void web_stop(void) {}
bool web_running(void) { return false; }
void web_push(const char *json, size_t n) { (void)json; (void)n; }
int  web_clients(void) { return 0; }
void tcp_srv_start(void) {}
void tcp_srv_stop(void) {}
int  tcp_srv_clients(void) { return 0; }
void mqtt_bridge_start(void) {}
void mqtt_bridge_stop(void) {}
bool mqtt_bridge_running(void) { return false; }
bool mqtt_bridge_connected(void) { return false; }
void mqtt_bridge_push(const char *json, size_t n) { (void)json; (void)n; }

size_t stats_json(char *b, size_t sz)
{
    return (size_t)snprintf(b, sz, "{\"t\":\"stats\",\"up\":%lld,\"chip\":\"esp32h2\",\"cpu\":%u,\"eco\":%d,\"heap\":%u,\"heapMin\":%u}",
                            (long long)(esp_timer_get_time() / 1000000), (unsigned)esp_rom_get_cpu_ticks_per_us(), g_set.eco,
                            (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}
