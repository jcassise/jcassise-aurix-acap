#include "settings.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *group, *key, *label, *type, *unit, *options, *help;
    double min, max, step;
} setting_def;

/* Order = order on the console. type: number | integer | boolean | choice */
static const setting_def DEFS[] = {
    { "Recognition", "recognition.matchThreshold", "Match threshold", "number", "", NULL,
      "Similarity needed to name someone. Higher: fewer false matches, more people left unnamed.", 0.30, 0.80, 0.01 },
    { "Recognition", "recognition.minFaceSizePx", "Smallest face to identify", "integer", "px", NULL,
      "Face width in the analysis image (about 2.25 times the distance between the eyes). Smaller faces are tracked but not named.", 40, 400, 5 },
    { "Recognition", "recognition.minDetectScore", "Face detection confidence", "number", "", NULL,
      "How sure the detector must be that it sees a face. Partly covered faces (hands, masks, hats) score lower: raise to ignore them, lower to catch them.", 0.30, 0.95, 0.05 },
    { "Recognition", "recognition.maxYawDeg", "Maximum head turn (left/right)", "integer", "°", NULL,
      "Identify only faces turned less than this from the camera (estimated from the eyes and nose). Turned faces keep their name while tracked.", 15, 90, 5 },
    { "Recognition", "recognition.maxPitchDeg", "Maximum head tilt (up/down)", "integer", "°", NULL,
      "Identify only faces tilted less than this (estimated). Ceiling cameras see faces from above: allow more tilt there.", 15, 90, 5 },
    { "Recognition", "recognition.maxFaces", "Faces per frame", "integer", "", NULL,
      "Most faces handled in one frame.", 1, 16, 1 },
    { "Recognition", "recognition.identifyPerFrame", "Identifications per frame", "integer", "", NULL,
      "Faces identified per frame; the rest wait for the next frame. 0 = this camera's default (4 on ARTPEC-8, 1 on ARTPEC-7).", 0, 8, 1 },
    { "Tracking", "tracking.lockFrames", "Confident frames to lock a name", "integer", "", NULL,
      "Matches in a row before a name is shown and an event is sent.", 1, 5, 1 },
    { "Tracking", "tracking.strangerFrames", "Frames before 'stranger'", "integer", "", NULL,
      "Unmatched checks before a face is reported as a stranger.", 2, 10, 1 },
    { "Tracking", "tracking.keepMargin", "Keep-name margin", "number", "", NULL,
      "Once named, a face keeps its name down to the match threshold minus this (head turns, poor light).", 0.0, 0.30, 0.01 },
    { "Tracking", "tracking.sameFace", "Same-face similarity", "number", "", NULL,
      "How closely a face must resemble the face a track has been following. Below this (twice) it is someone else: stops names jumping between people who cross.", 0.15, 0.60, 0.01 },
    { "Tracking", "tracking.recheckMs", "Re-check a named face every", "integer", "ms", NULL,
      "How often a named face is re-verified while in view.", 200, 10000, 100 },
    { "Tracking", "events.trackCloseSec", "Visit ends after", "integer", "s", NULL,
      "Seconds without the face before the visit's event is closed.", 1, 600, 1 },
    { "Reporting", "device.role", "Device role", "choice", "", "watchlist|virtual_access",
      "Watchlist: alerts for Concern and Threat, strangers as sightings. Virtual access: every person is an access decision; strangers are alerts.", 0, 0, 0 },
    { "Reporting", "reporting.mode", "Report sightings of", "choice", "", "watchlist|enrolled|all",
      "Watchlist: Concern and Threat only. Enrolled: everyone known. All: known and strangers.", 0, 0, 0 },
    { "Reporting", "reporting.strangers", "Report strangers", "boolean", "", NULL, "Send an event for each stranger visit.", 0, 0, 0 },
    { "Reporting", "reporting.noConcernSightings", "Report known people with no concern", "boolean", "", NULL,
      "Send events for known No-concern people too (watchlist role).", 0, 0, 0 },
    { "Reporting", "events.sceneImages", "Send scene pictures", "boolean", "", NULL, "A 1280x720 picture of the scene with each event.", 0, 0, 0 },
    { "Live view", "overlay.enabled", "Show faces in live view", "boolean", "", NULL, "Draw outlines and names on the camera's video.", 0, 0, 0 },
    { "Live view", "overlay.showScores", "Show match scores", "boolean", "", NULL, "Add the match score to each name.", 0, 0, 0 },
    { "Live view", "overlay.offsetY", "Vertical position correction", "number", "", NULL,
      "Moves the outlines up (negative) or down (positive), as a fraction of the picture height, if they sit off the faces.", -0.20, 0.20, 0.005 },
    { "Live view", "overlay.scaleY", "Vertical scale correction", "number", "", NULL,
      "Stretches (above 1) or squeezes (below 1) outline positions about the middle of the picture.", 0.70, 1.30, 0.01 },
};
#define NDEFS ((int)(sizeof DEFS / sizeof DEFS[0]))

