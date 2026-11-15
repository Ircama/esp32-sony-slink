/* UART over BLE GATT: Nordic UART Service (NUS) on NimBLE, peripheral role.
 *   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (write, phone -> device) ...0002     TX (notify, device -> phone) ...0003
 * Same text protocol as the USB port. Works with nRF Toolbox, "Serial Bluetooth Terminal", Web Bluetooth... */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/message_buffer.h"
#include "soc/soc_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_mac.h"
#include "net.h"
#include "settings.h"
#include "cli.h"
#include "state.h"

static const ble_uuid128_t k_svc = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t k_rx  = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t k_tx  = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static volatile bool s_running, s_synced;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_sub;
static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;
static char s_name[16];
static StreamBufferHandle_t s_sb;
static MessageBufferHandle_t s_mb;         /* JSON lines for the web page on GitHub Pages (@UI), each one sent whole between two chunks of text */
static TaskHandle_t s_tx_task, s_json_task;
static volatile bool s_ui;                 /* the client asked for the JSON frames of the portal ("@UI"): the Bluetooth page of the project */
static SemaphoreHandle_t s_stop_done;

bool ble_nus_running(void) { return s_running; }
const char *ble_nus_name(void) { return s_name; }
bool ble_nus_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

/* ---- output: CLI sink -> stream buffer -> notifications ---- */

static void ble_sink(const char *d, size_t n)
{
    if (s_sb && s_sub) { xStreamBufferSend(s_sb, d, n, 0); if (s_tx_task) xTaskNotifyGive(s_tx_task); }       /* drop when the phone cannot keep up */
}

/* send n bytes as notifications of at most MTU-3 bytes; false when the link is gone */
static bool send_all(const uint8_t *buf, size_t n)
{
    uint16_t conn = s_conn;
    if (!n || conn == BLE_HS_CONN_HANDLE_NONE || !s_sub) return true;
    size_t chunk = ble_att_mtu(conn);
    chunk = chunk > 3 ? chunk - 3 : 20;
    for (size_t off = 0; off < n; ) {
        size_t k = n - off < chunk ? n - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + off, (uint16_t)k);
        if (!om) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int rc = ble_gatts_notify_custom(conn, s_tx_handle, om);
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) { vTaskDelay(pdMS_TO_TICKS(15)); continue; }   /* mbuf consumed on failure too */
        if (rc != 0) return false;
        off += k;
        vTaskDelay(1);
    }
    return true;
}

static void tx_task(void *arg)
{
    static uint8_t jb[2100];
    uint8_t buf[244];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            size_t n = s_mb ? xMessageBufferReceive(s_mb, jb, sizeof jb, 0) : 0;      /* a JSON line goes out whole */
            if (n) { send_all(jb, n); continue; }
            n = xStreamBufferReceive(s_sb, buf, sizeof buf, 0);
            if (!n) break;
            send_all(buf, n);
        }
    }
}

/* ---- JSON frames for the web page (GitHub Pages "Bluetooth" page): the same {"t":...} frames the portal gets over its WebSocket, one per line ---- */

enum { J_STATE = 1, J_TABLES = 2, J_NET = 4, J_STATS = 8 };

static void json_line(char *b, size_t n, size_t cap)
{
    if (!n || n + 1 >= cap || !s_mb || !s_ui || s_conn == BLE_HS_CONN_HANDLE_NONE || !s_sub) return;
    b[n] = '\n';
    for (int i = 0; i < 40; i++) {                                    /* wait for room: never a partial line */
        if (xMessageBufferSend(s_mb, b, n + 1, 0) != 0) { if (s_tx_task) xTaskNotifyGive(s_tx_task); return; }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void json_task(void *arg)
{
    static char b[1900];
    for (;;) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, pdMS_TO_TICKS(5000)) != pdTRUE) bits = s_ui ? J_NET : 0;    /* every 5 s: the page's watchdog counts on a frame */
        if (!s_ui || !bits) continue;
        if (bits & J_STATE) {
            vTaskDelay(pdMS_TO_TICKS(60));                                    /* coalesce bursts of frames */
            { uint32_t more = 0; xTaskNotifyWait(0, 0xFFFFFFFF, &more, 0); bits |= more; }
            json_line(b, state_json(b, sizeof b - 2, cli_vol_pct(), cli_vol_x10()), sizeof b);
        }
        if (bits & J_TABLES) {
            json_line(b, state_presets_json(b, sizeof b - 2), sizeof b);
            json_line(b, cli_stations_json(b, sizeof b - 2), sizeof b);
            json_line(b, cli_favs_json(b, sizeof b - 2), sizeof b);
        }
        if (bits & J_NET) json_line(b, net_json_get(b, sizeof b - 2), sizeof b);
#if SOC_WIFI_SUPPORTED
        if (bits & J_STATS) json_line(b, stats_json(b, sizeof b - 2), sizeof b);
#endif
    }
}

