/* Over-the-air update, see ota.h. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include <time.h>
#include "settings.h"
#include "cli.h"
#include "net.h"
#include "ota.h"

#ifndef CONFIG_SLINK_OTA_GITHUB_REPO
#define CONFIG_SLINK_OTA_GITHUB_REPO "Ircama/esp32-sony-slink"
#endif
#define NONCE_LIFE_US (120LL * 1000000)
#define HEAD_MIN 320                           /* image header + first segment header + application description */

static volatile bool s_busy;                   /* one update at a time (pull task or push request) */
static char s_nonce[33];
static int64_t s_nonce_at;

static bool busy_take(void) { return !__atomic_exchange_n(&s_busy, true, __ATOMIC_SEQ_CST); }
static void busy_free(void) { s_busy = false; }

/* ---- SHA-256 (only for the password proof: written here to stay independent of the mbedTLS API of each IDF version) ---- */
static void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t blk[128];                          /* the message (< 110 bytes here) plus the padding fits in 2 blocks */
    size_t nb = (len + 9 + 63) / 64;
    memset(blk, 0, sizeof blk);
    memcpy(blk, msg, len); blk[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) blk[nb * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    for (size_t b = 0; b < nb; b++) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[b*64+i*4] << 24 | (uint32_t)blk[b*64+i*4+1] << 16 | (uint32_t)blk[b*64+i*4+2] << 8 | blk[b*64+i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ROR(e,6)^ROR(e,11)^ROR(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
            uint32_t t2 = (ROR(a,2)^ROR(a,13)^ROR(a,22)) + ((a&bb)^(a&c)^(bb&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=bb; bb=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
#undef ROR
    for (int i = 0; i < 8; i++) { out[i*4] = (uint8_t)(h[i] >> 24); out[i*4+1] = (uint8_t)(h[i] >> 16); out[i*4+2] = (uint8_t)(h[i] >> 8); out[i*4+3] = (uint8_t)h[i]; }
}

static void hex(const uint8_t *b, size_t n, char *o) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]); }

/* the proof expected from the client: sha256 hex of "<nonce>:<password>" */
static void proof(const char *nonce, char out[65])
{
    char m[96]; uint8_t d[32];
    int n = snprintf(m, sizeof m, "%s:%s", nonce, g_set.ota_pass);
    sha256((const uint8_t *)m, (size_t)n, d);
    hex(d, 32, out);
}

static void new_nonce(void)
{
    uint8_t r[16]; esp_fill_random(r, sizeof r);
    hex(r, sizeof r, s_nonce); s_nonce_at = esp_timer_get_time();
}

/* ---- common helpers ---- */

static void wifi_fast(bool on)                 /* the transfer crawls with the modem asleep (eco): no power save while it runs */
{
    static wifi_ps_type_t saved = WIFI_PS_MIN_MODEM; static bool have;
    if (on) { if (!have && esp_wifi_get_ps(&saved) == ESP_OK) { have = true; esp_wifi_set_ps(WIFI_PS_NONE); } }
    else if (have) { esp_wifi_set_ps(saved); have = false; }
}

static void reboot_task(void *a) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }
static void reboot_soon(void) { xTaskCreate(reboot_task, "ota_rst", 2048, NULL, 5, NULL); }

