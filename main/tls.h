#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* HTTPS for the web portal with a private certificate authority made by the device itself (tls.c).
 *   - the CA (EC P-256 key, certificate limited by name constraints to local names and private IP ranges) is created once and kept in the NVS;
 *     it is downloaded from /ca.crt and installed by the user on the phone / PC (a browser then trusts the portal, and a phone can install it as an app)
 *   - the server certificate (signed by that CA) carries <host>.local, <host>.fritz.box, <host>.lan, <host>.home.arpa and the current IPv4 address:
 *     it is made again when the host name or the address changes */

/* make sure the CA and a server certificate for (host, ip) exist; false on error. Blocks for a second or two the first time.
 * ip = IPv4 address in network byte order (0 = none). */
bool tls_prepare(const char *host, uint32_t ip);

/* PEM strings, valid after tls_prepare() succeeded (the length includes the final NUL, as esp_https_server wants) */
const uint8_t *tls_server_cert(size_t *len);
const uint8_t *tls_server_key(size_t *len);
const char *tls_ca_pem(void);                 /* NULL while there is no CA */

void tls_forget(void);                        /* delete the CA and the server certificate (a new CA is made at the next start: it has to be installed again) */
bool tls_has_ca(void);
