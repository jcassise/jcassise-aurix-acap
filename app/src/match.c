#include "match.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define AURIX_NEON 1
#endif

static int read_u32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

int gallery_load(const char *path, aurix_gallery *g)
{
    memset(g, 0, sizeof(*g));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t ver, dim, count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "AURG", 4) ||
        read_u32(f, &ver) || ver != 1 || read_u32(f, &dim) || read_u32(f, &count) ||
        dim == 0 || dim > 4096) {
        fclose(f);
        return -1;
    }
    g->ids = calloc(count ? count : 1, AURIX_ID_LEN);
    g->emb = malloc((size_t)(count ? count : 1) * dim);
    g->inv_norm = malloc(sizeof(float) * (count ? count : 1));
    if (!g->ids || !g->emb || !g->inv_norm) goto fail;
    for (uint32_t i = 0; i < count; i++) {
        if (fread(g->ids[i], 1, AURIX_ID_LEN, f) != AURIX_ID_LEN ||
            fread(g->emb + (size_t)i * dim, 1, dim, f) != dim)
            goto fail;
        g->ids[i][AURIX_ID_LEN - 1] = '\0';
        const int8_t *e = g->emb + (size_t)i * dim;
        int32_t n2 = dot_s8(e, e, dim);
        g->inv_norm[i] = n2 > 0 ? 1.0f / sqrtf((float)n2) : 0.0f;
    }
    fclose(f);
    g->dim = dim;
    g->count = count;
    return 0;
fail:
    fclose(f);
    gallery_free(g);
    return -1;
}

void gallery_free(aurix_gallery *g)
{
    free(g->ids);
    free(g->emb);
    free(g->inv_norm);
    memset(g, 0, sizeof(*g));
}

void embedding_quantize(const float *in, uint32_t dim, int8_t *out)
{
    double n2 = 0;
    for (uint32_t i = 0; i < dim; i++) n2 += (double)in[i] * in[i];
    float k = n2 > 0 ? (float)(127.0 / sqrt(n2)) : 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        long v = lrintf(in[i] * k);
        out[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
}

int32_t dot_s8(const int8_t *a, const int8_t *b, uint32_t dim)
{
    uint32_t i = 0;
    int32_t sum = 0;
#ifdef AURIX_NEON
    int32x4_t acc = vdupq_n_s32(0);
    for (; i + 16 <= dim; i += 16) {
        int8x16_t va = vld1q_s8(a + i), vb = vld1q_s8(b + i);
        acc = vpadalq_s16(acc, vmull_s8(vget_low_s8(va), vget_low_s8(vb)));
        acc = vpadalq_s16(acc, vmull_s8(vget_high_s8(va), vget_high_s8(vb)));
    }
#if defined(__aarch64__)
    sum = vaddvq_s32(acc);
#else
    sum = vgetq_lane_s32(acc, 0) + vgetq_lane_s32(acc, 1) +
          vgetq_lane_s32(acc, 2) + vgetq_lane_s32(acc, 3);
#endif
#endif
    for (; i < dim; i++) sum += (int32_t)a[i] * b[i];
    return sum;
}

int gallery_best(const aurix_gallery *g, const int8_t *q, float *score)
{
    *score = -1.0f;
    if (!g || g->count == 0) return -1;
    int32_t qn = dot_s8(q, q, g->dim);
    if (qn <= 0) return -1;
    float qinv = 1.0f / sqrtf((float)qn);
    int best = -1;
    for (uint32_t i = 0; i < g->count; i++) {
        float s = dot_s8(q, g->emb + (size_t)i * g->dim, g->dim) * qinv * g->inv_norm[i];
        if (s > *score) { *score = s; best = (int)i; }
    }
    return best;
}
