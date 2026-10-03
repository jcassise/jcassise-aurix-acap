/* AURIX - face tracker: one track per person in view, one event per visit (pure C, host-testable).
 *   pending  -> known     after `lock_hits` confident matches to the same person
 *   pending  -> stranger  after `stranger_after` embedded frames with no confident match
 *   stranger -> known     if the person is recognised later in the same visit (same event, updated)
 *   known keeps its name while the face stays plausibly the same (>= keep_thr) or is just weak
 *   (turned head); it splits only if it matches someone else confidently `split_hits` times in a row. */
#ifndef AURIX_TRACKER_H
#define AURIX_TRACKER_H

#define TRK_MAX 16
#define TRK_KEY 64

typedef struct {
    float lock_thr, keep_thr;
    int lock_hits, stranger_after, split_hits;
    long long close_ms, reverify_ms;
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
} trk_track;

typedef struct {
    trk_track t[TRK_MAX];
    unsigned next_id;
    trk_params p;
} tracker;

void tracker_init(tracker *tr, const trk_params *p);
void tracker_set_params(tracker *tr, const trk_params *p);

/* boxes[i] = {x0,y0,x1,y1}; out_track[i] = track index (new tracks are created as needed; -1 if full). */
void tracker_associate(tracker *tr, const float (*boxes)[4], int n, long long now_ms, int *out_track);

/* Should track ti be embedded this frame? 2 = needs identity, 1 = due for re-verification, 0 = no. */
int tracker_wants_embed(const tracker *tr, int ti, long long now_ms);

/* Identity evidence. best_key "" = no gallery entry. own_score = score of the locked identity (-1 if none).
 * Returns 1 if the track must split (it now confidently matches someone else). */
int tracker_observe(tracker *tr, int ti, const char *best_key, const char *best_name, const char *best_ref,
                    int best_cat, float best_score, float own_score, float quality, int face_px, long long now_ms);

/* Closes tracks unseen for close_ms; copies them into closed[] (ended_ms set). Returns how many. */
int tracker_expire(tracker *tr, long long now_ms, trk_track *closed, int max);

/* Closes one track now (split); copies it into *closed. */
void tracker_close(tracker *tr, int ti, long long now_ms, trk_track *closed);

#endif