void ble_nus_state_changed(void)                    /* the state changed: tell the Bluetooth page (called by net_notify_state) */
{
    if (s_ui && s_json_task) xTaskNotify(s_json_task, J_STATE, eSetBits);
}

/* ---- input: GATT write -> lines -> CLI ---- */

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static char line[200];
    static size_t len;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t tmp[244];
    if (n > sizeof tmp) n = sizeof tmp;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, n, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    for (uint16_t i = 0; i < n; i++) {
        char ch = (char)tmp[i];
        if (ch == '\r' || ch == '\n') {
            if (len && line[0] == '@') {                                   /* "@UI": send me the portal's JSON frames; "@GET <state|tables|net|stats>": send one now */
                line[len] = 0; len = 0;
                if (s_json_task) {
                    if (!strcmp(line, "@UI")) { s_ui = true; xTaskNotify(s_json_task, J_STATE | J_TABLES | J_NET, eSetBits); }
                    else if (!strncmp(line, "@GET ", 5)) {
                        const char *w = line + 5;
                        xTaskNotify(s_json_task, !strcmp(w, "stats") ? J_STATS : !strcmp(w, "net") ? J_NET : !strcmp(w, "state") ? J_STATE
                                    : (!strcmp(w, "stations") || !strcmp(w, "presets") || !strcmp(w, "favs") || !strcmp(w, "tables")) ? J_TABLES : 0, eSetBits);
                    }
                }
            } else if (len) { line[len] = 0; cli_submit(line); len = 0; }
        } else if (ch == 0x08 || ch == 0x7F) { if (len) len--; }
        else if ((unsigned char)ch >= 32 && len < sizeof line - 1) line[len++] = ch;
    }
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) { return 0; }

static const struct ble_gatt_svc_def k_gatt[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &k_svc.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &k_rx.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
          { .uuid = &k_tx.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY },
          { 0 } } },
    { 0 }
};

/* ---- GAP ---- */

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; s_sub = false; s_ui = false; }
        else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_sub = false; s_ui = false;
        if (s_running) advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) s_sub = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running) advertise();
        break;
    default: break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_running || !s_synced) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = (uint8_t)strlen(s_name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&k_svc;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (g_set.eco) { p.itvl_min = BLE_GAP_ADV_ITVL_MS(500); p.itvl_max = BLE_GAP_ADV_ITVL_MS(700); }   /* eco: advertise 5x less often (default 100 ms) */
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    advertise();
}

static void on_reset(int reason) { s_synced = false; }

static void host_task(void *arg)
{
    nimble_port_run();                       /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_nus_start(void)
{
    if (s_running) return;
    if (!s_sb) s_sb = xStreamBufferCreate(2048, 1);
    if (!s_mb) s_mb = xMessageBufferCreate(4600);
    if (!s_json_task) xTaskCreate(json_task, "slink_blej", 4096, NULL, 3, &s_json_task);
    if (!s_stop_done) s_stop_done = xSemaphoreCreateBinary();
    if (!s_tx_task) xTaskCreate(tx_task, "slink_ble", 3072, NULL, 4, &s_tx_task);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof s_name, "S-Link-%02X%02X", mac[4], mac[5]);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_sub = false;
    s_synced = false;
    if (nimble_port_init() != ESP_OK) return;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(k_gatt);
    ble_gatts_add_svcs(k_gatt);
    ble_svc_gap_device_name_set(s_name);
    s_running = true;
    cli_sink_add(ble_sink);
    nimble_port_freertos_init(host_task);
}

void ble_nus_stop(void)
{
    if (!s_running) return;
    s_running = false;
    cli_sink_remove(ble_sink);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 20 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
    ble_gap_adv_stop();
    nimble_port_stop();                      /* host_task returns and deinitialises its FreeRTOS part */
    vTaskDelay(pdMS_TO_TICKS(100));
    nimble_port_deinit();
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_sub = false;
    s_synced = false;
    s_ui = false;
    if (s_sb) xStreamBufferReset(s_sb);
    if (s_mb) xMessageBufferReset(s_mb);
}
