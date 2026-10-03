#include "metrics.h"
#include "capacity.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *STAGE_NAMES[ST_COUNT] = { "capture", "detect", "align", "embed", "match" };

typedef struct { double sum; unsigned n; double max; } acc_t;

typedef struct {
    long long t;           /* epoch ms */
    float fps, cpu, mem_pct, temp, frame_ms, detect_ms, embed_ms;
} sample_t;

typedef struct { long long t; char name[64]; int threat; float score; int matched; } recent_t;

static struct {
    pthread_mutex_t mu;
    metrics_identity id;
    long long started_ms;
    /* current window */
    acc_t st[ST_COUNT];
    unsigned frames, det, gated, emb, matches, threats, unknown;
    /* last completed window */
    double w_stage_ms[ST_COUNT], w_stage_max[ST_COUNT], w_per_frame_ms[ST_COUNT];
    double w_fps, w_det_pm, w_emb_pm, w_match_pm;
    unsigned w_frames;
    /* totals */
    unsigned long long t_frames, t_det, t_gated, t_emb, t_matches, t_threats, t_unknown;
    sys_reading sys;
    cpu_counters cpu_prev;
    sample_t hist[METRICS_HISTORY];
    int hist_n, hist_head;
    recent_t recent[METRICS_RECENT];
    int recent_n, recent_head;
    int g_total, g_allow, g_threat;
    unsigned long g_bytes;
    double ns_per_entry;
    unsigned bench_entries;
    char g_source[64];
    double threshold, min_eye;
    char mode[24];
    char ph_state[64], ph_detail[200];
    long long ph_rev, ph_since;
    long long sy_rev, sy_last_ok;
    int sy_people, sy_ready, sy_failed, sy_pending;
} M = { .mu = PTHREAD_MUTEX_INITIALIZER };

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void metrics_init(const metrics_identity *id)
{
    pthread_mutex_lock(&M.mu);
    M.id = *id;
    M.started_ms = now_ms();
    snprintf(M.ph_state, sizeof M.ph_state, "Not commissioned");
    snprintf(M.mode, sizeof M.mode, "local");
    pthread_mutex_unlock(&M.mu);
}

void metrics_frame(void)
{
    pthread_mutex_lock(&M.mu);
    M.frames++;
    M.t_frames++;
    pthread_mutex_unlock(&M.mu);
}

void metrics_stage_time(metrics_stage s, double ms)
{
    if (s >= ST_COUNT || ms < 0) return;
    pthread_mutex_lock(&M.mu);
    M.st[s].sum += ms;
    M.st[s].n++;
    if (ms > M.st[s].max) M.st[s].max = ms;
    pthread_mutex_unlock(&M.mu);
}

void metrics_faces(int d, int g, int e)
{
    pthread_mutex_lock(&M.mu);
    M.det += (unsigned)d; M.gated += (unsigned)g; M.emb += (unsigned)e;
    M.t_det += (unsigned)d; M.t_gated += (unsigned)g; M.t_emb += (unsigned)e;
    pthread_mutex_unlock(&M.mu);
}

void metrics_match(const char *name, int threat, float score, int matched)
{
    pthread_mutex_lock(&M.mu);
    if (matched) {
        M.matches++; M.t_matches++;
        if (threat) { M.threats++; M.t_threats++; }
        /* collapse repeats of the same person within 10 s into one row (keep the best score) */
        recent_t *last = M.recent_n ? &M.recent[(M.recent_head + M.recent_n - 1) % METRICS_RECENT] : NULL;
        long long t = now_ms();
        if (last && !strcmp(last->name, name) && t - last->t < 10000) {
            last->t = t;
            if (score > last->score) last->score = score;
        } else {
            recent_t *r;
            if (M.recent_n < METRICS_RECENT) r = &M.recent[(M.recent_head + M.recent_n++) % METRICS_RECENT];
            else { r = &M.recent[M.recent_head]; M.recent_head = (M.recent_head + 1) % METRICS_RECENT; }
            r->t = t;
            snprintf(r->name, sizeof r->name, "%s", name ? name : "");
            r->threat = threat;
            r->score = score;
            r->matched = 1;
        }
    } else {
        M.unknown++; M.t_unknown++;
    }
    pthread_mutex_unlock(&M.mu);
}

void metrics_gallery(int total, int allow, int threat, const char *source, unsigned long ram_bytes)
{
    pthread_mutex_lock(&M.mu);
    M.g_total = total; M.g_allow = allow; M.g_threat = threat; M.g_bytes = ram_bytes;
    snprintf(M.g_source, sizeof M.g_source, "%s", source ? source : "");
    pthread_mutex_unlock(&M.mu);
}

void metrics_match_cost(double ns, unsigned entries)
{
    pthread_mutex_lock(&M.mu);
    M.ns_per_entry = ns;
    M.bench_entries = entries;
    pthread_mutex_unlock(&M.mu);
}

void metrics_recognition(double thr, double min_eye, const char *mode)
{
    pthread_mutex_lock(&M.mu);
    M.threshold = thr; M.min_eye = min_eye;
    snprintf(M.mode, sizeof M.mode, "%s", mode ? mode : "");
    pthread_mutex_unlock(&M.mu);
}

