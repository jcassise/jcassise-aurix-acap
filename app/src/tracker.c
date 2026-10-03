#include "tracker.h"
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void tracker_init(tracker *tr, const trk_params *p)
{
    memset(tr, 0, sizeof *tr);
    tr->next_id = 1;
    tr->p = *p;
}

void tracker_set_params(tracker *tr, const trk_params *p) { tr->p = *p; }

static void uuid4(char out[37])
{
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b)
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
    if (fd >= 0) close(fd);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static long long now_guard;

static float overlap(const trk_track *t, const float *b)
{
    float ix = fminf(t->x1, b[2]) - fmaxf(t->x0, b[0]);
    float iy = fminf(t->y1, b[3]) - fmaxf(t->y0, b[1]);
    float iou = 0;
    if (ix > 0 && iy > 0) {
        float in = ix * iy;
        iou = in / ((t->x1 - t->x0) * (t->y1 - t->y0) + (b[2] - b[0]) * (b[3] - b[1]) - in);
    }
    if (iou > 0.15f) return iou;
    /* low frame rate: the face may have moved past its old box - accept a nearby, similar-sized face,
     * but only for a track seen moments ago (a face reappearing later must overlap to be inherited) */
    if (t->misses > 2 || now_guard - t->last_seen_ms > 1000) return 0;
    float cx = (t->x0 + t->x1) / 2, cy = (t->y0 + t->y1) / 2, bx = (b[0] + b[2]) / 2, by = (b[1] + b[3]) / 2;
    float size = fmaxf(t->x1 - t->x0, t->y1 - t->y0);
    float bs = fmaxf(b[2] - b[0], b[3] - b[1]);
    float d = hypotf(cx - bx, cy - by);
    if (size <= 0 || bs / size < 0.6f || bs / size > 1.6f || d > 1.2f * size) return 0;
    return 0.15f * (1.0f - d / (1.2f * size));
}

void tracker_associate(tracker *tr, const float (*boxes)[4], int n, long long now, int *out)
{
    now_guard = now;
    for (int i = 0; i < TRK_MAX; i++) tr->t[i].seen_now = 0;
    for (int i = 0; i < n; i++) out[i] = -1;
    /* greedy: repeatedly take the best remaining (detection, track) pair */
    int used_det[64] = { 0 };
    for (;;) {
        float best = 0;
        int bd = -1, bt = -1;
        for (int d = 0; d < n && d < 64; d++) {
            if (used_det[d]) continue;
            for (int k = 0; k < TRK_MAX; k++) {
                if (!tr->t[k].active || tr->t[k].seen_now) continue;
                float s = overlap(&tr->t[k], boxes[d]);
                if (s > best) { best = s; bd = d; bt = k; }
            }
        }
        if (bd < 0) break;
        used_det[bd] = 1;
        trk_track *t = &tr->t[bt];
        const float *b = boxes[bd];
        const float a = 0.6f;                         /* box smoothing */
        t->x0 = a * b[0] + (1 - a) * t->x0; t->y0 = a * b[1] + (1 - a) * t->y0;
        t->x1 = a * b[2] + (1 - a) * t->x1; t->y1 = a * b[3] + (1 - a) * t->y1;
        if (best <= 0.15f || t->misses > 0) t->unsure = 1;    /* reattached by distance or after a gap */
        t->seen_now = 1;
        t->last_seen_ms = now;
        t->frames++;
        t->misses = 0;
        out[bd] = bt;
    }
    for (int d = 0; d < n && d < 64; d++) {
        if (out[d] >= 0) continue;
        int k = 0;
        while (k < TRK_MAX && tr->t[k].active) k++;
        if (k == TRK_MAX) continue;
        trk_track *t = &tr->t[k];
        memset(t, 0, sizeof *t);
        t->active = 1;
        t->id = tr->next_id++;
        uuid4(t->event_id);
        t->started_ms = t->last_seen_ms = now;
        t->x0 = boxes[d][0]; t->y0 = boxes[d][1]; t->x1 = boxes[d][2]; t->y1 = boxes[d][3];
        t->seen_now = 1;
        t->frames = 1;
        out[d] = k;
    }
    for (int k = 0; k < TRK_MAX; k++)
        if (tr->t[k].active && !tr->t[k].seen_now) tr->t[k].misses++;
}

