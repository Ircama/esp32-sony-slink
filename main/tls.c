/* Private certificate authority and server certificate for the HTTPS web portal (see tls.h).
 * Uses PSA (key generation, random) and the mbedTLS X.509 writer of ESP-IDF 6 (mbedTLS 4). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "tls.h"

static const char *TAG = "tls";

#define NOT_BEFORE "20240101000000"          /* the device has no clock at start: a long fixed validity (the CA is only trusted by the user's own devices) */
#define NOT_AFTER  "20491231235959"
#define CERT_MAX   1792
#define KEY_MAX    512

static char *s_ca, *s_cert, *s_key;
static size_t s_cert_n, s_key_n;
static SemaphoreHandle_t s_mtx;

/* ---- NVS ---- */
static char *nvs_load(const char *k)
{
    nvs_handle_t h; size_t n = 0; char *b = NULL;
    if (nvs_open("slink", NVS_READONLY, &h) != ESP_OK) return NULL;
    if (nvs_get_str(h, k, NULL, &n) == ESP_OK && n > 1) {
        b = malloc(n);
        if (b && nvs_get_str(h, k, b, &n) != ESP_OK) { free(b); b = NULL; }
    }
    nvs_close(h);
    return b;
}
static bool nvs_save_str(const char *k, const char *v)
{
    nvs_handle_t h; bool ok = false;
    if (nvs_open("slink", NVS_READWRITE, &h) != ESP_OK) return false;
    ok = nvs_set_str(h, k, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---- DER pieces for the extensions the writer has no helper for ---- */
static size_t der(uint8_t *o, uint8_t tag, const uint8_t *v, size_t n)
{
    size_t k = 0; o[k++] = tag;
    if (n < 128) o[k++] = (uint8_t)n;
    else if (n < 256) { o[k++] = 0x81; o[k++] = (uint8_t)n; }
    else { o[k++] = 0x82; o[k++] = (uint8_t)(n >> 8); o[k++] = (uint8_t)n; }
    memcpy(o + k, v, n);
    return k + n;
}

/* the CA may only vouch for local names and private addresses: if its key were ever stolen from the board, a phone that trusts it could not be
 * fooled about any public site */
static size_t build_name_constraints(uint8_t *o)
{
    static const char *const dns[] = { "local", "lan", "home.arpa", "fritz.box", "localdomain", "home", "internal" };
    static const uint8_t net[][8] = {
        { 10, 0, 0, 0, 255, 0, 0, 0 }, { 172, 16, 0, 0, 255, 240, 0, 0 }, { 192, 168, 0, 0, 255, 255, 0, 0 }, { 169, 254, 0, 0, 255, 255, 0, 0 } };
    uint8_t st[320], gn[32]; size_t k = 0, n;
    for (size_t i = 0; i < sizeof dns / sizeof dns[0]; i++) {
        n = der(gn, 0x82, (const uint8_t *)dns[i], strlen(dns[i]));      /* dNSName */
        k += der(st + k, 0x30, gn, n);
    }
    for (size_t i = 0; i < sizeof net / sizeof net[0]; i++) {
        n = der(gn, 0x87, net[i], 8);                                    /* iPAddress: address + mask */
        k += der(st + k, 0x30, gn, n);
    }
    uint8_t p[340];
    n = der(p, 0xA0, st, k);                                             /* permittedSubtrees */
    return der(o, 0x30, p, n);
}

static size_t build_san(uint8_t *o, const char *host, uint32_t ip)
{
    static const char *const dom[] = { "local", "fritz.box", "lan", "home.arpa" };
    uint8_t b[384], gn[96]; size_t k = 0, n;
    for (size_t i = 0; i < sizeof dom / sizeof dom[0]; i++) {
        char name[80]; int l = snprintf(name, sizeof name, "%s.%s", host, dom[i]);
        if (l <= 0 || l >= (int)sizeof name) continue;
        n = der(gn, 0x82, (const uint8_t *)name, (size_t)l);
        memcpy(b + k, gn, n); k += n;
    }
    if (ip) { n = der(gn, 0x87, (const uint8_t *)&ip, 4); memcpy(b + k, gn, n); k += n; }
    return der(o, 0x30, b, k);
}

/* ---- keys ---- */
static psa_key_id_t key_new(void)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT; psa_key_id_t id = 0;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    if (psa_generate_key(&a, &id) != PSA_SUCCESS) return 0;
    return id;
}

static psa_key_id_t key_import(const uint8_t *raw, size_t n)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT; psa_key_id_t id = 0;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    if (psa_import_key(&a, raw, n, &id) != PSA_SUCCESS) return 0;
    return id;
}

