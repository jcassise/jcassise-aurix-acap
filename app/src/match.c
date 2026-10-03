#include "match.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

void gallery_init(aurix_gallery *g, uint32_t dim)
{
    memset(g, 0, sizeof(*g));
    g->dim = dim;
}

int gallery_add(aurix_gallery *g, const char *id, aurix_category cat, const int8_t *emb)
{
    return gallery_add_ref(g, id, "", cat, emb);
}

int gallery_add_ref(aurix_gallery *g, const char *id, const char *ref, aurix_category cat, const int8_t *emb)
{
    if (g->dim == 0) return -1;
    if (g->count == g->cap) {
        uint32_t cap = g->cap ? g->cap * 2 : 16;
        void *ids = realloc(g->ids, (size_t)cap * AURIX_ID_LEN);
        if (!ids) return -1;
        g->ids = ids;
        void *refs = realloc(g->refs, (size_t)cap * AURIX_ID_LEN);
        if (!refs) return -1;
        g->refs = refs;
        void *c = realloc(g->category, cap);
        if (!c) return -1;
        g->category = c;
        void *e = realloc(g->emb, (size_t)cap * g->dim);
        if (!e) return -1;
        g->emb = e;
        void *n = realloc(g->inv_norm, sizeof(float) * cap);
        if (!n) return -1;
        g->inv_norm = n;
        g->cap = cap;
    }
    uint32_t i = g->count++;
    memset(g->ids[i], 0, AURIX_ID_LEN);
    strncpy(g->ids[i], id, AURIX_ID_LEN - 1);
    memset(g->refs[i], 0, AURIX_ID_LEN);
    strncpy(g->refs[i], ref ? ref : "", AURIX_ID_LEN - 1);
    g->category[i] = (uint8_t)cat;
    memcpy(g->emb + (size_t)i * g->dim, emb, g->dim);
    int32_t n2 = dot_s8(emb, emb, g->dim);
    g->inv_norm[i] = n2 > 0 ? 1.0f / sqrtf((float)n2) : 0.0f;
    return 0;
}


int gallery_load(const char *path, aurix_gallery *g)
{
    memset(g, 0, sizeof(*g));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t ver, dim, count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "AURG", 4) || read_u32(f, &ver) ||
        (ver != 1 && ver != 2) || read_u32(f, &dim) || read_u32(f, &count) || dim == 0 || dim > 4096) {
        fclose(f);
        return -1;
    }
    gallery_init(g, dim);
    int8_t *e = malloc(dim);
    char id[AURIX_ID_LEN];
    if (!e) goto fail;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t cat = AURIX_CAT_ALLOW;
        if (fread(id, 1, AURIX_ID_LEN, f) != AURIX_ID_LEN) goto fail;
        if (ver == 2 && fread(&cat, 1, 1, f) != 1) goto fail;
        if (fread(e, 1, dim, f) != dim) goto fail;
        id[AURIX_ID_LEN - 1] = '\0';
        if (gallery_add(g, id, cat == AURIX_CAT_THREAT ? AURIX_CAT_THREAT : cat == AURIX_CAT_CONCERN ? AURIX_CAT_CONCERN : AURIX_CAT_ALLOW, e)) goto fail;
    }
    free(e);
    fclose(f);
    return 0;
fail:
    free(e);
    fclose(f);
    gallery_free(g);
    return -1;
}

