/* WiFi station + mDNS + service supervisor. Driven only by the persistent settings. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_HE_SUPPORT
#include "esp_wifi_he.h"
#endif
#include "mdns.h"
#include "lwip/sockets.h"
#include "net.h"
#include "settings.h"
#include "state.h"
#include "cli.h"

#define NB_APPLY  1u
#define NB_STATE  2u
#define NB_WS     4u
#define NB_NET    8u

static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_rc_timer;
static bool s_wifi_inited, s_wifi_started, s_have_ip, s_mdns_inited, s_mdns_web, s_mdns_app;
static char s_ap_ssid[33], s_ap_pass[65];       /* credentials currently applied to the driver */
static char s_mqtt_applied[96 + 32 + 64 + 3];   /* uri/user/pass the MQTT bridge was started with */
static esp_ip4_addr_t s_ip;
static char s_dns_name[64];                     /* name the router's DNS gives to our IP (reverse lookup), e.g. slink.fritz.box */
static int s_dns_tries;
static int s_retry;                             /* consecutive failed connection attempts (reconnect back-off) */
static int s_eco_applied = -1;                  /* eco profile the WiFi driver was configured with */

bool net_have_ip(void) { return s_have_ip; }

/* Low-power profile. CPU frequency scaling only: NO automatic light sleep, because the S-Link receiver time-stamps the bus
 * edges in a GPIO interrupt and a light-sleep wake-up (hundreds of microseconds) would corrupt the 600/1200/2400 us bit timing.
 * The big saving is the WiFi radio (modem sleep), configured in wifi_apply(). */
static void power_apply(void)
{
    esp_pm_config_t pm = { .max_freq_mhz = g_set.eco ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                           .min_freq_mhz = g_set.eco ? 40 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                           .light_sleep_enable = false };
    esp_pm_configure(&pm);                      /* ESP_ERR_NOT_SUPPORTED when CONFIG_PM_ENABLE is off: harmless */
}

/* ---- WiFi 6 Target Wake Time -------------------------------------------------------------------------------------
 * With an individual TWT agreement the radio sleeps for the whole agreed interval instead of waking for every beacon:
 * the biggest idle saving a station can get. Requested after the connection when the eco profile is on and the access
 * point is a WiFi 6 one. The interval trades power for command latency: 500 ms here, awake for 20 ms each time. */
#if SOC_WIFI_HE_SUPPORT
static volatile int s_twt_state = NET_TWT_OFF;
#else
static volatile int s_twt_state = NET_TWT_NO_CHIP;
#endif
static volatile unsigned s_twt_ms;

int net_twt_state(unsigned *interval_ms)
{
    if (interval_ms) *interval_ms = s_twt_ms;
    return s_twt_state;
}

#if SOC_WIFI_HE_SUPPORT
#define TWT_WAKE_INVL_MANT  15625u         /* interval = mantissa x 2^exponent microseconds = 15625 x 32 = 500 ms */
#define TWT_WAKE_INVL_EXPN  5
#define TWT_MIN_WAKE_DURA   78             /* in units of 256 us: ~20 ms awake per interval */

static void twt_request(void)
{
    s_twt_ms = 0;
    if (!g_set.eco) { s_twt_state = NET_TWT_OFF; return; }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || !ap.phy_11ax) { s_twt_state = NET_TWT_NO_AP; return; }
    wifi_twt_setup_config_t c = {
        .setup_cmd = TWT_REQUEST, .trigger = 0, .flow_type = 0 /* announced */, .flow_id = 0,
        .wake_invl_expn = TWT_WAKE_INVL_EXPN, .wake_invl_mant = TWT_WAKE_INVL_MANT, .min_wake_dura = TWT_MIN_WAKE_DURA,
        .twt_id = 0, .timeout_time_ms = 5000,
    };
    s_twt_state = esp_wifi_sta_itwt_setup(&c) == ESP_OK ? NET_TWT_PENDING : NET_TWT_REFUSED;   /* the result arrives as an event */
}
#endif