int tracker_wants_embed(const tracker *tr, int ti, long long now)
{
    const trk_track *t = &tr->t[ti];
    if (!t->active || !t->seen_now) return 0;
    if (t->state != TS_KNOWN || t->unsure) return 2;     /* undecided, or needs re-checking now */
    return now - t->last_embed_ms >= tr->p.reverify_ms ? 1 : 0;
}

static float self_similarity(const trk_track *t, const int8_t *emb, int dim, float *qn)
{
    double n2 = 0;
    for (int i = 0; i < dim; i++) n2 += (double)emb[i] * emb[i];
    float inv = n2 > 0 ? (float)(1.0 / sqrt(n2)) : 0;
    double d = 0;
    for (int i = 0; i < dim; i++) { qn[i] = emb[i] * inv; d += (double)qn[i] * t->print[i]; }
    return t->has_print ? (float)d : 1.0f;
}

static void update_print(trk_track *t, const float *qn, int dim)
{
    if (!t->has_print) { memcpy(t->print, qn, sizeof(float) * (size_t)dim); t->has_print = 1; return; }
    double n2 = 0;
    for (int i = 0; i < dim; i++) { t->print[i] = 0.8f * t->print[i] + 0.2f * qn[i]; n2 += (double)t->print[i] * t->print[i]; }
    float inv = n2 > 0 ? (float)(1.0 / sqrt(n2)) : 0;
    for (int i = 0; i < dim; i++) t->print[i] *= inv;
}


/* The same person seen again: continue their event instead of opening a new one.
 * Known: same identity key. Stranger: appearance print clearly the same face. */
static int same_person(const trk_track *t, const trk_track *o, float same_face)
{
    if (o->revision == 0 || o->state != t->state) return 0;
    if (t->state == TS_KNOWN) return !strcmp(o->key, t->key);
    if (!t->has_print || !o->has_print) return 0;
    double d = 0;
    for (int i = 0; i < TRK_DIM; i++) d += (double)t->print[i] * o->print[i];
    return d >= same_face + 0.20f;                 /* stricter than track-following: no gallery to back it */
}

static void take_over(trk_track *t, const trk_track *o)
{
    memcpy(t->event_id, o->event_id, sizeof t->event_id);
    t->revision = o->revision;
    t->started_ms = o->started_ms;
    if (o->best_quality > t->best_quality) t->best_quality = o->best_quality;
    if (o->best_score > t->best_score) t->best_score = o->best_score;
    t->better_face = 0;
}

static void adopt(tracker *tr, int ti)
{
    trk_track *t = &tr->t[ti];
    for (int k = 0; k < TRK_MAX; k++) {           /* an older track of the same person that lost the face */
        trk_track *o = &tr->t[k];
        if (k == ti || !o->active || o->seen_now || !same_person(t, o, tr->p.same_face)) continue;
        take_over(t, o);
        o->active = 0;
        return;
    }
    for (int k = 0; k < tr->nrecent; k++) {        /* a detached track (split) of the same person */
        if (!same_person(t, &tr->recent[k], tr->p.same_face)) continue;
        take_over(t, &tr->recent[k]);
        tr->recent[k] = tr->recent[--tr->nrecent];
        return;
    }
}

