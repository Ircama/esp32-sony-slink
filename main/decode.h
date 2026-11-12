#pragma once
#include <stddef.h>
#include <stdint.h>

/* Human-readable decoding of a received/transmitted S-Link frame.
 * Writes a short description (e.g. "AMP reply: power ON") into out.
 * Returns the length written (0 = nothing known about this frame). */
size_t slink_decode(const uint8_t *d, size_t len, char *out, size_t out_sz);

/* Sound field name for numbers 0 (off) .. 29, "?" otherwise. */
const char *slink_sf_name(unsigned n);