static void notify(uint32_t bits)
{
    if (s_task) xTaskNotify(s_task, bits, eSetBits);
}

void net_notify_state(void) { notify(NB_STATE); }
void net_notify_ws_client(void) { notify(NB_WS); }

static void rc_timer_cb(void *arg)
{
    if (s_wifi_started) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_have_ip = false;
#if SOC_WIFI_HE_SUPPORT
            s_twt_state = NET_TWT_OFF; s_twt_ms = 0;        /* agreements die with the association */
#endif
            if (s_wifi_started) {               /* back off 3, 6, 12, 24, 48, 60 s: an absent AP must not keep the radio scanning */
                uint64_t us = 3000000ull << (s_retry < 5 ? s_retry : 5);
                if (us > 60000000ull) us = 60000000ull;
                s_retry++;
                esp_timer_start_once(s_rc_timer, us);
            }
        }
#if SOC_WIFI_HE_SUPPORT
        else if (id == WIFI_EVENT_ITWT_SETUP) {
            const wifi_event_sta_itwt_setup_t *s = data;
            if (s->status == 1) {                           /* 1 = success (see the ESP-IDF itwt example) */
                s_twt_ms = (unsigned)(((uint64_t)s->config.wake_invl_mant << s->config.wake_invl_expn) / 1000u);
                s_twt_state = NET_TWT_ACTIVE;
            } else { s_twt_ms = 0; s_twt_state = NET_TWT_REFUSED; }
        } else if (id == WIFI_EVENT_ITWT_TEARDOWN) { s_twt_state = NET_TWT_OFF; s_twt_ms = 0; }
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ip = ((ip_event_got_ip_t *)data)->ip_info.ip;
        s_have_ip = true;
        s_retry = 0;
        s_dns_name[0] = 0; s_dns_tries = 0;
#if SOC_WIFI_HE_SUPPORT
        twt_request();
#endif
        notify(NB_APPLY);
    }
}

static void wifi_ensure_init(void)
{
    if (s_wifi_inited) return;
    esp_netif_init();
    esp_event_loop_create_default();            /* ESP_ERR_INVALID_STATE if it already exists: fine */
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     /* credentials live in our own NVS keys */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);   /* 20 MHz channel: less radio time and current than HT40 */
#if SOC_WIFI_HE_SUPPORT
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);   /* needed for TWT */
#endif
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t ta = { .callback = rc_timer_cb, .name = "wifi_rc" };
    esp_timer_create(&ta, &s_rc_timer);
    s_wifi_inited = true;
}

/* WiFi power parameters: the explicit settings (TXPOWER / WIFIPS / LISTEN) win, "auto" follows the eco profile */
static int eff_txq(void) { return g_set.txp ? g_set.txp * 4 : (g_set.eco ? 44 : 80); }   /* quarter dBm: 11 dBm in eco (plenty at home), 20 otherwise */
static wifi_ps_type_t eff_ps(void)
{
    switch (g_set.wps) {
    case 1: return WIFI_PS_NONE;
    case 2: return WIFI_PS_MIN_MODEM;
    case 3: return WIFI_PS_MAX_MODEM;
    default: return g_set.eco ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    }
}
static int eff_listen(void) { return g_set.lis ? g_set.lis : (g_set.eco ? 3 : 0); }
static int pw_key(void) { return (int)g_set.eco | (g_set.txp << 1) | (g_set.wps << 8) | (g_set.lis << 10); }

