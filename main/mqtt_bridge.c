/* MQTT bridge with Home Assistant MQTT discovery.
 *   <base>/avail   online|offline (retained, last will)
 *   <base>/state   JSON state (same object as the web portal)
 *   <base>/cmd     any CLI line, e.g.  PWRON  |  INPUT MD  |  VOL UP 5  |  SFNAME Jazz Club
 * Entities (HA discovery): power, mute, input, sound field, volume (estimate), volume +/-, tuner sensors. */
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_mac.h"
#include "net.h"
#include "cli.h"
#include "settings.h"
#include "decode.h"
#include "state.h"

static esp_mqtt_client_handle_t s_cl;
static volatile bool s_conn;
static char s_id[8], s_base[24], s_last[1800];

bool mqtt_bridge_running(void) { return s_cl != NULL; }
bool mqtt_bridge_connected(void) { return s_conn; }

static void pub(const char *topic, const char *payload, int retain)
{
    esp_mqtt_client_publish(s_cl, topic, payload, 0, 1, retain);
}

/* one discovery message. extra = additional JSON members (no leading comma) */
static void disc(const char *comp, const char *obj, const char *name, const char *extra)
{
    static char topic[96], pl[1800];
    snprintf(topic, sizeof topic, "homeassistant/%s/slink_%s/%s/config", comp, s_id, obj);
    snprintf(pl, sizeof pl,
        "{\"name\":\"%s\",\"unique_id\":\"slink_%s_%s\",\"availability_topic\":\"%s/avail\","
        "\"device\":{\"identifiers\":[\"slink_%s\"],\"name\":\"Sony S-Link amplifier\",\"manufacturer\":\"Sony\","
        "\"model\":\"S-Link / Control-A1 bridge\"},%s}",
        name, s_id, obj, s_base, s_id, extra);
    pub(topic, pl, 1);
}

static void publish_discovery(void)
{
    char x[1200];
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ 'ON' if value_json.power == 1 else 'OFF' }}\","
        "\"command_topic\":\"%s/cmd\",\"payload_on\":\"PWRON\",\"payload_off\":\"PWROFF\",\"icon\":\"mdi:power\"", s_base, s_base);
    disc("switch", "power", "Power", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ 'ON' if value_json.mute == 1 else 'OFF' }}\","
        "\"command_topic\":\"%s/cmd\",\"payload_on\":\"MUTE\",\"payload_off\":\"UNMUTE\",\"icon\":\"mdi:volume-mute\"", s_base, s_base);
    disc("switch", "mute", "Mute", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.inName }}\","
        "\"command_topic\":\"%s/cmd\",\"command_template\":\"INPUT {{ value }}\","
        "\"options\":[\"TUNER\",\"PHONO\",\"CD\",\"MD\",\"DAT\",\"TAPE\",\"VIDEO1\",\"VIDEO2\",\"VIDEO3\",\"DVD\",\"SAT\",\"TV\"],"
        "\"icon\":\"mdi:import\"", s_base, s_base);
    disc("select", "input", "Input", x);
    char opts[900];
    size_t k = 0;
    for (unsigned i = 0; i < 30 && k < sizeof opts - 40; i++)
        k += (size_t)snprintf(opts + k, sizeof opts - k, "%s\"%s\"", i ? "," : "", slink_sf_name(i));
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.sfName }}\","
        "\"command_topic\":\"%s/cmd\",\"command_template\":\"SFNAME {{ value }}\",\"options\":[%s],\"icon\":\"mdi:surround-sound\"",
        s_base, s_base, opts);
    disc("select", "soundfield", "Sound field", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.vol if value_json.vol >= 0 else none }}\","
        "\"command_topic\":\"%s/cmd\",\"command_template\":\"VOL SET {{ value | int }}\",\"min\":0,\"max\":100,\"step\":1,"
        "\"unit_of_measurement\":\"%%\",\"mode\":\"slider\",\"icon\":\"mdi:volume-high\"", s_base, s_base);
    disc("number", "volume", "Volume (estimate)", x);
    snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"VOL UP 5\",\"icon\":\"mdi:volume-plus\"", s_base);
    disc("button", "volup", "Volume up", x);
    snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"VOL DOWN 5\",\"icon\":\"mdi:volume-minus\"", s_base);
    disc("button", "voldown", "Volume down", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ (value_json.tuner.f / 100) | round(2) if value_json.tuner and value_json.tuner.band == 'FM' else none }}\","
        "\"unit_of_measurement\":\"MHz\",\"icon\":\"mdi:radio\"", s_base);
    disc("sensor", "fm", "FM frequency", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.tuner.name if value_json.tuner else none }}\","
        "\"icon\":\"mdi:radio-tower\"", s_base);
    disc("sensor", "station", "Station name", x);
    snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.tuner.level if value_json.tuner else none }}\","
        "\"icon\":\"mdi:signal\"", s_base);
    disc("sensor", "signal", "Signal level", x);
}