static const setting_def *find(const char *key)
{
    for (int i = 0; i < NDEFS; i++) if (!strcmp(DEFS[i].key, key)) return &DEFS[i];
    return NULL;
}

static int in_list(const json_t *arr, const char *key)
{
    size_t i;
    json_t *v;
    json_array_foreach(arr, i, v) if (json_is_string(v) && !strcmp(json_string_value(v), key)) return 1;
    return 0;
}

json_t *settings_load(const char *path)
{
    json_t *o = json_load_file(path, 0, NULL);
    if (!json_is_object(o)) { json_decref(o); return json_object(); }
    const char *k;
    json_t *v;
    void *tmp;
    json_object_foreach_safe(o, tmp, k, v) if (!find(k)) json_object_del(o, k);   /* drop anything unknown */
    return o;
}

int settings_save(const char *path, const json_t *local)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (json_dump_file(local, tmp, JSON_INDENT(1) | JSON_SORT_KEYS)) return -1;
    return rename(tmp, path);
}

json_t *settings_update(const json_t *local, const json_t *posted, const json_t *managed, double thr, char *why, size_t n)
{
    if (!json_is_object(posted)) { snprintf(why, n, "expected an object of settings"); return NULL; }
    json_t *next = json_deep_copy(local ? local : json_object());
    const char *k;
    json_t *v;
    json_object_foreach((json_t *)posted, k, v) {
        const setting_def *d = find(k);
        if (!d) { snprintf(why, n, "unknown setting %s", k); json_decref(next); return NULL; }
        if (in_list(managed, k)) { snprintf(why, n, "%s is set by Pharos", d->label); json_decref(next); return NULL; }
        if (json_is_null(v)) json_object_del(next, k);
        else json_object_set(next, k, v);
    }
    /* validate with the same rules Pharos's values go through */
    pc_config prev, out;
    pc_defaults(&prev, thr);
    json_t *a, *r, *u;
    pc_apply(next, &prev, thr, &out, &a, &r, &u);
    int bad = 0;
    size_t i;
    json_t *e;
    json_array_foreach(r, i, e) {
        const char *rk = json_string_value(json_object_get(e, "key"));
        if (rk && json_object_get(posted, rk)) {
            const setting_def *d = find(rk);
            snprintf(why, n, "%s: %s", d ? d->label : rk, json_string_value(json_object_get(e, "reason")));
            bad = 1;
            break;
        }
    }
    json_decref(a); json_decref(r); json_decref(u);
    if (bad) { json_decref(next); return NULL; }
    return next;
}

json_t *settings_describe(const pc_config *eff, const json_t *local, const json_t *managed, double thr)
{
    json_t *values = pc_to_json(eff);
    pc_config def;
    pc_defaults(&def, thr);
    json_t *defaults = pc_to_json(&def);
    json_t *groups = json_array(), *cur = NULL;
    const char *cur_name = NULL;
    for (int i = 0; i < NDEFS; i++) {
        const setting_def *d = &DEFS[i];
        if (!cur_name || strcmp(cur_name, d->group)) {
            cur = json_array();
            json_array_append_new(groups, json_pack("{s:s,s:o}", "name", d->group, "settings", cur));
            cur_name = d->group;
        }
        json_t *s = json_pack("{s:s,s:s,s:s,s:s}", "key", d->key, "label", d->label, "help", d->help, "type", d->type);
        if (!strcmp(d->type, "number") || !strcmp(d->type, "integer")) {
            json_object_set_new(s, "min", json_real(d->min));
            json_object_set_new(s, "max", json_real(d->max));
            json_object_set_new(s, "step", json_real(d->step));
        }
        if (d->unit && *d->unit) json_object_set_new(s, "unit", json_string(d->unit));
        if (d->options) {
            json_t *opts = json_array();
            char buf[128];
            snprintf(buf, sizeof buf, "%s", d->options);
            for (char *t = strtok(buf, "|"); t; t = strtok(NULL, "|")) json_array_append_new(opts, json_string(t));
            json_object_set_new(s, "options", opts);
        }
        json_t *v = json_object_get(values, d->key), *dv = json_object_get(defaults, d->key);
        json_object_set(s, "value", v ? v : json_null());
        json_object_set(s, "default", dv ? dv : json_null());
        json_object_set_new(s, "source", json_string(in_list(managed, d->key) ? "pharos"
                                                     : json_object_get(local, d->key) ? "local" : "default"));
        json_array_append_new(cur, s);
    }
    json_decref(values);
    json_decref(defaults);
    return json_pack("{s:o}", "groups", groups);
}