static void wifi_apply(bool want)
{
    if (want) {
        wifi_ensure_init();
        bool changed = strcmp(s_ap_ssid, g_set.ssid) || strcmp(s_ap_pass, g_set.pass) || s_eco_applied != pw_key();
        if (changed) s_retry = 0;
        wifi_config_t wc = {0};
        wc.sta.listen_interval = eff_listen();   /* eco: wake for every 3rd beacon (~300 ms) instead of every DTIM */
        strncpy((char *)wc.sta.ssid, g_set.ssid, sizeof wc.sta.ssid - 1);
        strncpy((char *)wc.sta.password, g_set.pass, sizeof wc.sta.password - 1);
        wc.sta.threshold.authmode = g_set.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_netif_set_hostname(s_netif, g_set.host);
        if (!s_wifi_started) {
            esp_wifi_start();                   /* STA_START event -> connect */
            s_wifi_started = true;
        } else if (changed) {
            esp_timer_stop(s_rc_timer);
            esp_wifi_disconnect();              /* DISCONNECTED event -> reconnect with the new credentials */
        }
        esp_wifi_set_ps(eff_ps());
        esp_wifi_set_max_tx_power(eff_txq());
        s_eco_applied = pw_key();
        strcpy(s_ap_ssid, g_set.ssid);
        strcpy(s_ap_pass, g_set.pass);
    } else if (s_wifi_started) {
        s_wifi_started = false;
        s_have_ip = false;
        esp_timer_stop(s_rc_timer);
        esp_wifi_stop();
        s_ap_ssid[0] = s_ap_pass[0] = 0;
    }
}

static void mdns_apply(bool want_web, bool want_app)
{
    if (!s_have_ip) return;
    if (!s_mdns_inited) {
        if (mdns_init() != ESP_OK) return;
        s_mdns_inited = true;
    }
    mdns_hostname_set(g_set.host);
    mdns_instance_name_set("S-Link bridge");
    if (want_web && !s_mdns_web) { mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0); s_mdns_web = true; }
    if (!want_web && s_mdns_web) { mdns_service_remove("_http", "_tcp"); s_mdns_web = false; }
    if (want_app && !s_mdns_app) { mdns_service_add(NULL, "_slink", "_tcp", 2323, NULL, 0); s_mdns_app = true; }
    if (!want_app && s_mdns_app) { mdns_service_remove("_slink", "_tcp"); s_mdns_app = false; }
}

void net_apply(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    power_apply();
    if (g_set.ble) ble_nus_start(); else ble_nus_stop();

    bool want_wifi = (g_set.web || g_set.app) && g_set.ssid[0];
    wifi_apply(want_wifi);

    bool up = want_wifi && s_have_ip;
    if (up && g_set.web) web_start(); else web_stop();
    if (up && g_set.app) tcp_srv_start(); else tcp_srv_stop();

    char snap[sizeof s_mqtt_applied];
    snprintf(snap, sizeof snap, "%s\n%s\n%s", g_set.mqtt_uri, g_set.mqtt_user, g_set.mqtt_pass);
    if (up && g_set.app && g_set.mqtt_uri[0]) {
        if (mqtt_bridge_running() && strcmp(snap, s_mqtt_applied)) mqtt_bridge_stop();   /* settings changed: restart */
        if (!mqtt_bridge_running()) { mqtt_bridge_start(); strcpy(s_mqtt_applied, snap); }
    } else mqtt_bridge_stop();

    mdns_apply(up && g_set.web, up && g_set.app);
    xSemaphoreGive(s_mtx);
    notify(NB_NET);
}