void gallery_free(aurix_gallery *g)
{
    free(g->ids);
    free(g->refs);
    free(g->category);
    free(g->emb);
    free(g->inv_norm);
    memset(g, 0, sizeof(*g));
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-' || c == ' ') return 62;   /* ' ': '+' mangled by a web form */
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decodes standard or URL-safe base64 (padding optional). Returns bytes written or -1. */
static int b64decode(const char *s, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '=') break;
        int v = b64val((unsigned char)s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

static void trim(const char **a, const char **b)
{
    while (*a < *b && (**a == ' ' || **a == '\t' || **a == '\n' || **a == '\r')) (*a)++;
    while (*b > *a && ((*b)[-1] == ' ' || (*b)[-1] == '\t' || (*b)[-1] == '\n' || (*b)[-1] == '\r')) (*b)--;
}

int gallery_parse_param(aurix_gallery *g, const char *s, const char *kind, int *skipped_kind, int *skipped_size)
{
    int sk = 0, ss = 0;
    if (!s) return 0;
    uint32_t start = g->count;
    int added = 0;
    uint8_t buf[4096];
    while (*s) {
        const char *end = strchr(s, ';');
        if (!end) end = s + strlen(s);
        const char *a = s, *b = end;
        trim(&a, &b);
        if (a < b) {
            const char *f[4];
            size_t fl[4];
            int nf = 0;
            const char *p = a;
            while (nf < 4) {
                const char *c = (nf < 3) ? memchr(p, ',', (size_t)(b - p)) : NULL;
                const char *fe = c ? c : b;
                const char *x = p, *y = fe;
                trim(&x, &y);
                f[nf] = x;
                fl[nf] = (size_t)(y - x);
                nf++;
                if (!c) break;
                p = c + 1;
            }
            if (nf != 4 || fl[0] == 0 || fl[0] >= AURIX_ID_LEN) goto bad;
            aurix_category cat;
            if (fl[1] == 5 && !strncmp(f[1], "allow", 5)) cat = AURIX_CAT_ALLOW;
            else if (fl[1] == 6 && !strncmp(f[1], "threat", 6)) cat = AURIX_CAT_THREAT;
            else goto bad;
            int n = b64decode(f[3], fl[3], buf, sizeof(buf));
            if (n < 0) goto bad;
            if (!(strlen(kind) == fl[2] && !strncmp(f[2], kind, fl[2]))) sk++;
            else if ((uint32_t)n != g->dim) ss++;
            else {
                char id[AURIX_ID_LEN];
                memcpy(id, f[0], fl[0]);
                id[fl[0]] = '\0';
                if (gallery_add(g, id, cat, (const int8_t *)buf)) goto bad;
                added++;
            }
        }
        s = *end ? end + 1 : end;
    }
    if (skipped_kind) *skipped_kind = sk;
    if (skipped_size) *skipped_size = ss;
    return added;
bad:
    g->count = start;   /* roll back this call's additions */
    return -1;
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

unsigned long gallery_bytes(const aurix_gallery *g)
{
    if (!g || !g->cap) return 0;
    return (unsigned long)g->cap * (2 * AURIX_ID_LEN + 1 + g->dim + sizeof(float));
}

double gallery_benchmark_ns_per_entry(uint32_t dim, uint32_t entries, int rounds)
{
    aurix_gallery g;
    gallery_init(&g, dim);
    int8_t *e = malloc(dim), *q = malloc(dim);
    if (!e || !q) { free(e); free(q); return 0; }
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < dim; i++) { seed = seed * 1103515245u + 12345u; q[i] = (int8_t)((seed >> 16) % 255 - 127); }
    for (uint32_t n = 0; n < entries; n++) {
        for (uint32_t i = 0; i < dim; i++) { seed = seed * 1103515245u + 12345u; e[i] = (int8_t)((seed >> 16) % 255 - 127); }
        if (gallery_add(&g, "bench", AURIX_CAT_ALLOW, e)) break;
    }
    float score;
    volatile int sink = gallery_best(&g, q, &score);      /* warm caches */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < rounds; r++) sink += gallery_best(&g, q, &score);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    double ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / ((double)rounds * g.count);
    gallery_free(&g);
    free(e);
    free(q);
    return ns;
}

float gallery_score_for(const aurix_gallery *g, const int8_t *q, const char *key)
{
    if (!g || !g->count || !key || !*key) return -1.0f;
    int32_t qn = dot_s8(q, q, g->dim);
    if (qn <= 0) return -1.0f;
    float qinv = 1.0f / sqrtf((float)qn), best = -1.0f;
    for (uint32_t i = 0; i < g->count; i++) {
        const char *k = g->refs[i][0] ? g->refs[i] : g->ids[i];
        if (strcmp(k, key)) continue;
        float s = dot_s8(q, g->emb + (size_t)i * g->dim, g->dim) * qinv * g->inv_norm[i];
        if (s > best) best = s;
    }
    return best;
}
