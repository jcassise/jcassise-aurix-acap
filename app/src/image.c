#include "image.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static inline uint8_t clamp_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

void nv12_to_rgb(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, uint8_t *rgb)
{
    for (int j = 0; j < h; j++) {
        const uint8_t *yr = y + (size_t)j * stride;
        const uint8_t *uvr = uv + (size_t)(j / 2) * stride;
        uint8_t *o = rgb + (size_t)j * w * 3;
        for (int i = 0; i < w; i++) {
            int Y = yr[i] - 16;
            if (Y < 0) Y = 0;
            int U = uvr[i & ~1] - 128;
            int V = uvr[(i & ~1) + 1] - 128;
            int c = 298 * Y;
            o[3 * i + 0] = clamp_u8((c + 409 * V + 128) >> 8);
            o[3 * i + 1] = clamp_u8((c - 100 * U - 208 * V + 128) >> 8);
            o[3 * i + 2] = clamp_u8((c + 516 * U + 128) >> 8);
        }
    }
}

void warp_affine_bilinear(const aurix_image *src, aurix_image *dst, const float m[6])
{
    const int ch = dst->ch;
    for (int v = 0; v < dst->h; v++) {
        uint8_t *orow = dst->data + (size_t)v * dst->stride;
        for (int u = 0; u < dst->w; u++) {
            uint8_t *p = orow + (size_t)u * ch;
            float x = m[0] * u + m[1] * v + m[2];
            float y = m[3] * u + m[4] * v + m[5];
            if (x <= -1.0f || y <= -1.0f || x >= (float)src->w || y >= (float)src->h) {
                for (int k = 0; k < ch; k++) p[k] = 0;
                continue;
            }
            int x0 = (int)floorf(x), y0 = (int)floorf(y);
            float fx = x - x0, fy = y - y0;
            int x1 = x0 + 1, y1 = y0 + 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > src->w - 1) x1 = src->w - 1;
            if (y1 > src->h - 1) y1 = src->h - 1;
            const uint8_t *r0 = src->data + (size_t)y0 * src->stride;
            const uint8_t *r1 = src->data + (size_t)y1 * src->stride;
            for (int k = 0; k < ch; k++) {
                float a = r0[x0 * ch + k] + fx * (r0[x1 * ch + k] - r0[x0 * ch + k]);
                float b = r1[x0 * ch + k] + fx * (r1[x1 * ch + k] - r1[x0 * ch + k]);
                p[k] = clamp_u8((int)lrintf(a + fy * (b - a)));
            }
        }
    }
}

void resize_bilinear(const aurix_image *src, aurix_image *dst)
{
    float sx = (float)src->w / dst->w, sy = (float)src->h / dst->h;
    const float m[6] = { sx, 0.0f, 0.5f * sx - 0.5f, 0.0f, sy, 0.5f * sy - 0.5f };
    warp_affine_bilinear(src, dst, m);
}

int image_letterbox(const aurix_image *src, double aspect, aurix_image *c, int *ox, int *oy)
{
    int w = src->w, h = src->h;
    int cw = w, ch = h;
    if ((double)w / h < aspect) cw = (int)(h * aspect + 0.5);
    else ch = (int)(w / aspect + 0.5);
    unsigned char *px = malloc((size_t)cw * ch * 3);
    if (!px) return -1;
    memset(px, 128, (size_t)cw * ch * 3);
    *ox = (cw - w) / 2;
    *oy = (ch - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(px + ((size_t)(y + *oy) * cw + *ox) * 3, src->data + (size_t)y * src->stride, (size_t)w * 3);
    *c = (aurix_image){ px, cw, ch, cw * 3, 3 };
    return 0;
}

