#include "pharos_config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *MODES[] = { "watchlist", "enrolled", "all" };
const char *pc_mode_name(pc_mode m) { return MODES[m]; }

/* Keys this AURIX build knows but cannot honour (no relay output, no local UI lock yet). */
static const char *UNSUPPORTED_KNOWN[] = { "relay.enabled", "relay.pulseMs", "device.localUiEnabled", NULL };

void pc_defaults(pc_config *c, double thr)
{
    memset(c, 0, sizeof *c);
    c->mode = PC_MODE_WATCHLIST;
    c->strangers = true;
    c->match_threshold = thr;
    c->min_face_px = 90;             /* ~40 px between the eyes */
    c->track_close_sec = 3;
    c->scene_images = true;
    c->sync_interval_ms = 3000;
    c->keep_all_people = true;
    c->max_delta_age_sec = 1296000;
    c->download_photos = true;
}

static void reject(json_t *rej, const char *key, const char *reason)
{
    json_array_append_new(rej, json_pack("{s:s, s:s}", "key", key, "reason", reason));
}

static int get_bool(const json_t *v, bool *out)
{
    if (!json_is_boolean(v)) return -1;
    *out = json_is_true(v);
    return 0;
}

static int get_int(const json_t *v, long long lo, long long hi, long long *out)
{
    if (!json_is_integer(v)) return -1;
    long long x = json_integer_value(v);
    if (x < lo || x > hi) return -2;
    *out = x;
    return 0;
}

static int tz_known(const char *tz)
{
    if (!*tz) return 1;
    if (strstr(tz, "..") || tz[0] == '/') return 0;
    char p[200];
    snprintf(p, sizeof p, "/usr/share/zoneinfo/%s", tz);
    return access(p, R_OK) == 0;
}

void pc_apply(const json_t *desired, const pc_config *prev, double thr, pc_config *o,
              json_t **applied, json_t **rejected, json_t **unsupported)
{
    json_t *rej = json_array(), *uns = json_array();
    pc_defaults(o, thr);
    const char *key;
    json_t *v;
    char why[96];

    if (json_is_object(desired)) json_object_foreach((json_t *)desired, key, v) {
        bool b;
        long long n;
        int rc;
#define BOOLKEY(K, F)                                                   \
        if (!strcmp(key, K)) {                                          \
            if (get_bool(v, &b)) { reject(rej, key, "expected boolean"); o->F = prev->F; } \
            else o->F = b;                                              \
            continue;                                                   \
        }
        BOOLKEY("reporting.strangers", strangers)
        BOOLKEY("reporting.noConcernSightings", no_concern_sightings)
        BOOLKEY("reporting.unrecognizable", unrecognizable)
        BOOLKEY("events.sceneImages", scene_images)
        BOOLKEY("sync.keepAllPeople", keep_all_people)
        BOOLKEY("sync.downloadPhotos", download_photos)
        BOOLKEY("access.reportDecisions", report_decisions)
#undef BOOLKEY
#define INTKEY(K, F, LO, HI)                                            \
        if (!strcmp(key, K)) {                                          \
            rc = get_int(v, LO, HI, &n);                                \
            if (rc == -1) { reject(rej, key, "expected integer"); o->F = prev->F; } \
            else if (rc == -2) { snprintf(why, sizeof why, "out of range %lld..%lld", (long long)(LO), (long long)(HI)); \
                                 reject(rej, key, why); o->F = prev->F; } \
            else o->F = n;                                              \
            continue;                                                   \
        }
        INTKEY("recognition.minFaceSizePx", min_face_px, 40, 400)
        INTKEY("events.trackCloseSec", track_close_sec, 1, 600)
        INTKEY("sync.intervalMs", sync_interval_ms, 500, 3600000)
        INTKEY("sync.maxDeltaAgeSec", max_delta_age_sec, 0, 315360000)
#undef INTKEY
        if (!strcmp(key, "recognition.matchThreshold")) {
            if (!json_is_number(v)) { reject(rej, key, "expected number"); o->match_threshold = prev->match_threshold; }
            else {
                double x = json_number_value(v);
                if (x < 0.2 || x > 0.95) { reject(rej, key, "out of range 0.20..0.95 for this model"); o->match_threshold = prev->match_threshold; }
                else o->match_threshold = x;
            }
            continue;
        }
        if (!strcmp(key, "recognition.liveness")) {
            if (get_bool(v, &b)) { reject(rej, key, "expected boolean"); }
            else if (b) { reject(rej, key, "liveness is not available on this device"); }
            o->liveness = false;
            continue;
        }
        if (!strcmp(key, "reporting.mode")) {
            int m = -1;
            if (json_is_string(v))
                for (int i = 0; i < 3; i++) if (!strcmp(json_string_value(v), MODES[i])) m = i;
            if (m < 0) { reject(rej, key, "expected watchlist, enrolled or all"); o->mode = prev->mode; }
            else o->mode = (pc_mode)m;
            continue;
        }
        if (!strcmp(key, "site.timeZone")) {
            const char *tz = json_is_string(v) ? json_string_value(v) : NULL;
            if (!tz || strlen(tz) >= sizeof o->time_zone) { reject(rej, key, "expected IANA time zone name"); snprintf(o->time_zone, sizeof o->time_zone, "%s", prev->time_zone); }
            else if (!tz_known(tz)) { reject(rej, key, "unknown time zone on this device"); snprintf(o->time_zone, sizeof o->time_zone, "%s", prev->time_zone); }
            else snprintf(o->time_zone, sizeof o->time_zone, "%s", tz);
            continue;
        }
        if (!strcmp(key, "site.zones")) {
            int ok = json_is_array(v) && json_array_size(v) <= PC_MAX_ZONES;
            size_t i;
            json_t *z;
            if (ok) json_array_foreach(v, i, z)
                if (!json_is_string(z) || !json_string_length(z) || json_string_length(z) >= PC_ZONE_LEN) ok = 0;
            if (!ok) {
                reject(rej, key, "expected up to 16 zone names (1..127 chars)");
                memcpy(o->zones, prev->zones, sizeof o->zones);
                o->nzones = prev->nzones;
            } else {
                o->nzones = 0;
                json_array_foreach(v, i, z) snprintf(o->zones[o->nzones++], PC_ZONE_LEN, "%s", json_string_value(z));
            }
            continue;
        }
        json_array_append_new(uns, json_string(key));   /* unknown or known-unsupported */
    }
    (void)UNSUPPORTED_KNOWN;
    *applied = pc_to_json(o);
    *rejected = rej;
    *unsupported = uns;
}