static const char *state_name(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return "undefined";
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

/* ---- installation date: the device has no clock of its own, so it uses SNTP when it can, else the time sent by the pusher ---- */

static int64_t now_epoch(void) { time_t t = time(NULL); return t > 1700000000 ? (int64_t)t : 0; }

static void stamp(const esp_partition_t *p, int64_t hint)
{
    int64_t t = now_epoch(); if (!t) t = hint;
    nvs_handle_t h;
    if (!p || nvs_open("slink_ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "t", t); nvs_set_str(h, "p", p->label); nvs_commit(h); nvs_close(h);
}

/* when the running firmware was installed over WiFi (epoch seconds, 0 = unknown, e.g. flashed over USB) */
int64_t ota_installed(void)
{
    nvs_handle_t h; int64_t t = 0; char l[17] = ""; size_t n = sizeof l;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || nvs_open("slink_ota", NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_i64(h, "t", &t) != ESP_OK || nvs_get_str(h, "p", l, &n) != ESP_OK || strcmp(l, run->label)) t = 0;
    nvs_close(h);
    return t;
}

void ota_slot_info(char *slot, size_t n, const char **state)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(slot, n, "%s", run ? run->label : "");
    *state = state_name(run);
}

/* size of the running image (header + segments + padding/checksum + appended hash), read from the headers in flash; the slot and the flash chip */
void ota_size_info(uint32_t *img, uint32_t *slot, uint32_t *flash)
{
    static uint32_t cached = 0;
    const esp_partition_t *run = esp_ota_get_running_partition();
    *img = *slot = *flash = 0;
    if (run) {
        *slot = run->size;
        if (!cached) {
            uint8_t h[24];
            if (esp_partition_read(run, 0, h, sizeof h) == ESP_OK && h[0] == ESP_IMAGE_HEADER_MAGIC) {
                uint32_t off = sizeof h; bool ok = true;
                for (int i = 0; i < h[1] && ok; i++) {
                    uint32_t sh[2];
                    if (esp_partition_read(run, off, sh, sizeof sh) != ESP_OK || sh[1] > run->size) { ok = false; break; }
                    off += 8 + sh[1];
                }
                if (ok) { off = (off + 1 + 15) & ~15u; if (h[23]) off += 32; cached = off; }
            }
        }
        *img = cached;
    }
    uint32_t f = 0;
    if (esp_flash_get_physical_size(NULL, &f) != ESP_OK) f = 0;
    *flash = f;
}

static int64_t hdr_time(httpd_req_t *r)
{
    char v[24];
    return httpd_req_get_hdr_value_str(r, "X-OTA-Time", v, sizeof v) == ESP_OK ? atoll(v) : 0;
}

static void default_url(char *u, size_t n)
{
    snprintf(u, n, "https://github.com/%s/releases/latest/download/slink-%s.bin", CONFIG_SLINK_OTA_GITHUB_REPO, CONFIG_IDF_TARGET);
}

/* checks shared by pull and push: right application, not the firmware already running (unless forced). NULL = can be installed */
static const char *check_desc(const esp_app_desc_t *d, bool force)
{
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d->project_name, me->project_name, sizeof d->project_name)) return "not the S-Link firmware (different project name)";
    if (!force && !strncmp(d->version, me->version, sizeof d->version) && !strncmp(d->date, me->date, sizeof d->date) && !strncmp(d->time, me->time, sizeof d->time))
        return "already running this firmware (add FORCE to install it anyway)";
    return NULL;
}

/* the first bytes of an uploaded image: magic, chip, application description */
static const char *check_image(const uint8_t *b, size_t n, bool force, int *code)
{
    const size_t need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    *code = 400;
    if (n < need) return "file too short: not a firmware image";
    if (b[0] != ESP_IMAGE_HEADER_MAGIC) return "not an ESP32 application image";
    esp_image_header_t h; memcpy(&h, b, sizeof h);
    if (h.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return "image built for another chip";
    esp_app_desc_t d; memcpy(&d, b + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof d);
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) return "no application description in the image";
    const char *why = check_desc(&d, force);
    if (why) *code = 409;
    return why;
}

/* ---- pull: the device downloads the image ---- */

typedef struct { char url[256]; bool force; } pull_arg_t;

static esp_err_t http_init_cb(esp_http_client_handle_t c)
{
    if (g_set.ota_token[0]) {
        char v[80]; snprintf(v, sizeof v, "Bearer %s", g_set.ota_token);
        esp_http_client_set_header(c, "Authorization", v);
    }
    return ESP_OK;
}

