#pragma once
#include <stddef.h>
#include <stdint.h>

void slink_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);   /* message shorter than 110 bytes */
void slink_hex(const uint8_t *b, size_t n, char *o);                  /* 2n lowercase hex digits and a NUL */
