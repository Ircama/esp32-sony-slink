#include <string.h>
#include "settings.h"
#include "nvs.h"
#include "soc/soc_caps.h"

settings_t g_set;

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t sz, const char *def)
{
    size_t n = sz;
    if (nvs_get_str(h, key, dst, &n) != ESP_OK) { strncpy(dst, def, sz - 1); dst[sz - 1] = 0; }
}

void settings_load(void)
{
    memset(&g_set, 0, sizeof g_set);
    g_set.eco = true;
#if !SOC_WIFI_SUPPORTED
    g_set.ble = true;                   /* no WiFi on this chip: BLE is the only wireless interface, on by default */
#endif
    g_set.vol_up = 420; g_set.vol_down = 445; g_set.vol_margin = 10;
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READONLY, &h) != ESP_OK) { strcpy(g_set.host, "slink"); return; }
    uint8_t v;
    if (nvs_get_u8(h, "ble", &v) == ESP_OK) g_set.ble = v;
    if (nvs_get_u8(h, "web", &v) == ESP_OK) g_set.web = v;
    if (nvs_get_u8(h, "app", &v) == ESP_OK) g_set.app = v;
    if (nvs_get_u8(h, "eco", &v) == ESP_OK) g_set.eco = v;
    get_str(h, "ssid", g_set.ssid, sizeof g_set.ssid, "");
    get_str(h, "pass", g_set.pass, sizeof g_set.pass, "");
    get_str(h, "host", g_set.host, sizeof g_set.host, "slink");
    get_str(h, "muri", g_set.mqtt_uri, sizeof g_set.mqtt_uri, "");
    get_str(h, "muser", g_set.mqtt_user, sizeof g_set.mqtt_user, "");
    get_str(h, "mpass", g_set.mqtt_pass, sizeof g_set.mqtt_pass, "");
    uint16_t w;
    if (nvs_get_u8(h, "pad", &v) == ESP_OK && v <= 2) g_set.pad = v;
    if (nvs_get_u16(h, "vup", &w) == ESP_OK && w >= 50 && w <= 2000) g_set.vol_up = w;
    if (nvs_get_u16(h, "vdn", &w) == ESP_OK && w >= 50 && w <= 2000) g_set.vol_down = w;
    if (nvs_get_u16(h, "vmg", &w) == ESP_OK && w <= 200) g_set.vol_margin = w;
    if (nvs_get_u8(h, "txp", &v) == ESP_OK && (v == 0 || (v >= 2 && v <= 20))) g_set.txp = v;
    if (nvs_get_u8(h, "wps", &v) == ESP_OK && v <= 3) g_set.wps = v;
    if (nvs_get_u8(h, "lis", &v) == ESP_OK && v <= 10) g_set.lis = v;
    get_str(h, "tapp", g_set.title_app, sizeof g_set.title_app, "");
    get_str(h, "tpg", g_set.title_page, sizeof g_set.title_page, "");
    get_str(h, "ourl", g_set.ota_url, sizeof g_set.ota_url, "");
    get_str(h, "otok", g_set.ota_token, sizeof g_set.ota_token, "");
    get_str(h, "opw", g_set.ota_pass, sizeof g_set.ota_pass, "");
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ble", g_set.ble);
    nvs_set_u8(h, "web", g_set.web);
    nvs_set_u8(h, "app", g_set.app);
    nvs_set_u8(h, "eco", g_set.eco);
    nvs_set_str(h, "ssid", g_set.ssid);
    nvs_set_str(h, "pass", g_set.pass);
    nvs_set_str(h, "host", g_set.host);
    nvs_set_str(h, "muri", g_set.mqtt_uri);
    nvs_set_str(h, "muser", g_set.mqtt_user);
    nvs_set_str(h, "mpass", g_set.mqtt_pass);
    nvs_set_u8(h, "pad", g_set.pad);
    nvs_set_u16(h, "vup", g_set.vol_up);
    nvs_set_u16(h, "vdn", g_set.vol_down);
    nvs_set_u16(h, "vmg", g_set.vol_margin);
    nvs_set_u8(h, "txp", g_set.txp);
    nvs_set_u8(h, "wps", g_set.wps);
    nvs_set_u8(h, "lis", g_set.lis);
    nvs_set_str(h, "tapp", g_set.title_app);
    nvs_set_str(h, "tpg", g_set.title_page);
    nvs_set_str(h, "ourl", g_set.ota_url);
    nvs_set_str(h, "otok", g_set.ota_token);
    nvs_set_str(h, "opw", g_set.ota_pass);
    nvs_commit(h);
    nvs_close(h);
}