static void on_data(const char *t, int tl, const char *d, int dl)
{
    char cmdt[40];
    snprintf(cmdt, sizeof cmdt, "%s/cmd", s_base);
    if ((int)strlen(cmdt) != tl || strncmp(t, cmdt, (size_t)tl)) return;
    char line[200];
    if (dl <= 0 || dl >= (int)sizeof line) return;
    memcpy(line, d, (size_t)dl);
    line[dl] = 0;
    if (!strncmp(line, "SFNAME ", 7)) {                    /* Home Assistant select: sound field by name */
        for (unsigned i = 0; i < 30; i++)
            if (!strcmp(slink_sf_name(i), line + 7)) {
                snprintf(line, sizeof line, i == 0 ? "SF OFF" : "SF %u", i);
                break;
            }
    }
    cli_submit(line);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_conn = true;
        s_last[0] = 0;
        char t[48];
        snprintf(t, sizeof t, "%s/avail", s_base);
        pub(t, "online", 1);
        snprintf(t, sizeof t, "%s/cmd", s_base);
        esp_mqtt_client_subscribe(s_cl, t, 1);
        publish_discovery();
        net_notify_state();                                   /* publish the current state */
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_conn = false;
    } else if (id == MQTT_EVENT_DATA && e->data_len > 0 && e->current_data_offset == 0 && e->data_len == e->total_data_len) {
        on_data(e->topic, e->topic_len, e->data, e->data_len);
    }
}

void mqtt_bridge_push(const char *json, size_t n)
{
    if (!s_cl || !s_conn || n >= sizeof s_last) return;
    if (strlen(s_last) == n && !memcmp(s_last, json, n)) return;     /* unchanged */
    memcpy(s_last, json, n);
    s_last[n] = 0;
    char t[48];
    snprintf(t, sizeof t, "%s/state", s_base);
    esp_mqtt_client_publish(s_cl, t, json, (int)n, 0, 0);
}

void mqtt_bridge_start(void)
{
    if (s_cl || !g_set.mqtt_uri[0]) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof s_id, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof s_base, "slink/%s", s_id);
    char will[48];
    snprintf(will, sizeof will, "%s/avail", s_base);
    esp_mqtt_client_config_t c = {0};
    c.broker.address.uri = g_set.mqtt_uri;
    if (g_set.mqtt_user[0]) c.credentials.username = g_set.mqtt_user;
    if (g_set.mqtt_pass[0]) c.credentials.authentication.password = g_set.mqtt_pass;
    c.session.last_will.topic = will;
    c.session.last_will.msg = "offline";
    c.session.last_will.qos = 1;
    c.session.last_will.retain = 1;
    c.session.keepalive = 30;
    c.network.reconnect_timeout_ms = 5000;
    c.buffer.size = 2048;
    s_cl = esp_mqtt_client_init(&c);
    if (!s_cl) return;
    esp_mqtt_client_register_event(s_cl, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_cl);
}

void mqtt_bridge_stop(void)
{
    if (!s_cl) return;
    esp_mqtt_client_handle_t c = s_cl;
    if (s_conn) {
        char t[48];
        snprintf(t, sizeof t, "%s/avail", s_base);
        esp_mqtt_client_publish(c, t, "offline", 0, 1, 1);
    }
    s_cl = NULL;
    s_conn = false;
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
}
