#include "events.h"
#include <string.h>

static int is_watch(const char *wl) { return wl && (!strcmp(wl, "threat") || !strcmp(wl, "concern")); }

int event_should_report(const trk_track *t, const pc_config *cfg, const char *wl)
{
    if (t->state == TS_PENDING) return 0;                       /* not decided yet */
    if (cfg->role == PC_ROLE_VIRTUAL_ACCESS) return 1;           /* every decision is an access event */
    if (t->state == TS_STRANGER) return cfg->strangers || cfg->mode == PC_MODE_ALL;
    if (is_watch(wl)) return 1;                                  /* Threat and Concern: always */
    return cfg->mode != PC_MODE_WATCHLIST || cfg->no_concern_sightings;
}

int event_priority(const trk_track *t, const char *wl)
{
    if (t->state == TS_STRANGER) return 1;
    if (t->state == TS_PENDING) return 0;
    return is_watch(wl) ? 3 : 2;
}

static json_t *r2(double v) { return json_real((double)(long long)(v * 1000 + (v >= 0 ? 0.5 : -0.5)) / 1000.0); }

json_t *event_build(const trk_track *t, int revision, int ended, long long now, const event_ctx *c)
{
    const long long off = c->clock_offset_ms;
    const int access = c->has_access;
    json_t *e = json_object();
    json_object_set_new(e, "eventId", json_string(t->event_id));
    json_object_set_new(e, "revision", json_integer(revision));
    json_object_set_new(e, "deviceId", json_string(c->device_id));
    json_object_set_new(e, "streamId", json_string("main"));
    json_object_set_new(e, "kind", json_string(access ? "access" : "sighting"));
    json_object_set_new(e, "startedAt", json_integer(t->started_ms + off));
    json_object_set_new(e, "updatedAt", json_integer(now + off));
    json_object_set_new(e, "endedAt", ended ? json_integer((t->ended_ms ? t->ended_ms : now) + off) : json_null());
    json_object_set_new(e, "zone", c->zone ? json_string(c->zone) : json_null());
    if (t->state == TS_KNOWN && t->ref[0]) {
        json_object_set_new(e, "person", json_pack("{s:s,s:s,s:s}", "personId", t->ref, "displayName", t->name,
                                                   "watchlist", c->watchlist ? c->watchlist : "no_concern"));
        json_object_set_new(e, "stranger", json_false());
    } else {
        json_object_set_new(e, "person", json_null());
        /* a local test enrolment (no Pharos id) is recognised but cannot be linked: not a stranger */
        json_object_set_new(e, "stranger", json_boolean(t->state == TS_STRANGER));
    }
    json_t *m = json_pack("{s:o,s:o,s:s}", "score", r2(t->best_score < 0 ? 0 : t->best_score > 1 ? 1 : t->best_score),
                          "threshold", r2(c->threshold), "modelVersion", c->model_version);
    json_object_set_new(e, "match", m);
    json_object_set_new(e, "liveness", json_pack("{s:b,s:n,s:n}", "checked", 0, "passed", "score"));
    if (access)
        json_object_set_new(e, "access", json_pack("{s:b,s:s,s:s,s:o}", "evaluated", 1,
            "decision", c->access.granted ? "granted" : "denied", "reason", c->access.reason,
            "policyId", c->access.policy_id[0] ? json_string(c->access.policy_id) : json_null()));
    else
        json_object_set_new(e, "access", json_pack("{s:b,s:n,s:n,s:n}", "evaluated", 0, "decision", "reason", "policyId"));
    json_t *face = json_pack("{s:o,s:i}", "quality", r2(t->best_quality > 1 ? 1 : t->best_quality), "sizePx", t->face_px);
    if (t->has_face_geo) {                                  /* estimated from the landmarks */
        json_object_set_new(face, "yawDeg", json_integer((json_int_t)(t->yaw_deg >= 0 ? t->yaw_deg + 0.5f : t->yaw_deg - 0.5f)));
        json_object_set_new(face, "pitchDeg", json_integer((json_int_t)(t->pitch_deg >= 0 ? t->pitch_deg + 0.5f : t->pitch_deg - 0.5f)));
    }
    json_object_set_new(e, "face", face);
    json_object_set_new(e, "images", json_pack("{s:b,s:b}", "face", c->has_face, "scene", c->has_scene));
    return e;
}
