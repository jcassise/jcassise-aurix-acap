/* AURIX - events for Pharos (protocol v1 §7): which tracks are reported, and their JSON. Pure C + jansson. */
#ifndef AURIX_EVENTS_H
#define AURIX_EVENTS_H
#include <jansson.h>
#include "access.h"
#include "pharos_config.h"
#include "tracker.h"

typedef struct {
    const char *device_id, *model_version, *zone;   /* zone: first of site.zones, NULL if none */
    float threshold;
    const char *watchlist;                          /* no_concern | concern | threat (known tracks) */
    int has_access;                                 /* virtual access: decision below applies */
    access_result access;
    int has_face, has_scene;
    long long clock_offset_ms;                      /* added to camera times (Pharos time) */
} event_ctx;

/* Should this track be reported under the device's role and reporting.* settings? */
int event_should_report(const trk_track *t, const pc_config *cfg, const char *watchlist);

/* Eviction priority in the offline queue: higher survives longer. */
int event_priority(const trk_track *t, const char *watchlist);

/* Full Event body for PUT /events/{eventId} at `revision`. ended = closing update. */
json_t *event_build(const trk_track *t, int revision, int ended, long long now_ms, const event_ctx *c);

#endif
