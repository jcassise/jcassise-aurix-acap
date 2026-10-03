#include "jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 12000
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int jpeg_decode_rgb(const unsigned char *data, size_t len, int max_side, aurix_image *out, char *why, size_t wl)
{
    memset(out, 0, sizeof *out);
    if (!data || len < 4 || len > (32u << 20)) { snprintf(why, wl, "photo is empty or larger than 32 MB"); return -1; }
    int w, h, n;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &n)) { snprintf(why, wl, "not a JPEG photo"); return -1; }
    if ((long long)w * h > 40000000LL) { snprintf(why, wl, "photo too large (%dx%d)", w, h); return -1; }
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 3);
    if (!px) { snprintf(why, wl, "JPEG decode failed: %s", stbi_failure_reason()); return -1; }
    aurix_image src = { px, w, h, w * 3, 3 };
    if (max_side > 0 && (w > max_side || h > max_side)) {
        double s = (double)max_side / (w > h ? w : h);
        int nw = (int)(w * s + 0.5), nh = (int)(h * s + 0.5);
        unsigned char *small = malloc((size_t)nw * nh * 3);
        if (!small) { stbi_image_free(px); snprintf(why, wl, "out of memory"); return -1; }
        aurix_image dst = { small, nw, nh, nw * 3, 3 };
        resize_bilinear(&src, &dst);
        stbi_image_free(px);
        *out = dst;
        return 0;
    }
    /* hand back a malloc'd copy so callers free() it uniformly */
    unsigned char *copy = malloc((size_t)w * h * 3);
    if (!copy) { stbi_image_free(px); snprintf(why, wl, "out of memory"); return -1; }
    memcpy(copy, px, (size_t)w * h * 3);
    stbi_image_free(px);
    *out = (aurix_image){ copy, w, h, w * 3, 3 };
    return 0;
}

typedef struct { unsigned char *p; size_t n, cap; int bad; } outbuf;

static void put(void *ctx, void *data, int size)
{
    outbuf *o = ctx;
    if (o->bad || size <= 0) return;
    if (o->n + (size_t)size > o->cap) {
        size_t cap = (o->n + (size_t)size) * 2;
        unsigned char *q = realloc(o->p, cap);
        if (!q) { o->bad = 1; return; }
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->n, data, (size_t)size);
    o->n += (size_t)size;
}

unsigned char *jpeg_encode_rgb(const aurix_image *img, int quality, size_t *out_len)
{
    outbuf o = { NULL, 0, 0, 0 };
    const unsigned char *px = img->data;
    unsigned char *packed = NULL;
    if (img->stride != img->w * 3) {               /* stb wants tightly packed rows */
        packed = malloc((size_t)img->w * img->h * 3);
        if (!packed) return NULL;
        for (int y = 0; y < img->h; y++)
            memcpy(packed + (size_t)y * img->w * 3, img->data + (size_t)y * img->stride, (size_t)img->w * 3);
        px = packed;
    }
    int ok = stbi_write_jpg_to_func(put, &o, img->w, img->h, 3, px, quality);
    free(packed);
    if (!ok || o.bad) { free(o.p); return NULL; }
    *out_len = o.n;
    return o.p;
}