json_t *pc_to_json(const pc_config *c)
{
    json_t *z = json_array();
    for (int i = 0; i < c->nzones; i++) json_array_append_new(z, json_string(c->zones[i]));
    json_t *o = json_object();
    json_object_set_new(o, "site.zones", z);
    if (c->time_zone[0]) json_object_set_new(o, "site.timeZone", json_string(c->time_zone));
    json_object_set_new(o, "reporting.mode", json_string(MODES[c->mode]));
    json_object_set_new(o, "reporting.strangers", json_boolean(c->strangers));
    json_object_set_new(o, "reporting.noConcernSightings", json_boolean(c->no_concern_sightings));
    json_object_set_new(o, "reporting.unrecognizable", json_boolean(c->unrecognizable));
    json_object_set_new(o, "recognition.matchThreshold", json_real((double)(int)(c->match_threshold * 1000 + 0.5) / 1000.0));
    json_object_set_new(o, "recognition.minFaceSizePx", json_integer(c->min_face_px));
    json_object_set_new(o, "recognition.liveness", json_boolean(c->liveness));
    json_object_set_new(o, "events.trackCloseSec", json_integer(c->track_close_sec));
    json_object_set_new(o, "events.sceneImages", json_boolean(c->scene_images));
    json_object_set_new(o, "sync.intervalMs", json_integer(c->sync_interval_ms));
    json_object_set_new(o, "sync.keepAllPeople", json_boolean(c->keep_all_people));
    json_object_set_new(o, "sync.maxDeltaAgeSec", json_integer(c->max_delta_age_sec));
    json_object_set_new(o, "sync.downloadPhotos", json_boolean(c->download_photos));
    json_object_set_new(o, "access.reportDecisions", json_boolean(c->report_decisions));
    return o;
}