void metrics_pharos(const char *state, const char *detail, long long rev)
{
    pthread_mutex_lock(&M.mu);
    if (state) {
        if (strcmp(M.ph_state, state)) M.ph_since = now_ms();
        snprintf(M.ph_state, sizeof M.ph_state, "%s", state);
        snprintf(M.ph_detail, sizeof M.ph_detail, "%s", detail ? detail : "");
    }
    if (rev >= 0) M.ph_rev = rev;
    pthread_mutex_unlock(&M.mu);
}

void metrics_sync(long long rev, long long last_ok, int people, int ready, int failed, int pending)
{
    pthread_mutex_lock(&M.mu);
    M.sy_rev = rev; M.sy_last_ok = last_ok; M.sy_people = people;
    M.sy_ready = ready; M.sy_failed = failed; M.sy_pending = pending;
    pthread_mutex_unlock(&M.mu);
}

void metrics_sample(double dt, const char *storage_path)
{
    sys_reading s;
    pthread_mutex_lock(&M.mu);
    cpu_counters prev = M.cpu_prev;
    pthread_mutex_unlock(&M.mu);
    sysinfo_read(&s, &prev, storage_path);     /* file reads outside the lock */

    pthread_mutex_lock(&M.mu);
    M.cpu_prev = prev;
    M.sys = s;
    M.w_frames = M.frames;
    M.w_fps = dt > 0 ? M.frames / dt : 0;
    double frame_ms = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        M.w_stage_ms[i] = M.st[i].n ? M.st[i].sum / M.st[i].n : 0;
        M.w_stage_max[i] = M.st[i].max;
        /* budget view: average time per frame spent in this stage (per-face stages scale with faces) */
        M.w_per_frame_ms[i] = M.frames ? M.st[i].sum / M.frames : 0;
        frame_ms += M.w_per_frame_ms[i];
    }
    double per_min = dt > 0 ? 60.0 / dt : 0;
    M.w_det_pm = M.det * per_min; M.w_emb_pm = M.emb * per_min; M.w_match_pm = M.matches * per_min;
    sample_t *h;
    if (M.hist_n < METRICS_HISTORY) h = &M.hist[(M.hist_head + M.hist_n++) % METRICS_HISTORY];
    else { h = &M.hist[M.hist_head]; M.hist_head = (M.hist_head + 1) % METRICS_HISTORY; }
    h->t = now_ms();
    h->fps = (float)M.w_fps;
    h->cpu = (float)s.cpu_pct;
    h->mem_pct = s.mem_total_kb ? (float)(100.0 * (1.0 - (double)s.mem_available_kb / s.mem_total_kb)) : -1;
    h->temp = (float)s.temp_c;
    h->frame_ms = (float)frame_ms;
    h->detect_ms = (float)M.w_stage_ms[ST_DETECT];
    h->embed_ms = (float)M.w_stage_ms[ST_EMBED];
    memset(M.st, 0, sizeof M.st);
    M.frames = M.det = M.gated = M.emb = M.matches = M.threats = M.unknown = 0;
    pthread_mutex_unlock(&M.mu);
}

static json_t *num_or_null(double v, double invalid_below)
{
    return v < invalid_below ? json_null() : json_real((double)(long long)(v * 100 + (v >= 0 ? 0.5 : -0.5)) / 100.0);
}

