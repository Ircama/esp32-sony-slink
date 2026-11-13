#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/* Over-the-air firmware update (WiFi chips only; the ESP32-H2 has no WiFi).
 *   pull: the device downloads the .bin itself, from the GitHub release (default) or from the URL set in the settings / given to OTA UPDATE
 *   push: an external tool (tools/ota_push.py) or the web portal sends the .bin with POST /api/ota; when a password is set the request
 *         carries sha256("nonce:password") of a nonce taken from GET /api/ota, so the password itself never travels on the network
 * Two app slots + rollback: a new firmware must confirm itself (ota_confirm_start) or the bootloader goes back to the previous one. */

void ota_cli(char **av, int na);              /* OTA [STATUS] | UPDATE [url] [FORCE] | URL [url|DEFAULT] | TOKEN [t|OFF] | PASS [p|OFF] | ROLLBACK */
void ota_confirm_start(void);                 /* from app_main: mark the running firmware valid once it has proved healthy (rollback) */
esp_err_t ota_http_info(httpd_req_t *r);      /* GET  /api/ota : JSON with version, slot, whether a password is needed and the nonce */
esp_err_t ota_http_push(httpd_req_t *r);      /* POST /api/ota : raw .bin body */
int64_t ota_installed(void);                  /* epoch seconds of the WiFi installation of the running firmware, 0 = unknown */
void ota_slot_info(char *slot, size_t n, const char **state);   /* running slot label and image state */
void ota_size_info(uint32_t *img, uint32_t *slot, uint32_t *flash);   /* bytes: running image, its slot, flash chip (0 = unknown) */