void net_status(char *b, size_t n)
{
    size_t k = 0;
    #define P(...) do { if (k < n) k += (size_t)snprintf(b + k, n - k, __VA_ARGS__); } while (0)
    if (!g_set.ssid[0]) P("WIFI  no network set (WIFI <ssid> <password>)\r\n");
    else if (!(g_set.web || g_set.app)) P("WIFI  \"%s\" saved, idle (turns on with WEB ON or APP ON)\r\n", g_set.ssid);
    else if (s_have_ip) {
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        P("WIFI  \"%s\" connected, IP " IPSTR ", RSSI %d dBm, host %s.local%s%s\r\n", g_set.ssid, IP2STR(&s_ip), rssi, g_set.host, s_dns_name[0] ? ", DNS name " : "", s_dns_name);
    } else P("WIFI  \"%s\" connecting...\r\n", g_set.ssid);
    P("BLE   %s%s\r\n", g_set.ble ? "ON" : "OFF", g_set.ble ? (ble_nus_connected() ? ", client connected" : ", advertising as S-Link-xxxx") : "");
    if (g_set.web) {
        if (s_have_ip) P("WEB   ON  http://" IPSTR "/  http://%s.local/\r\n", IP2STR(&s_ip), g_set.host);
        else P("WEB   ON  (waiting for WiFi)\r\n");
    } else P("WEB   OFF\r\n");
    if (g_set.app) {
        P("APP   ON  tcp %d client(s) on port 2323; MQTT %s", tcp_srv_clients(),
          !g_set.mqtt_uri[0] ? "not configured (MQTT <uri>)" : mqtt_bridge_connected() ? "connected" : "connecting...");
        if (g_set.mqtt_uri[0]) P(" (%s)", g_set.mqtt_uri);
        P("\r\n");
    } else P("APP   OFF\r\n");
    #undef P
    while (k >= 2 && b[k - 1] == '\n') { b[--k] = 0; if (k && b[k - 1] == '\r') b[--k] = 0; }   /* no trailing CRLF */
}

static size_t jstr(char *b, size_t sz, const char *s)   /* append a JSON string literal */
{
    size_t n = 0;
    if (n < sz) b[n++] = '"';
    for (; *s && n + 7 < sz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = (char)c; }
        else if (c < 32 || c > 126) b[n++] = '?';
        else b[n++] = (char)c;
    }
    if (n < sz) b[n++] = '"';
    if (n < sz) b[n] = 0;
    return n;
}

static size_t net_json(char *b, size_t sz)
{
    size_t n = 0;
    #define P(...) do { if (n < sz) n += (size_t)snprintf(b + n, sz - n, __VA_ARGS__); } while (0)
    #define J(str) do { if (n < sz) n += jstr(b + n, sz - n, (str)); } while (0)
    const char *wifi = !g_set.ssid[0] ? "none" : !(g_set.web || g_set.app) ? "idle" : s_have_ip ? "connected" : "connecting";
    wifi_ap_record_t ap;
    int rssi = s_have_ip && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    P("{\"t\":\"net\",\"ble\":%d,\"web\":%d,\"app\":%d,\"eco\":%d,\"bleConn\":%d,\"bleName\":", g_set.ble, g_set.web, g_set.app, g_set.eco, ble_nus_connected());
    J(ble_nus_name());
    P(",\"wifi\":\"%s\",\"rssi\":%d,\"ip\":\"", wifi, rssi);
    if (s_have_ip) P(IPSTR, IP2STR(&s_ip));
    P("\",\"dns\":"); J(s_dns_name);
    P(",\"ssid\":"); J(g_set.ssid);
    P(",\"host\":"); J(g_set.host);
    P(",\"mqtt\":"); J(g_set.mqtt_uri);
    P(",\"mqttUser\":"); J(g_set.mqtt_user);
    P(",\"mqttOk\":%d,\"tcp\":%d}", mqtt_bridge_connected(), tcp_srv_clients());
    #undef P
    #undef J
    return n < sz ? n : sz - 1;
}

/* decode a (possibly compressed) DNS name starting at off into dotted text; returns false on malformed data */
static bool dns_name(const uint8_t *m, int len, int off, char *out, size_t sz)
{
    size_t o = 0; int jumps = 0;
    while (off < len) {
        int l = m[off];
        if (l == 0) { if (o) out[o - 1] = 0; else out[0] = 0; return o > 0; }
        if ((l & 0xC0) == 0xC0) { if (off + 1 >= len || ++jumps > 8) return false; off = ((l & 0x3F) << 8) | m[off + 1]; continue; }
        if (off + 1 + l > len || o + (size_t)l + 1 >= sz) return false;
        memcpy(out + o, m + off + 1, (size_t)l); o += (size_t)l; out[o++] = '.';
        off += 1 + l;
    }
    return false;
}