/* ---- one certificate ---- */
static int mk_cert(const char *subject, const char *issuer, mbedtls_pk_context *spk, mbedtls_pk_context *ipk, bool ca,
                   const uint8_t *nc, size_t nc_n, const uint8_t *san, size_t san_n, char *pem, size_t sz)
{
    mbedtls_x509write_cert w; uint8_t serial[16]; int r;
    static const uint8_t oid_nc[] = { 0x55, 0x1D, 0x1E }, oid_san[] = { 0x55, 0x1D, 0x11 }, oid_eku[] = { 0x55, 0x1D, 0x25 };
    static const uint8_t eku[] = { 0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };   /* serverAuth */
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, spk);
    mbedtls_x509write_crt_set_issuer_key(&w, ipk);
    psa_generate_random(serial, sizeof serial);
    serial[0] = (uint8_t)((serial[0] & 0x7F) | 0x10);                    /* positive, no leading zero */
    if ((r = mbedtls_x509write_crt_set_subject_name(&w, subject)) != 0) goto out;
    if ((r = mbedtls_x509write_crt_set_issuer_name(&w, issuer)) != 0) goto out;
    if ((r = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) != 0) goto out;
    if ((r = mbedtls_x509write_crt_set_validity(&w, NOT_BEFORE, NOT_AFTER)) != 0) goto out;
    if ((r = mbedtls_x509write_crt_set_basic_constraints(&w, ca, ca ? 0 : -1)) != 0) goto out;
    if ((r = mbedtls_x509write_crt_set_key_usage(&w, ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0) goto out;
    if (nc && (r = mbedtls_x509write_crt_set_extension(&w, (const char *)oid_nc, sizeof oid_nc, 1, nc, nc_n)) != 0) goto out;
    if (san && (r = mbedtls_x509write_crt_set_extension(&w, (const char *)oid_san, sizeof oid_san, 0, san, san_n)) != 0) goto out;
    if (!ca && (r = mbedtls_x509write_crt_set_extension(&w, (const char *)oid_eku, sizeof oid_eku, 0, eku, sizeof eku)) != 0) goto out;
    r = mbedtls_x509write_crt_pem(&w, (unsigned char *)pem, sz);
out:
    mbedtls_x509write_crt_free(&w);
    return r;
}

/* ---- generation (runs in a task of its own: key generation needs a deep stack) ---- */
typedef struct { char host[40]; uint32_t ip; bool ok; SemaphoreHandle_t done; } job_t;

static void gen_task(void *arg)
{
    job_t *j = arg;
    char *ca_pem = nvs_load("tls_ca_c"), *ca_k = nvs_load("tls_ca_k"), *sv_id = nvs_load("tls_sv_id"), *sv_c = nvs_load("tls_sv_c"), *sv_k = nvs_load("tls_sv_k"), *ca_n = nvs_load("tls_ca_n");
    char cn_ca[64], id_now[80], cn_sv[64];
    uint8_t mac[6] = {0}, raw[32], nc[400], san[400];
    mbedtls_pk_context capk, svpk;
    psa_key_id_t ca_id = 0, sv_id_k = 0; size_t rl = 0;
    char *pem = NULL;

    mbedtls_pk_init(&capk); mbedtls_pk_init(&svpk);
    j->ok = false;
    if (psa_crypto_init() != PSA_SUCCESS) goto done;
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(cn_ca, sizeof cn_ca, "CN=esp32-sony-slink CA %02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(id_now, sizeof id_now, "%s|%u.%u.%u.%u", j->host, (unsigned)(j->ip & 255), (unsigned)((j->ip >> 8) & 255), (unsigned)((j->ip >> 16) & 255), (unsigned)(j->ip >> 24));
    snprintf(cn_sv, sizeof cn_sv, "CN=%s.local", j->host);
    pem = malloc(CERT_MAX);
    if (!pem) goto done;

    /* the CA: made once; its key is kept raw (32 bytes) in a string of hex digits */
    if (!(ca_pem && ca_k && strlen(ca_k) == 64 && ca_n && !strcmp(ca_n, cn_ca))) {      /* also made again when its name changed (older firmware) */
        free(ca_pem); free(ca_k); ca_pem = ca_k = NULL;
        ca_id = key_new();
        if (!ca_id || psa_export_key(ca_id, raw, sizeof raw, &rl) != PSA_SUCCESS || rl != 32) { ESP_LOGE(TAG, "CA key"); goto done; }
        if (mbedtls_pk_copy_from_psa(ca_id, &capk) != 0) { ESP_LOGE(TAG, "CA pk"); goto done; }
        size_t n = build_name_constraints(nc);
        int r = mk_cert(cn_ca, cn_ca, &capk, &capk, true, nc, n, NULL, 0, pem, CERT_MAX);
        if (r != 0) { ESP_LOGE(TAG, "CA cert -0x%04x", -r); goto done; }
        char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", raw[i]);
        if (!nvs_save_str("tls_ca_k", hex) || !nvs_save_str("tls_ca_c", pem) || !nvs_save_str("tls_ca_n", cn_ca)) { ESP_LOGE(TAG, "CA save"); goto done; }
        ca_pem = strdup(pem); ca_k = strdup(hex);
        memset(raw, 0, sizeof raw); memset(hex, 0, sizeof hex);
        ESP_LOGI(TAG, "new CA: %s", cn_ca);
        free(sv_id); sv_id = NULL;                                       /* a new CA: the old server certificate is no longer valid */
    } else {
        for (int i = 0; i < 32; i++) { unsigned v; sscanf(ca_k + 2 * i, "%2x", &v); raw[i] = (uint8_t)v; }
        ca_id = key_import(raw, 32); memset(raw, 0, sizeof raw);
        if (!ca_id || mbedtls_pk_copy_from_psa(ca_id, &capk) != 0) { ESP_LOGE(TAG, "CA load"); goto done; }
    }

    /* the server certificate: made again when the name or the address changed */
    if (!(sv_id && sv_c && sv_k && !strcmp(sv_id, id_now))) {
        sv_id_k = key_new();
        char *kp = malloc(KEY_MAX);
        if (!sv_id_k || !kp || mbedtls_pk_copy_from_psa(sv_id_k, &svpk) != 0 || mbedtls_pk_write_key_pem(&svpk, (unsigned char *)kp, KEY_MAX) != 0) {
            free(kp); ESP_LOGE(TAG, "server key"); goto done;
        }
        size_t sn = build_san(san, j->host, j->ip);
        int r = mk_cert(cn_sv, cn_ca, &svpk, &capk, false, NULL, 0, san, sn, pem, CERT_MAX);
        if (r != 0) { free(kp); ESP_LOGE(TAG, "server cert -0x%04x", -r); goto done; }
        if (!nvs_save_str("tls_sv_c", pem) || !nvs_save_str("tls_sv_k", kp) || !nvs_save_str("tls_sv_id", id_now)) ESP_LOGW(TAG, "server certificate not saved");
        free(sv_c); free(sv_k); sv_c = strdup(pem); sv_k = kp;
        ESP_LOGI(TAG, "new server certificate for %s", id_now);
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    free(s_ca); free(s_cert); free(s_key);
    s_ca = ca_pem; s_cert = sv_c; s_key = sv_k;
    ca_pem = sv_c = sv_k = NULL;
    s_cert_n = strlen(s_cert) + 1; s_key_n = strlen(s_key) + 1;
    xSemaphoreGive(s_mtx);
    j->ok = true;
done:
    mbedtls_pk_free(&capk); mbedtls_pk_free(&svpk);
    if (ca_id) psa_destroy_key(ca_id);
    if (sv_id_k) psa_destroy_key(sv_id_k);
    free(pem); free(ca_pem); free(ca_k); free(ca_n); free(sv_id); free(sv_c); free(sv_k);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

bool tls_prepare(const char *host, uint32_t ip)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    job_t j = { .ip = ip, .ok = false, .done = xSemaphoreCreateBinary() };
    snprintf(j.host, sizeof j.host, "%s", host && *host ? host : "slink");
    if (!j.done) return false;
    if (xTaskCreate(gen_task, "tls_gen", 12288, &j, 4, NULL) != pdPASS) { vSemaphoreDelete(j.done); return false; }
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return j.ok;
}

const uint8_t *tls_server_cert(size_t *len) { if (len) *len = s_cert_n; return (const uint8_t *)s_cert; }
const uint8_t *tls_server_key(size_t *len) { if (len) *len = s_key_n; return (const uint8_t *)s_key; }

const char *tls_ca_pem(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_ca) s_ca = nvs_load("tls_ca_c");                              /* the download works even while the HTTPS server is off */
    return s_ca;
}

bool tls_has_ca(void) { return tls_ca_pem() != NULL; }

void tls_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("slink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "tls_ca_k"); nvs_erase_key(h, "tls_ca_c"); nvs_erase_key(h, "tls_ca_n");
        nvs_erase_key(h, "tls_sv_c"); nvs_erase_key(h, "tls_sv_k"); nvs_erase_key(h, "tls_sv_id");
        nvs_commit(h); nvs_close(h);
    }
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    free(s_ca); free(s_cert); free(s_key); s_ca = s_cert = s_key = NULL; s_cert_n = s_key_n = 0;
    xSemaphoreGive(s_mtx);
}