static void pull_task(void *arg)
{
    pull_arg_t *a = arg;
    esp_http_client_config_t hc = {
        .url = a->url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000, .keep_alive_enable = true,
        .buffer_size = 2048, .buffer_size_tx = 1024, .user_agent = "slink-ota",     /* the release redirects to a long signed URL */
    };
    esp_https_ota_config_t oc = { .http_config = &hc, .http_client_init_cb = http_init_cb };
    esp_https_ota_handle_t h = NULL;
    const char *why;
    cli_out("OTA: downloading %s\r\n", a->url);
    wifi_fast(true);
    esp_err_t e = esp_https_ota_begin(&oc, &h);
    if (e != ESP_OK) { cli_out("OTA: failed - cannot open the source (%s)\r\n", esp_err_to_name(e)); goto fail; }
    esp_app_desc_t nd;
    if (esp_https_ota_get_img_desc(h, &nd) == ESP_OK) {
        why = check_desc(&nd, a->force);
        if (why) { cli_out("OTA: %s\r\n", why); esp_https_ota_abort(h); goto fail; }
        cli_out("OTA: new firmware %s (%s %s)\r\n", nd.version, nd.date, nd.time);
    }
    int last = -1, size = esp_https_ota_get_image_size(h);
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int pct = size > 0 ? (int)((int64_t)esp_https_ota_get_image_len_read(h) * 100 / size) : 0;
        if (pct / 5 != last / 5) { last = pct; cli_out("OTA: %d%%\r\n", pct); }
    }
    if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        cli_out("OTA: failed - download interrupted (%s)\r\n", esp_err_to_name(e)); esp_https_ota_abort(h); goto fail;
    }
    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    e = esp_https_ota_finish(h);
    if (e != ESP_OK) { cli_out("OTA: failed - %s\r\n", e == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is not valid" : esp_err_to_name(e)); goto fail; }
    stamp(tgt, 0);
    cli_out("OTA: 100%%\r\nOTA: done - restarting into the new firmware\r\n");
    reboot_soon();
    free(a); vTaskDelete(NULL);                /* s_busy stays set: the device is about to restart */
    return;
fail:
    wifi_fast(false);
    free(a); busy_free(); vTaskDelete(NULL);
}

static bool pull_start(const char *url, bool force)
{
    if (!g_set.ota_pull) { cli_out("ERR ota: downloading is disabled (OTA PULL ON enables it)\r\n"); return false; }
    if (!net_have_ip()) { cli_out("ERR ota: no network connection\r\n"); return false; }
    if (!busy_take()) { cli_out("ERR ota: an update is already running\r\n"); return false; }
    pull_arg_t *a = calloc(1, sizeof *a);
    if (!a) { busy_free(); cli_out("ERR ota: out of memory\r\n"); return false; }
    if (url && url[0]) strncpy(a->url, url, sizeof a->url - 1);
    else if (g_set.ota_url[0]) strncpy(a->url, g_set.ota_url, sizeof a->url - 1);
    else default_url(a->url, sizeof a->url);
    a->force = force;
    if (strncmp(a->url, "http://", 7) && strncmp(a->url, "https://", 8)) { free(a); busy_free(); cli_out("ERR ota: the URL must start with http:// or https://\r\n"); return false; }
    if (xTaskCreate(pull_task, "ota_pull", 10240, a, 5, NULL) != pdPASS) { free(a); busy_free(); cli_out("ERR ota: cannot start the task\r\n"); return false; }
    return true;
}

/* ---- push: POST /api/ota ---- */

static void drain(httpd_req_t *r)             /* read and drop the body, so that the client can read the answer (bounded) */
{
    char b[512]; size_t left = r->content_len > (2u << 20) ? (2u << 20) : r->content_len; int tmo = 0;
    while (left) {
        int n = httpd_req_recv(r, b, left > sizeof b ? sizeof b : left);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++tmo < 4) continue;
        if (n <= 0) break;
        left -= (size_t)n;
    }
}

static esp_err_t reject(httpd_req_t *r, int code, const char *msg)
{
    static const struct { int c; const char *s; } st[] = { {400, "400 Bad Request"}, {401, "401 Unauthorized"}, {409, "409 Conflict"}, {403, "403 Forbidden"}, {413, "413 Payload Too Large"}, {500, "500 Internal Server Error"} };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) if (st[i].c == code) httpd_resp_set_status(r, st[i].s);
    httpd_resp_set_type(r, "text/plain");
    cli_out("OTA: push rejected - %s\r\n", msg);
    return httpd_resp_send(r, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ota_http_info(httpd_req_t *r)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition(), *oth = esp_ota_get_next_update_partition(NULL);
    char b[460], nonce[48] = "";
    bool auth = g_set.ota_pass[0] != 0;
    if (auth) { new_nonce(); snprintf(nonce, sizeof nonce, ",\"nonce\":\"%s\"", s_nonce); }
    int n = snprintf(b, sizeof b, "{\"fw\":\"%.31s\",\"target\":\"%s\",\"running\":\"%s\",\"state\":\"%s\",\"max\":%u,\"push\":%s,\"pull\":%s,\"auth\":%s,\"busy\":%s%s}",
                     d->version, CONFIG_IDF_TARGET, run ? run->label : "", state_name(run), oth ? (unsigned)oth->size : 0,
                     g_set.ota_push ? "true" : "false", g_set.ota_pull ? "true" : "false", auth ? "true" : "false", s_busy ? "true" : "false", nonce);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, b, n);
}