json_t *metrics_json(void)
{
    pthread_mutex_lock(&M.mu);
    json_t *o = json_object();
    long long t = now_ms();
    json_object_set_new(o, "time", json_integer(t));
    json_object_set_new(o, "device", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:f,s:I}",
        "app", M.id.app_version ?: "", "chip", M.id.chip ?: "", "accelerator", M.id.device ?: "",
        "embedKind", M.id.embed_kind ?: "", "modelVersion", M.id.model_version ?: "", "model", M.id.hw_model ?: "",
        "serial", M.id.serial ?: "", "firmware", M.id.firmware ?: "", "targetFps", M.id.target_fps,
        "appUptimeS", (json_int_t)((t - M.started_ms) / 1000)));
    json_object_set_new(json_object_get(o, "device"), "frame", json_pack("[i,i]", M.id.frame_w, M.id.frame_h));

    json_t *stages = json_array();
    for (int i = 0; i < ST_COUNT; i++)
        json_array_append_new(stages, json_pack("{s:s,s:o,s:o,s:o}", "name", STAGE_NAMES[i],
            "avgMs", num_or_null(M.w_stage_ms[i], 0), "maxMs", num_or_null(M.w_stage_max[i], 0),
            "perFrameMs", num_or_null(M.w_per_frame_ms[i], 0)));
    json_object_set_new(o, "pipeline", json_pack("{s:o,s:i,s:o,s:o,s:o,s:o}",
        "fps", num_or_null(M.w_fps, 0), "windowFrames", (int)M.w_frames, "stages", stages,
        "facesDetectedPerMin", num_or_null(M.w_det_pm, 0), "facesEmbeddedPerMin", num_or_null(M.w_emb_pm, 0),
        "matchesPerMin", num_or_null(M.w_match_pm, 0)));
    json_object_set_new(o, "totals", json_pack("{s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
        "frames", (json_int_t)M.t_frames, "facesDetected", (json_int_t)M.t_det, "facesChecked", (json_int_t)M.t_gated,
        "facesEmbedded", (json_int_t)M.t_emb, "matches", (json_int_t)M.t_matches, "threats", (json_int_t)M.t_threats,
        "unknown", (json_int_t)M.t_unknown));

    const sys_reading *s = &M.sys;
    json_object_set_new(o, "system", json_pack("{s:o,s:i,s:o,s:o,s:I,s:I,s:I,s:o,s:o,s:I,s:I}",
        "cpuPct", num_or_null(s->cpu_pct, 0), "cores", s->cores, "load1", num_or_null(s->load1, 0),
        "load5", num_or_null(s->load5, 0), "memTotalKb", (json_int_t)s->mem_total_kb,
        "memAvailableKb", (json_int_t)s->mem_available_kb, "appRssKb", (json_int_t)s->app_rss_kb,
        "tempC", num_or_null(s->temp_c, -999), "uptimeS", num_or_null(s->uptime_s, 0),
        "storageFreeKb", (json_int_t)s->storage_free_kb, "storageTotalKb", (json_int_t)s->storage_total_kb));

    json_object_set_new(o, "recognition", json_pack("{s:f,s:f,s:s,s:{s:i,s:i,s:i,s:s}}",
        "threshold", M.threshold, "minEyePx", M.min_eye, "mode", M.mode,
        "gallery", "total", M.g_total, "allow", M.g_allow, "threat", M.g_threat, "source", M.g_source));

    {
        cap_result c = capacity_estimate(s->mem_available_kb, s->storage_free_kb, M.ns_per_entry);
        json_object_set_new(o, "capacity", json_pack("{s:I,s:s,s:I,s:I,s:I,s:o,s:o,s:I,s:I,s:{s:i,s:i,s:i,s:i,s:f}}",
            "estimate", (json_int_t)c.estimate, "limitedBy", capacity_limit_name(c.limited_by),
            "byMemory", (json_int_t)c.by_memory, "byStorage", (json_int_t)c.by_storage, "byMatching", (json_int_t)c.by_matching,
            "nsPerEntry", num_or_null(M.ns_per_entry, 0.0001), "matchMsAtEstimate", num_or_null(c.match_ms_at_estimate, 0),
            "galleryRamBytes", (json_int_t)M.g_bytes, "benchEntries", (json_int_t)M.bench_entries,
            "assumptions", "ramBytesPerPerson", CAP_RAM_BYTES_PER_PERSON, "storageBytesPerPerson", CAP_FLASH_BYTES_PER_PERSON,
            "ramReserveMb", CAP_RAM_RESERVE_KB / 1024, "storageReserveMb", CAP_FLASH_RESERVE_KB / 1024,
            "matchBudgetMs", CAP_MATCH_BUDGET_MS));
    }
    json_object_set_new(o, "pharos", json_pack("{s:s,s:s,s:I,s:I}", "state", M.ph_state, "detail", M.ph_detail,
        "configRevision", (json_int_t)M.ph_rev, "sinceMs", (json_int_t)M.ph_since));
    json_object_set_new(o, "sync", json_pack("{s:I,s:I,s:i,s:i,s:i,s:i}", "revision", (json_int_t)M.sy_rev,
        "lastOkMs", (json_int_t)M.sy_last_ok, "people", M.sy_people, "templatesReady", M.sy_ready,
        "templatesFailed", M.sy_failed, "templatesPending", M.sy_pending));

    json_t *hist = json_array();
    for (int i = 0; i < M.hist_n; i++) {
        const sample_t *h = &M.hist[(M.hist_head + i) % METRICS_HISTORY];
        json_array_append_new(hist, json_pack("{s:I,s:o,s:o,s:o,s:o,s:o,s:o,s:o}", "t", (json_int_t)h->t,
            "fps", num_or_null(h->fps, 0), "cpu", num_or_null(h->cpu, 0), "mem", num_or_null(h->mem_pct, 0),
            "temp", num_or_null(h->temp, -999), "frameMs", num_or_null(h->frame_ms, 0),
            "detectMs", num_or_null(h->detect_ms, 0), "embedMs", num_or_null(h->embed_ms, 0)));
    }
    json_object_set_new(o, "history", hist);

    json_t *rec = json_array();
    for (int i = M.recent_n - 1; i >= 0; i--) {
        const recent_t *r = &M.recent[(M.recent_head + i) % METRICS_RECENT];
        json_array_append_new(rec, json_pack("{s:I,s:s,s:s,s:o}", "t", (json_int_t)r->t, "name", r->name,
            "category", r->threat ? "threat" : "allow", "score", json_real((double)(int)(r->score * 1000 + 0.5) / 1000.0)));
    }
    json_object_set_new(o, "recentMatches", rec);
    pthread_mutex_unlock(&M.mu);
    return o;
}
