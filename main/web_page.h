#pragma once
#include <stddef.h>

/* the files of main/web/ embedded in the firmware by tools/gen_web.py */
typedef struct {
    const char *uri, *mime, *cache;
    const unsigned char *data;
    size_t len;
} web_asset_t;

extern const web_asset_t web_assets[];
extern const size_t web_assets_n;