esp_err_t ota_http_push(httpd_req_t *r)
{
    char hv[8];
    if (!g_set.ota_push) { drain(r); return reject(r, 403, "firmware upload is disabled on this device (enable it in Settings > Firmware update, or with OTA PUSH ON)"); }
    bool force = httpd_req_get_hdr_value_str(r, "X-OTA-Force", hv, sizeof hv) == ESP_OK && hv[0] == '1';
    if (g_set.ota_pass[0]) {
        char nonce[40] = "", auth[72] = "", want[65];
        httpd_req_get_hdr_value_str(r, "X-OTA-Nonce", nonce, sizeof nonce);
        httpd_req_get_hdr_value_str(r, "X-OTA-Auth", auth, sizeof auth);
        bool ok = s_nonce[0] && !strcmp(nonce, s_nonce) && esp_timer_get_time() - s_nonce_at < NONCE_LIFE_US;
        proof(s_nonce, want);
        s_nonce[0] = 0;                        /* one try per nonce */
        unsigned diff = strlen(auth) == 64 ? 0 : 1;
        for (int i = 0; i < 64; i++) diff |= (unsigned)(auth[i] ^ want[i]);           /* auth is zero padded: no read beyond its buffer */
        if (!ok || diff) { vTaskDelay(pdMS_TO_TICKS(1000)); drain(r); return reject(r, 401, "wrong password, or the nonce expired (ask GET /api/ota again)"); }
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return reject(r, 500, "no update partition");
    if (r->content_len < 1024) return reject(r, 400, "the body must be the firmware .bin");
    if (r->content_len > part->size) { drain(r); return reject(r, 413, "the firmware is larger than the update partition"); }
    if (!busy_take()) { drain(r); return reject(r, 409, "an update is already running"); }

    int code = 500; const char *err = "upload interrupted";
    uint8_t *buf = malloc(4096);
    esp_ota_handle_t oh = 0; bool began = false;
    size_t got = 0, total = r->content_len; int last = -1, tmo = 0;
    if (!buf) { busy_free(); return reject(r, 500, "out of memory"); }
    wifi_fast(true);
    cli_out("OTA: receiving %u bytes\r\n", (unsigned)total);
    while (got < HEAD_MIN && got < total) {                                           /* the header decides before anything is written */
        int n = httpd_req_recv(r, (char *)buf + got, 4096 - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++tmo < 6) continue;
        if (n <= 0) goto done;
        got += (size_t)n; tmo = 0;
    }
    if ((err = check_image(buf, got, force, &code)) != NULL) { drain(r); goto done; }
    if (esp_ota_begin(part, total, &oh) != ESP_OK) { err = "cannot start the flash write"; code = 500; goto done; }
    began = true;
    if (esp_ota_write(oh, buf, got) != ESP_OK) { err = "flash write failed"; code = 500; goto done; }
    err = "upload interrupted"; code = 500;
    while (got < total) {
        size_t want = total - got < 4096 ? total - got : 4096;
        int n = httpd_req_recv(r, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++tmo < 6) continue;
        if (n <= 0) goto done;
        tmo = 0;
        if (esp_ota_write(oh, buf, (size_t)n) != ESP_OK) { err = "flash write failed"; goto done; }
        got += (size_t)n;
        int pct = (int)((uint64_t)got * 100 / total);
        if (pct / 10 != last / 10) { last = pct; cli_out("OTA: %d%%\r\n", pct); }
    }
    began = false;                                                                    /* esp_ota_end() closes the handle in every case */
    if (esp_ota_end(oh) != ESP_OK) { err = "the image is not valid (checksum or signature)"; code = 400; goto done; }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { err = "cannot select the new firmware"; goto done; }
    stamp(part, hdr_time(r));
    err = NULL;
done:
    free(buf);
    if (began) esp_ota_abort(oh);
    if (err) { wifi_fast(false); busy_free(); return reject(r, code, err); }
    cli_out("OTA: done - restarting into the new firmware\r\n");
    httpd_resp_set_type(r, "text/plain");
    esp_err_t e = httpd_resp_send(r, "OK firmware installed, restarting", HTTPD_RESP_USE_STRLEN);
    reboot_soon();
    return e;
}

/* ---- confirmation / rollback ---- */

static void confirm_task(void *a)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (esp_timer_get_time() < 30LL * 1000000) continue;                       /* it must run for 30 s ... */
        if (g_set.ssid[0] && (g_set.web || g_set.app) && !net_have_ip()) continue; /* ... and, when the network is in use, be on it */
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            cli_out("OTA: the new firmware is confirmed\r\n");
        }
        vTaskDelete(NULL);
    }
}

void ota_confirm_start(void)
{
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sc);
    xTaskCreate(confirm_task, "ota_ok", 3072, NULL, 3, NULL);
}

/* ---- CLI ---- */

