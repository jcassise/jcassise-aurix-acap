#include "yunet.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YUNET_MAX_CANDIDATES 512

static const int STRIDES[YUNET_LEVELS] = { 8, 16, 32 };
static const int CHANNELS[YUNET_ROLES] = { 1, 1, 4, 10 };

static int role_of(const char *s)
{
    static const char *names[YUNET_ROLES] = { "cls", "obj", "bbox", "kps" };
    for (int i = 0; i < YUNET_ROLES; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

static int level_of(int stride)
{
    for (int i = 0; i < YUNET_LEVELS; i++)
        if (STRIDES[i] == stride) return i;
    return -1;
}

int yunet_meta_load(const char *path, yunet_meta *m)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(m, 0, sizeof(*m));
    int seen = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char role[16];
        int idx, stride, zp;
        float scale;
        if (sscanf(line, "input %d %d", &m->in_w, &m->in_h) == 2) continue;
        if (sscanf(line, "out %d %15s %d %f %d", &idx, role, &stride, &scale, &zp) == 5) {
            int r = role_of(role), l = level_of(stride);
            if (r < 0 || l < 0 || idx < 0 || idx >= YUNET_OUTPUTS || m->t[l][r].scale != 0.0f) {
                fclose(f);
                return -1;
            }
            m->t[l][r] = (yunet_tensor){ idx, scale, zp };
            seen++;
        }
    }
    fclose(f);
    if (seen != YUNET_OUTPUTS || m->in_w <= 0 || m->in_h <= 0 || m->in_w % 32 || m->in_h % 32)
        return -1;
    return 0;
}

static inline float deq(const yunet_tensor *t, int8_t v) { return (v - t->zero_point) * t->scale; }

static float iou(const aurix_face *a, const aurix_face *b)
{
    float ix = fminf(a->x1, b->x1) - fmaxf(a->x0, b->x0);
    float iy = fminf(a->y1, b->y1) - fmaxf(a->y0, b->y0);
    if (ix <= 0 || iy <= 0) return 0.0f;
    float inter = ix * iy;
    float u = (a->x1 - a->x0) * (a->y1 - a->y0) + (b->x1 - b->x0) * (b->y1 - b->y0) - inter;
    return u > 0 ? inter / u : 0.0f;
}

static int by_score_desc(const void *a, const void *b)
{
    float sa = ((const aurix_face *)a)->score, sb = ((const aurix_face *)b)->score;
    return (sa < sb) - (sa > sb);
}

int yunet_decode(const yunet_meta *m, const int8_t *const *outputs,
                 aurix_face *faces, int max, float thr, float nms_iou)
{
    static aurix_face cand[YUNET_MAX_CANDIDATES];
    int n = 0;

    for (int l = 0; l < YUNET_LEVELS; l++) {
        const int st = STRIDES[l], cols = m->in_w / st, rows = m->in_h / st;
        const yunet_tensor *tc = &m->t[l][YUNET_CLS], *to = &m->t[l][YUNET_OBJ];
        const yunet_tensor *tb = &m->t[l][YUNET_BBOX], *tk = &m->t[l][YUNET_KPS];
        const int8_t *cls = outputs[tc->index], *obj = outputs[to->index];
        const int8_t *bb = outputs[tb->index], *kp = outputs[tk->index];

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                const int i = r * cols + c;
                float pc = deq(tc, cls[i]), po = deq(to, obj[i]);
                pc = pc < 0 ? 0 : pc > 1 ? 1 : pc;
                po = po < 0 ? 0 : po > 1 ? 1 : po;
                float score = sqrtf(pc * po);
                if (score < thr) continue;
                if (n == YUNET_MAX_CANDIDATES) {   /* keep the best: replace current weakest */
                    int w = 0;
                    for (int k = 1; k < n; k++) if (cand[k].score < cand[w].score) w = k;
                    if (cand[w].score >= score) continue;
                    n--;
                    cand[w] = cand[n];
                }
                const int8_t *b = bb + (size_t)i * CHANNELS[YUNET_BBOX];
                const int8_t *k = kp + (size_t)i * CHANNELS[YUNET_KPS];
                float cx = (c + deq(tb, b[0])) * st, cy = (r + deq(tb, b[1])) * st;
                float w = expf(deq(tb, b[2])) * st, h = expf(deq(tb, b[3])) * st;
                aurix_face *f = &cand[n++];
                f->score = score;
                f->x0 = cx - w / 2; f->y0 = cy - h / 2;
                f->x1 = cx + w / 2; f->y1 = cy + h / 2;
                for (int j = 0; j < 5; j++) {
                    f->lm.x[j] = (c + deq(tk, k[2 * j])) * st;
                    f->lm.y[j] = (r + deq(tk, k[2 * j + 1])) * st;
                }
            }
        }
    }

    qsort(cand, (size_t)n, sizeof(cand[0]), by_score_desc);
    int kept = 0;
    for (int i = 0; i < n && kept < max; i++) {
        int ok = 1;
        for (int j = 0; j < kept; j++)
            if (iou(&cand[i], &faces[j]) > nms_iou) { ok = 0; break; }
        if (ok) faces[kept++] = cand[i];
    }
    return kept;
}
