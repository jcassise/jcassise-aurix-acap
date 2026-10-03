/* AURIX - live performance metrics for the on-camera dashboard (thread-safe, pure C + jansson). */
#ifndef AURIX_METRICS_H
#define AURIX_METRICS_H
#include <jansson.h>
#include "sysinfo.h"

typedef enum { ST_CAPTURE = 0, ST_DETECT, ST_ALIGN, ST_EMBED, ST_MATCH, ST_COUNT } metrics_stage;

#define METRICS_HISTORY 120          /* samples kept (at 5 s: 10 minutes) */
#define METRICS_RECENT 20            /* recent matches kept */

typedef struct {
    const char *app_version, *chip, *device, *embed_kind, *model_version;
    const char *hw_model, *serial, *firmware;
    double target_fps;
    int frame_w, frame_h;
} metrics_identity;

void metrics_init(const metrics_identity *id);

/* From the pipeline thread. ms = time spent; count = how many times in this frame (per-face stages). */
void metrics_frame(void);
void metrics_stage_time(metrics_stage s, double ms);
void metrics_faces(int detected, int gated, int embedded);
void metrics_match(const char *name, int threat, float score, int matched);

/* From anywhere. */
void metrics_gallery(int total, int allow, int threat, const char *source, unsigned long ram_bytes);
/* Measured cost of comparing one face against one gallery entry (startup benchmark). */
void metrics_match_cost(double ns_per_entry, unsigned bench_entries);
void metrics_recognition(double threshold, double min_eye_px, const char *mode);
void metrics_pharos(const char *state, const char *detail, long long config_rev);
void metrics_overlay(double avg_render_ms, double renders_per_s);
void metrics_sync(long long revision, long long last_ok_ms, int people, int ready, int failed, int pending);

/* Roll the current window into history (call every `interval_s`, e.g. 5 s). */
void metrics_sample(double interval_s, const char *storage_path);

/* Full dashboard document. Caller frees. */
json_t *metrics_json(void);

#endif