int tracker_observe(tracker *tr, int ti, const int8_t *emb, int dim, const char *bkey, const char *bname,
                    const char *bref, int bcat, float bscore, float own, float quality, int face_px, long long now)
{
    trk_track *t = &tr->t[ti];
    const trk_params *p = &tr->p;
    t->last_embed_ms = now;
    t->face_px = face_px;
    if (dim > TRK_DIM) dim = TRK_DIM;
    float qn[TRK_DIM];
    float self = emb && dim > 0 ? self_similarity(t, emb, dim, qn) : 1.0f;
    t->last_self = self;
    int same_face = self >= p->same_face;
    int confident = bkey && *bkey && bscore >= p->lock_thr;

    /* a different face in this track: two checks in a row and it is someone else, whatever the gallery says */
    if (!same_face) {
        t->unsure = 1;
        if (++t->different >= 2) return 1;
        return 0;                                    /* one odd frame (blur): look again next frame */
    }
    t->different = 0;
    if (emb && dim > 0) update_print(t, qn, dim);
    if (quality > t->best_quality * 1.15f + 0.02f) {
        if (t->revision) t->better_face = 1;
        t->best_quality = quality;
    }

    if (t->state == TS_KNOWN) {
        if (own >= 0) t->score = own;
        if (own > t->best_score) t->best_score = own;
        if (own >= p->keep_thr) { t->split_count = 0; t->unsure = 0; return 0; }
        if (confident && strcmp(bkey, t->key)) {
            if (!strcmp(t->split_key, bkey)) t->split_count++;
            else { snprintf(t->split_key, sizeof t->split_key, "%s", bkey); t->split_count = 1; }
            return t->split_count >= p->split_hits;
        }
        t->unsure = 0;                               /* weak but still the same face (turned head): keep the name */
        return 0;
    }

    t->unsure = 0;
    if (confident) {
        if (!strcmp(t->cand_key, bkey)) t->hits++;
        else {
            snprintf(t->cand_key, sizeof t->cand_key, "%s", bkey);
            snprintf(t->cand_name, sizeof t->cand_name, "%s", bname ? bname : bkey);
            snprintf(t->cand_ref, sizeof t->cand_ref, "%s", bref ? bref : "");
            t->cand_cat = bcat;
            t->hits = 1;
        }
        if (bscore > t->cand_score) t->cand_score = bscore;
        if (t->hits >= p->lock_hits) {
            t->state = TS_KNOWN;
            memcpy(t->key, t->cand_key, sizeof t->key);
            memcpy(t->name, t->cand_name, sizeof t->name);
            memcpy(t->ref, t->cand_ref, sizeof t->ref);
            t->category = t->cand_cat;
            t->score = bscore;
            t->best_score = t->cand_score;
            t->needs_emit = 1;
            adopt(tr, ti);
        }
        return 0;
    }
    if (t->state == TS_PENDING && ++t->unknown_frames >= p->stranger_after && t->hits == 0) {
        t->state = TS_STRANGER;
        t->best_score = bscore;
        t->needs_emit = 1;
        adopt(tr, ti);
    }
    if (bscore > t->best_score && t->state != TS_KNOWN) t->best_score = bscore;
    return 0;
}

void tracker_close(tracker *tr, int ti, long long now, trk_track *closed)
{
    trk_track *t = &tr->t[ti];
    t->ended_ms = t->last_seen_ms ? t->last_seen_ms : now;
    if (closed) *closed = *t;
    t->active = 0;
}

void tracker_detach(tracker *tr, int ti, long long now)
{
    trk_track *t = &tr->t[ti];
    t->active = 0;
    if (t->revision == 0) return;                  /* nothing was reported: nothing to continue */
    if (tr->nrecent == TRK_MAX) {                  /* full: the oldest is closed by expire anyway soon */
        int o = 0;
        for (int k = 1; k < tr->nrecent; k++) if (tr->recent[k].last_seen_ms < tr->recent[o].last_seen_ms) o = k;
        tr->recent[o] = tr->recent[--tr->nrecent];
    }
    tr->recent[tr->nrecent] = *t;
    if (!tr->recent[tr->nrecent].last_seen_ms) tr->recent[tr->nrecent].last_seen_ms = now;
    tr->nrecent++;
}

int tracker_expire(tracker *tr, long long now, trk_track *closed, int max)
{
    int n = 0;
    for (int k = 0; k < tr->nrecent;) {
        trk_track *r = &tr->recent[k];
        if (now - r->last_seen_ms < tr->p.close_ms) { k++; continue; }
        r->ended_ms = r->last_seen_ms;
        if (n < max) closed[n++] = *r;
        tr->recent[k] = tr->recent[--tr->nrecent];
    }
    for (int k = 0; k < TRK_MAX; k++) {
        trk_track *t = &tr->t[k];
        if (!t->active || now - t->last_seen_ms < tr->p.close_ms) continue;
        if (n < max) tracker_close(tr, k, now, &closed[n++]);
        else t->active = 0;
    }
    return n;
}
