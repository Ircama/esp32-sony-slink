/* Device statistics for the web "Statistics" tab (GET /api/stats).
 * Everything is read ON REQUEST: nothing runs in the background, so it costs no power while the tab is closed.
 *
 * What the chip can NOT tell: supply voltage and current. The ESP32-C3/S3 have no internal VDD33 or current
 * measurement; that needs an external sensor (INA219/INA226 on I2C) or a USB power meter. */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_app_desc.h"
#include "ota.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "slink.h"

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic (crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (supply dip)";
    case ESP_RST_USB:       return "USB";
    default:                return "unknown";
    }
}

/* internal temperature sensor (die temperature, not ambient). Enabled only for the instant of the reading. */
static bool read_temp(float *c)
{
    static temperature_sensor_handle_t h;
    if (!h) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &h) != ESP_OK) { h = NULL; return false; }
    }
    if (temperature_sensor_enable(h) != ESP_OK) return false;
    esp_err_t e = temperature_sensor_get_celsius(h, c);
    temperature_sensor_disable(h);
    return e == ESP_OK;
}

static void jesc(char *dst, size_t n, const char *s)      /* keep JSON strings safe: drop quotes, backslashes, control chars */
{
    size_t k = 0;
    for (; *s && k + 1 < n; s++) dst[k++] = (*s == '"' || *s == '\\' || (unsigned char)*s < 32 || (unsigned char)*s > 126) ? '?' : *s;
    dst[k] = 0;
}

size_t stats_json(char *b, size_t sz)
{
    size_t n = 0;
    #define P(...) do { if (n < sz) n += (size_t)snprintf(b + n, sz - n, __VA_ARGS__); } while (0)

    esp_chip_info_t ci; esp_chip_info(&ci);
    const esp_app_desc_t *ad = esp_app_get_description();
    char fw[40], built[40], idf[40], ssid[40];
    jesc(fw, sizeof fw, ad->version); jesc(idf, sizeof idf, ad->idf_ver);
    char tb[40]; snprintf(tb, sizeof tb, "%s %s", ad->date, ad->time); jesc(built, sizeof built, tb);
    jesc(ssid, sizeof ssid, g_set.ssid);

    uint32_t edges = 0, syncs = 0, bits = 0, frames = 0;
    slink_stats(&edges, &syncs, &bits, &frames);
    esp_pm_config_t pm = {0};
    bool pm_ok = esp_pm_get_configuration(&pm) == ESP_OK;
    float t = 0; bool t_ok = read_temp(&t);

    char slot[20]; const char *ost; ota_slot_info(slot, sizeof slot, &ost);
    uint32_t fimg, fslot, fflash; ota_size_info(&fimg, &fslot, &fflash);
    P("{\"t\":\"stats\",\"inst\":%lld,\"slot\":\"%s\",\"ostate\":\"%s\",\"fimg\":%u,\"fslot\":%u,\"fflash\":%u,\"up\":%lld,\"reset\":\"%s\",\"chip\":\"%s\",\"rev\":%d,\"cores\":%d,\"fw\":\"%s\",\"built\":\"%s\",\"idf\":\"%s\",",
      (long long)ota_installed(), slot, ost, (unsigned)fimg, (unsigned)fslot, (unsigned)fflash, (long long)(esp_timer_get_time() / 1000000), reset_reason(), CONFIG_IDF_TARGET, ci.revision, ci.cores, fw, built, idf);
    if (t_ok) P("\"temp\":%.1f,", (double)t); else P("\"temp\":null,");
    P("\"heap\":%u,\"heapMin\":%u,\"heapBlock\":%u,\"heapTotal\":%u,\"tasks\":%u,",
      (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT),
      (unsigned)uxTaskGetNumberOfTasks());
    unsigned twt_ms = 0; int twt = net_twt_state(&twt_ms);
    P("\"cpu\":%u,\"cpuMax\":%d,\"cpuMin\":%d,\"eco\":%d,\"twt\":%d,\"twtMs\":%u,", (unsigned)esp_rom_get_cpu_ticks_per_us(),
      pm_ok ? pm.max_freq_mhz : 0, pm_ok ? pm.min_freq_mhz : 0, g_set.eco, twt, twt_ms);

    /* WiFi */
    wifi_ap_record_t ap;
    wifi_ps_type_t ps = WIFI_PS_NONE;
    int8_t txq = 0;
    wifi_bandwidth_t bw = WIFI_BW20;
    esp_wifi_get_ps(&ps); esp_wifi_get_max_tx_power(&txq); esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    P("\"ssid\":\"%s\",\"ps\":%d,\"txq\":%d,\"bw\":%d,", ssid, (int)ps, (int)txq, bw == WIFI_BW40 ? 40 : 20);
    if (net_have_ip() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) esp_netif_get_ip_info(nif, &ip);
        P("\"wifi\":{\"rssi\":%d,\"ch\":%d,\"ip\":\"" IPSTR "\"},", ap.rssi, ap.primary, IP2STR(&ip.ip));
    } else P("\"wifi\":null,");

    /* links */
    P("\"web\":%d,\"tcp\":%d,\"ble\":%d,\"bleConn\":%d,\"mqtt\":%d,\"mqttOk\":%d,",
      web_clients(), tcp_srv_clients(), g_set.ble, ble_nus_connected(), g_set.mqtt_uri[0] != 0, mqtt_bridge_connected());

    /* S-Link bus */
    uint32_t lng = 0, lmax = 0;
    slink_long_pulses(&lng, &lmax);
    P("\"bus\":{\"edges\":%u,\"syncs\":%u,\"bits\":%u,\"frames\":%u,\"level\":%d,\"long\":%u,\"longMax\":%u}}",
      (unsigned)edges, (unsigned)syncs, (unsigned)bits, (unsigned)frames, slink_line_level(), (unsigned)lng, (unsigned)lmax);
    #undef P
    return n < sz ? n : sz - 1;
}