/* reverse DNS (PTR) of our own address, asked to the DNS server given by DHCP: lwIP has no getnameinfo(), so the query is built here */
static void ptr_lookup(void)
{
    esp_netif_dns_info_t di;
    if (!s_netif || esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &di) != ESP_OK || di.ip.type != ESP_IPADDR_TYPE_V4 || !di.ip.u_addr.ip4.addr) return;
    uint8_t q[96], r[512];
    int n = 0;
    uint32_t a = s_ip.addr;                         /* network order: first octet in the low byte */
    q[n++] = 0x5A; q[n++] = 0x17; q[n++] = 0x01; q[n++] = 0x00; q[n++] = 0; q[n++] = 1; memset(q + n, 0, 6); n += 6;
    for (int i = 3; i >= 0; i--) {
        char d[4]; int l = snprintf(d, sizeof d, "%u", (unsigned)((a >> (8 * i)) & 0xFF));
        q[n++] = (uint8_t)l; memcpy(q + n, d, (size_t)l); n += l;
    }
    static const char tail[] = "\x07in-addr\x04" "arpa";
    memcpy(q + n, tail, sizeof tail - 1); n += (int)sizeof tail - 1;
    q[n++] = 0; q[n++] = 0; q[n++] = 12; q[n++] = 0; q[n++] = 1;          /* QTYPE PTR, QCLASS IN */
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return;
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = di.ip.u_addr.ip4.addr };
    int len = -1;
    if (sendto(fd, q, (size_t)n, 0, (struct sockaddr *)&sa, sizeof sa) == n) len = recv(fd, r, sizeof r, 0);
    close(fd);
    if (len < 12 || r[0] != 0x5A || r[1] != 0x17 || (r[3] & 0x0F) != 0) return;
    int an = (r[6] << 8) | r[7], off = 12;
    while (off < len && r[off]) off += (r[off] & 0xC0) == 0xC0 ? 2 : 1 + r[off];     /* question name */
    off += 5;                                                                           /* root label + QTYPE + QCLASS */
    for (int i = 0; i < an && off + 12 <= len; i++) {
        while (off < len && r[off] && (r[off] & 0xC0) != 0xC0) off += 1 + r[off];
        off += (off < len && (r[off] & 0xC0) == 0xC0) ? 2 : 1;
        if (off + 10 > len) return;
        int type = (r[off] << 8) | r[off + 1], rl = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 12 && dns_name(r, len, off, s_dns_name, sizeof s_dns_name)) return;
        off += rl;
    }
}

static void net_task(void *arg)
{
    static char buf[1800];
    for (;;) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, pdMS_TO_TICKS(5000)) != pdTRUE) bits = NB_NET;   /* periodic refresh */
        if (bits & NB_APPLY) { net_apply(); bits |= NB_NET; }
        if (bits & (NB_STATE | NB_WS)) {
            vTaskDelay(pdMS_TO_TICKS(60));                          /* coalesce bursts of frames */
            { uint32_t more = 0; xTaskNotifyWait(0, 0xFFFFFFFF, &more, 0); bits |= more; }
            size_t n = state_json(buf, sizeof buf, cli_vol_pct(), cli_vol_x10());
            web_push(buf, n);
            mqtt_bridge_push(buf, n);
            if (bits & NB_WS) {                                      /* new web client: send it the tables too */
                n = state_presets_json(buf, sizeof buf);
                web_push(buf, n);
                n = cli_stations_json(buf, sizeof buf);
                web_push(buf, n);
                bits |= NB_NET;
            }
        }
        if (s_have_ip && !s_dns_name[0] && s_dns_tries < 4) {          /* the name is not pushed by anyone: ask for it, a few times after getting the IP */
            s_dns_tries++; ptr_lookup();
            if (s_dns_name[0]) bits |= NB_NET;
        }
        if ((bits & NB_NET) && web_running()) {
            size_t n = net_json(buf, sizeof buf);
            web_push(buf, n);
        }
    }
}

void net_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    state_set_listener(net_notify_state);
    xTaskCreate(net_task, "slink_net", 6144, NULL, 4, &s_task);
    net_apply();
}