static void status(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition(), *oth = esp_ota_get_next_update_partition(NULL);
    char u[256];
    cli_out("OTA fw %s built %s %s target %s\r\n", d->version, d->date, d->time, CONFIG_IDF_TARGET);
    cli_out("OTA installed %lld\r\n", (long long)ota_installed());
    cli_out("OTA running %s state %s\r\n", run ? run->label : "?", state_name(run));
    esp_app_desc_t od;
    if (oth && esp_ota_get_partition_description(oth, &od) == ESP_OK) cli_out("OTA other %s fw %s built %s %s\r\n", oth->label, od.version, od.date, od.time);
    else cli_out("OTA other %s empty\r\n", oth ? oth->label : "?");
    if (g_set.ota_url[0]) snprintf(u, sizeof u, "%s", g_set.ota_url); else default_url(u, sizeof u);
    cli_out("OTA source %s\r\nOTA url \"%s\"\r\nOTA pass %d\r\nOTA token %d\r\nOTA push %d\r\nOTA pull %d\r\nOTA busy %d\r\n", u, g_set.ota_url, g_set.ota_pass[0] != 0, g_set.ota_token[0] != 0, g_set.ota_push, g_set.ota_pull, s_busy);
    cli_out("OK ota\r\n");
}

static bool setstr(char *dst, size_t sz, const char *v, const char *what)
{
    if (strlen(v) >= sz) { cli_out("ERR ota: %s too long (max %d characters)\r\n", what, (int)sz - 1); return false; }
    strcpy(dst, v); settings_save(); return true;
}

void ota_cli(char **av, int na)
{
    const char *sub = na > 1 ? av[1] : "STATUS";
    if (!strcasecmp(sub, "STATUS")) status();
    else if (!strcasecmp(sub, "UPDATE")) {
        bool force = false; const char *url = NULL;
        for (int i = 2; i < na; i++) { if (!strcasecmp(av[i], "FORCE")) force = true; else url = av[i]; }
        if (pull_start(url, force)) cli_out("OK ota started\r\n");
    }
    else if (!strcasecmp(sub, "URL")) {
        if (na > 2) {
            if (!strcasecmp(av[2], "DEFAULT") || !strcmp(av[2], "-")) { g_set.ota_url[0] = 0; settings_save(); }
            else if (strncmp(av[2], "http://", 7) && strncmp(av[2], "https://", 8)) { cli_out("ERR ota: the URL must start with http:// or https://\r\n"); return; }
            else if (!setstr(g_set.ota_url, sizeof g_set.ota_url, av[2], "URL")) return;
        }
        cli_out("OK ota url \"%s\"%s\r\n", g_set.ota_url, g_set.ota_url[0] ? "" : " (default: latest GitHub release)");
    }
    else if (!strcasecmp(sub, "TOKEN") || !strcasecmp(sub, "PASS")) {
        bool tok = !strcasecmp(sub, "TOKEN");
        char *dst = tok ? g_set.ota_token : g_set.ota_pass; size_t sz = tok ? sizeof g_set.ota_token : sizeof g_set.ota_pass;
        if (na > 2) {
            if (!strcasecmp(av[2], "OFF") || !strcmp(av[2], "-")) { dst[0] = 0; settings_save(); }
            else if (!setstr(dst, sz, av[2], tok ? "token" : "password")) return;
            if (!tok) s_nonce[0] = 0;
        }
        cli_out("OK ota %s %s\r\n", tok ? "token" : "password", dst[0] ? "set" : "off");
    }
    else if (!strcasecmp(sub, "PUSH") || !strcasecmp(sub, "PULL")) {
        bool push = !strcasecmp(sub, "PUSH"); bool *flag = push ? &g_set.ota_push : &g_set.ota_pull;
        if (na > 2) {
            if (!strcasecmp(av[2], "ON") || !strcmp(av[2], "1")) *flag = true;
            else if (!strcasecmp(av[2], "OFF") || !strcmp(av[2], "0")) *flag = false;
            else { cli_out("ERR usage: OTA %s [ON|OFF]\r\n", push ? "PUSH" : "PULL"); return; }
            settings_save();
        }
        cli_out("OK ota %s %s\r\n", push ? "push" : "pull", *flag ? "on" : "off");
    }
    else if (!strcasecmp(sub, "ROLLBACK")) {
        if (!esp_ota_check_rollback_is_possible()) { cli_out("ERR ota: no previous firmware to go back to\r\n"); return; }
        cli_out("OK ota rolling back to the previous firmware, restarting\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    else cli_out("ERR usage: OTA [STATUS] | UPDATE [url] [FORCE] | URL [url|DEFAULT] | TOKEN [text|OFF] | PASS [text|OFF] | PUSH [ON|OFF] | PULL [ON|OFF] | ROLLBACK\r\n");
}
