/* AURIX - face tracker: one track per person in view, one event per visit (pure C, host-testable).
 *   pending  -> known     after `lock_hits` confident matches to the same person
 *   pending  -> stranger  after `stranger_after` embedded frames with no confident match
 *   stranger -> known     if the person is recognised later in the same visit (same event, updated)
 *   known keeps its name while the face stays plausibly the same (>= keep_thr) or is just weak
 *   (turned head) - but only while the face still looks like the face this track has been following.
 * Every track keeps an appearance print (running mean of its embeddings). A face that does not resemble
 * it (two checks in a row) is a different person: the track splits, whatever the gallery says. This is
 * what stops a name jumping to another face when people cross or photos are swapped. */
#ifndef AURIX_TRACKER_H
#define AURIX_TRACKER_H

#include <stdint.h>

#define TRK_MAX 16
#define TRK_KEY 64
#define TRK_DIM 128

typedef struct {
    float lock_thr, keep_thr;
    int lock_hits, stranger_after, split_hits;
    long long close_ms, reverify_ms;
    float same_face;             /* min similarity to the track's own print to count as the same face.
                                  * 0.30, measured on a real recording (photos swapped in front of the camera):
                                  * swapped faces scored 0.07-0.46 against the other track's print, the same
                                  * face 0.55-0.73, a moving head median 0.90; with two checks in a row both
                                  * swap directions were caught and 1 of 270 frames of a moving person split. */
} trk_params;

typedef enum { TS_PENDING = 0, TS_KNOWN, TS_STRANGER } trk_state;

typedef struct {
    int active;
    unsigned id;
    char event_id[37];
    int revision;                 /* last revision emitted, 0 = none yet */
    int needs_emit;               /* identity/state changed since the last emit */
    int better_face;              /* a clearly better face since the last emit */
    long long started_ms, last_seen_ms, last_embed_ms, ended_ms;
    float x0, y0, x1, y1;         /* smoothed box, frame pixels */
    int seen_now, frames, misses;
    trk_state state;
    char key[TRK_KEY], name[TRK_KEY], ref[TRK_KEY];
    int category;                 /* AURIX_CAT_* of the locked identity */
    float score, best_score;
    int hits;
    char cand_key[TRK_KEY], cand_name[TRK_KEY], cand_ref[TRK_KEY];
    int cand_cat;
    float cand_score;
    int unknown_frames, split_count;
    char split_key[TRK_KEY];
    float best_quality;
    int face_px;
    float print[TRK_DIM];         /* appearance print: L2-normalised running mean of embeddings */
    int has_print;
    int unsure;                   /* re-check every frame: reattached after a gap or by distance, or doubtful */
    int different;                /* consecutive checks where the face did not match the print */
    float last_self;              /* similarity of the last check to the print (diagnostics) */
} trk_track;

typedef struct {
    trk_track t[TRK_MAX];
    trk_track recent[TRK_MAX];    /* detached tracks whose event may continue (same person back within close_ms) */
    int nrecent;
    unsigned next_id;
    trk_params p;
} tracker;

void tracker_init(tracker *tr, const trk_params *p);
void tracker_set_params(tracker *tr, const trk_params *p);

/* boxes[i] = {x0,y0,x1,y1}; out_track[i] = track index (new tracks are created as needed; -1 if full). */
void tracker_associate(tracker *tr, const float (*boxes)[4], int n, long long now_ms, int *out_track);

/* Should track ti be embedded this frame? 2 = needs identity, 1 = due for re-verification, 0 = no. */
int tracker_wants_embed(const tracker *tr, int ti, long long now_ms);

/* Identity evidence. emb = this frame's embedding. best_key "" = no gallery entry. own_score = score of
 * the locked identity (-1 if none). Returns 1 if the track must split (a different face, or confidently
 * someone else); the caller closes it and the face starts a fresh track next frame. */
int tracker_observe(tracker *tr, int ti, const int8_t *emb, int dim, const char *best_key, const char *best_name,
                    const char *best_ref, int best_cat, float best_score, float own_score, float quality, int face_px,
                    long long now_ms);

/* Closes tracks unseen for close_ms (and detached ones past their window); copies them into closed[]
 * (ended_ms set). Returns how many. */
int tracker_expire(tracker *tr, long long now_ms, trk_track *closed, int max);

/* Closes one track now; copies it into *closed. */
void tracker_close(tracker *tr, int ti, long long now_ms, trk_track *closed);

/* Detaches a track after a split: its event is not closed yet. If the same person (by name, or by
 * appearance for strangers) is identified again within close_ms, the new track continues that event;
 * otherwise tracker_expire closes it. */
void tracker_detach(tracker *tr, int ti, long long now_ms);

#endif
