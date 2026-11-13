#pragma once
#include <stddef.h>

/* the files of main/web/ embedded in the firmware by tools/gen_web.py */
typedef struct {
    const char *uri, *mime, *cache, *etag;   /* etag: quoted, ready for the header */
    const unsigned char *data;
    size_t len;                              /* bytes stored (gzip when gz) */
    int gz;                                  /* stored gzip-compressed: sent with Content-Encoding: gzip */
} web_asset_t;

extern const web_asset_t web_assets[];
extern const size_t web_assets_n;
extern const char web_build[];             /* id of the embedded index.html (also in the page as window.SLINK_BUILD) */
