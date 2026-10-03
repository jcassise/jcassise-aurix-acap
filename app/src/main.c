/* AURIX - on-camera face matching ACAP (benchmark build).
 * Worker thread: VDO -> larod detect -> CPU 5-pt align -> larod embed -> NEON cosine match.
 * Main thread:   GLib loop driving the live-view overlay, app settings and shutdown.
 * Research weights are for internal benchmarking only and are never committed or shipped. */
#include <axsdk/axparameter.h>
#include <glib-unix.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "align.h"
#include "capture.h"
#include "config.h"
#include "detect.h"
#include "embed.h"
#include "match.h"
#include "overlay.h"
#include "pharos.h"
#include "metrics.h"
#include "web.h"
#include "commission.h"

#define APP_NAME "aurix"
#define MATCH_LOG_INTERVAL_S 5.0

typedef struct {
    aurix_config cfg;
    aurix_detector *det;
    aurix_embedder *emb;
    aurix_capture *cap;
    GMainLoop *loop;
    AXParameter *params;
    volatile gint running;

    GMutex gal_lock;             /* protects everything below */
    aurix_gallery gallery;
    double *last_logged;         /* per gallery entry, for MATCH log rate limiting */
    double last_nomatch_log;
    float threshold;
    float min_eye_px;
    gboolean overlay_on;
    double fps;
    gboolean stream_ok;

    pharos *pharos;              /* Pharos client (NULL until commissioned settings are read) */
    GMutex pharos_lock;          /* serialises restarts */
    gboolean pharos_managed;     /* commissioned: Pharos owns threshold / min face size */
    char pharos_status_text[200];
    char *file_gallery_path;
} app_ctx;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct { double total; unsigned n; } stat_acc;
static void acc(stat_acc *s, double v) { s->total += v; s->n++; }
static double avg(const stat_acc *s) { return s->n ? s->total / s->n : 0.0; }

/* Builds the gallery from localdata/gallery.bin (if present) plus the Gallery setting, then swaps it in. */
static void rebuild_gallery(app_ctx *a, const char *param_value)
{
    if (!a->emb) return;
    const uint32_t dim = (uint32_t)embedder_dim(a->emb);
    aurix_gallery g;
    if (gallery_load(a->file_gallery_path, &g) == 0 && g.dim != dim) {
        syslog(LOG_WARNING, "gallery.bin dim %u != embedder dim %u - ignored", g.dim, dim);
        gallery_free(&g);
        gallery_init(&g, dim);
    } else if (g.dim == 0) {
        gallery_init(&g, dim);
    }
    uint32_t from_file = g.count;
    int sk = 0, ss = 0;
    syslog(LOG_INFO, "Gallery setting: %zu characters", param_value ? strlen(param_value) : (size_t)0);
    int from_param = gallery_parse_param(&g, param_value, a->cfg.embed_kind, &sk, &ss);
    if (from_param < 0) {
        syslog(LOG_ERR, "Gallery setting is malformed (expected name,allow|threat,kind,base64;...) - "
                        "keeping file entries only");
        from_param = 0;
    }
    if (sk) syslog(LOG_WARNING, "Gallery: %d entr%s for another camera type skipped (this camera needs '%s')",
                   sk, sk == 1 ? "y" : "ies", a->cfg.embed_kind);
    if (ss) syslog(LOG_WARNING, "Gallery: %d entr%s with wrong embedding size skipped (truncated?)",
                   ss, ss == 1 ? "y" : "ies");
    double *ll = calloc(g.count ? g.count : 1, sizeof(double));

    g_mutex_lock(&a->gal_lock);
    gallery_free(&a->gallery);
    free(a->last_logged);
    a->gallery = g;
    a->last_logged = ll;
    g_mutex_unlock(&a->gal_lock);

    syslog(LOG_INFO, "gallery: %u identities (%u from file, %d from settings, kind %s)",
           g.count, from_file, from_param, a->cfg.embed_kind);
    {
        int threats = 0;
        for (uint32_t i = 0; i < g.count; i++) threats += g.category[i] == AURIX_CAT_THREAT;
        metrics_gallery((int)g.count, (int)g.count - threats, threats,
                        from_file && from_param ? "Settings and gallery file" : from_file ? "Gallery file"
                        : from_param ? "Settings" : "Nobody enrolled", gallery_bytes(&g));
    }
    for (uint32_t i = 0; i < g.count; i++)
        syslog(LOG_INFO, "  %s [%s]", g.ids[i], g.category[i] == AURIX_CAT_THREAT ? "threat" : "allow");
}

static void schedule_pharos_restart(app_ctx *a);

static void on_param(const gchar *name, const gchar *value, gpointer data)
{
    app_ctx *a = data;   /* must not call ax_parameter_* here */
    if (g_str_has_suffix(name, ".Gallery")) {
        rebuild_gallery(a, value);
    } else if (g_str_has_suffix(name, ".MatchThreshold")) {
        int v = value ? atoi(value) : 45;
        g_mutex_lock(&a->gal_lock);
        if (a->pharos_managed) {
            g_mutex_unlock(&a->gal_lock);
            syslog(LOG_INFO, "MatchThreshold setting ignored: this camera is managed by Pharos");
            return;
        }
        a->threshold = (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
        float me = a->min_eye_px;
        g_mutex_unlock(&a->gal_lock);
        metrics_recognition((v < 0 ? 0 : v > 100 ? 100 : v) / 100.0, me, "local settings");
        syslog(LOG_INFO, "match threshold now %.2f", (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0);
    } else if (g_str_has_suffix(name, ".PharosUrl") || g_str_has_suffix(name, ".PharosDeviceId") ||
               g_str_has_suffix(name, ".PharosToken") || g_str_has_suffix(name, ".PharosServerCert")) {
        schedule_pharos_restart(a);
    } else if (g_str_has_suffix(name, ".Overlay")) {
        g_mutex_lock(&a->gal_lock);
        a->overlay_on = value && !strcmp(value, "yes");
        g_mutex_unlock(&a->gal_lock);
        if (!(value && !strcmp(value, "yes"))) overlay_publish(NULL, 0);
    }
}

static char *param_get(AXParameter *p, const char *name)
{
    GError *err = NULL;
    gchar *v = NULL;
    if (!p || !ax_parameter_get(p, name, &v, &err)) {
        if (err) {
            syslog(LOG_WARNING, "parameter %s: %s", name, err->message);
            g_error_free(err);
        }
        return NULL;
    }
    return v;
}

/* ---------------- Pharos integration ---------------- */

static void pharos_apply(const pc_config *c, long long rev, void *user)
{
    app_ctx *a = user;
    metrics_recognition(c->match_threshold, c->min_face_px / 2.25, pc_mode_name(c->mode));
    metrics_pharos(NULL, NULL, rev);
    g_mutex_lock(&a->gal_lock);
    a->pharos_managed = TRUE;
    a->threshold = (float)c->match_threshold;
    a->min_eye_px = c->min_face_px / 2.25f;      /* face box width -> inter-eye distance */
    g_mutex_unlock(&a->gal_lock);
    syslog(LOG_INFO, "pharos config: threshold %.2f, min face %d px, mode %s, %d zone(s)%s%s",
           c->match_threshold, c->min_face_px, pc_mode_name(c->mode), c->nzones,
           c->time_zone[0] ? ", tz " : "", c->time_zone);
}

static void pharos_snapshot_cb(pharos_snapshot *s, void *user)
{
    app_ctx *a = user;
    g_mutex_lock(&a->gal_lock);
    s->people = (int)a->gallery.count;
    s->templates_ready = (int)a->gallery.count;
    s->fps = a->fps;
    s->stream_ok = a->stream_ok;
    g_mutex_unlock(&a->gal_lock);
}

typedef struct { app_ctx *a; char text[200]; } status_msg;

static gboolean publish_status_idle(gpointer data)
{
    status_msg *m = data;
    GError *err = NULL;
    if (m->a->params && !ax_parameter_set(m->a->params, "PharosStatus", m->text, TRUE, &err)) {
        syslog(LOG_WARNING, "cannot update PharosStatus: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void pharos_state_cb(pharos_state st, const char *detail, void *user)
{
    app_ctx *a = user;
    status_msg *m = g_new0(status_msg, 1);
    m->a = a;
    snprintf(m->text, sizeof m->text, "%s%s%s", pharos_state_name(st), detail && *detail ? ": " : "",
             detail ? detail : "");
    metrics_pharos(pharos_state_name(st), detail, -1);
    g_mutex_lock(&a->gal_lock);
    snprintf(a->pharos_status_text, sizeof a->pharos_status_text, "%s", m->text);
    g_mutex_unlock(&a->gal_lock);
    g_idle_add(publish_status_idle, m);      /* ax_parameter_* must run on the main loop */
}

static const char *pharos_command_cb(const char *type, const json_t *args, char *detail, size_t n, void *user)
{
    (void)args; (void)user;
    if (!strcmp(type, "resync") || !strcmp(type, "reenroll") || !strcmp(type, "captureScene")) {
        snprintf(detail, n, "%s arrives in a later AURIX build", type);
        return "unsupported";
    }
    return "unsupported";
}

static void read_param(AXParameter *p, const char *name, char *out, size_t n)
{
    char *v = param_get(p, name);
    snprintf(out, n, "%s", v ? v : "");
    g_free(v);
}

/* (Re)starts the Pharos client from the current settings. Runs off the main loop: stopping the old
 * client can wait for an in-flight request. */
static gpointer pharos_restart_thread(gpointer data)
{
    app_ctx *a = data;
    g_mutex_lock(&a->pharos_lock);
    pharos_stop(a->pharos);
    a->pharos = NULL;
    pharos_settings s = { 0 };
    commission cm;
    if (commission_load(AURIX_APP_DIR "/localdata/pharos", &cm) == 0) {
        /* saved from the AURIX page: wins over the app settings */
        snprintf(s.url, sizeof s.url, "%s", cm.url);
        snprintf(s.device_id, sizeof s.device_id, "%s", cm.device_id);
        snprintf(s.token, sizeof s.token, "%s", cm.token);
        snprintf(s.trust, sizeof s.trust, "%s", cm.trust);
    } else {
        read_param(a->params, "PharosUrl", s.url, sizeof s.url);
        read_param(a->params, "PharosDeviceId", s.device_id, sizeof s.device_id);
        read_param(a->params, "PharosToken", s.token, sizeof s.token);
        read_param(a->params, "PharosServerCert", s.trust, sizeof s.trust);
    }
    explicit_bzero(&cm, sizeof cm);
    snprintf(s.state_dir, sizeof s.state_dir, "%s/localdata/pharos", AURIX_APP_DIR);
    snprintf(s.sw_version, sizeof s.sw_version, "%s", AURIX_VERSION);
    snprintf(s.model_version, sizeof s.model_version, "mobilefacenet-128-int8-%s", a->cfg.embed_kind);
    read_param(a->params, "Brand.ProdNbr", s.hw_model, sizeof s.hw_model);
    read_param(a->params, "Properties.System.SerialNumber", s.serial, sizeof s.serial);
    read_param(a->params, "Properties.Firmware.Version", s.firmware, sizeof s.firmware);
    if (!s.hw_model[0]) snprintf(s.hw_model, sizeof s.hw_model, "unknown");
    if (!s.serial[0]) snprintf(s.serial, sizeof s.serial, "unknown");
    s.default_threshold = a->cfg.match_threshold;
    pharos_hooks h = { pharos_apply, pharos_snapshot_cb, pharos_state_cb, pharos_command_cb, a };
    if (s.url[0] && s.device_id[0] && s.token[0]) {
        a->pharos = pharos_start(&s, &h);
    } else {
        g_mutex_lock(&a->gal_lock);
        gboolean was = a->pharos_managed;
        a->pharos_managed = FALSE;
        g_mutex_unlock(&a->gal_lock);
        if (was) {                                   /* decommissioned: local settings apply again */
            char *v = param_get(a->params, "MatchThreshold");
            if (v) { on_param(".MatchThreshold", v, a); g_free(v); }
        }
        pharos_state_cb(PS_DISABLED, "enter Pharos URL, device ID and token", a);
    }
    explicit_bzero(s.token, sizeof s.token);
    g_mutex_unlock(&a->pharos_lock);
    return NULL;
}

static guint pharos_restart_src;

static void web_status_cb(char *out, size_t n, void *user)
{
    app_ctx *a = user;
    g_mutex_lock(&a->gal_lock);
    snprintf(out, n, "%s", a->pharos_status_text[0] ? a->pharos_status_text : "Not commissioned");
    g_mutex_unlock(&a->gal_lock);
}

static void schedule_pharos_restart(app_ctx *a);
static gboolean commission_changed_idle(gpointer data) { schedule_pharos_restart(data); return G_SOURCE_REMOVE; }
static void web_changed_cb(void *user) { g_idle_add(commission_changed_idle, user); }   /* from the web thread */

static gboolean pharos_restart_due(gpointer data)
{
    pharos_restart_src = 0;
    g_thread_unref(g_thread_new("aurix-pharos-restart", pharos_restart_thread, data));
    return G_SOURCE_REMOVE;
}

static void schedule_pharos_restart(app_ctx *a)
{
    /* debounce: an installer usually edits several fields in a row */
    if (pharos_restart_src) g_source_remove(pharos_restart_src);
    pharos_restart_src = g_timeout_add_seconds(2, pharos_restart_due, a);
}

static gpointer worker(gpointer data)
{
    app_ctx *a = data;
    const aurix_config *cfg = &a->cfg;
    aurix_face faces[AURIX_MAX_FACES];
    uint8_t face_px[AURIX_FACE_SIZE * AURIX_FACE_SIZE * 3];
    aurix_image face = { face_px, AURIX_FACE_SIZE, AURIX_FACE_SIZE, AURIX_FACE_SIZE * 3, 3 };
    int8_t q[AURIX_MAX_DIM];
    overlay_box ob[OVERLAY_MAX_BOXES];
    stat_acc t_cap = {0}, t_det = {0}, t_align = {0}, t_emb = {0}, t_match = {0};
    unsigned frames = 0, faces_detected = 0, faces_gated = 0, faces_embedded = 0, matches = 0;
    const unsigned max_faces = cfg->max_faces < AURIX_MAX_FACES ? cfg->max_faces : AURIX_MAX_FACES;
    double fps_t0 = now_s();
    unsigned fps_frames = 0;

    while (g_atomic_int_get(&a->running)) {
        aurix_image frame;
        double t0 = now_s();
        if (capture_next(a->cap, &frame)) continue;
        double t1 = now_s();
        int n = detect_faces(a->det, &frame, faces, (int)max_faces, cfg->detect_threshold);
        double t2 = now_s();
        acc(&t_cap, (t1 - t0) * 1e3);
        acc(&t_det, (t2 - t1) * 1e3);
        metrics_frame();
        double cap_wait, cap_conv;
        capture_last_timing(a->cap, &cap_wait, &cap_conv);
        metrics_stage_time(ST_CAPTURE, cap_conv);      /* waiting for the camera counts as idle */
        metrics_stage_time(ST_DETECT, (t2 - t1) * 1e3);
        int f_gated = 0, f_emb = 0;
        if (n < 0) n = 0;
        faces_detected += (unsigned)n;
        g_mutex_lock(&a->gal_lock);
        const float min_eye = a->min_eye_px;
        a->stream_ok = TRUE;
        if (++fps_frames >= 20) {
            double t = now_s();
            a->fps = fps_frames / (t - fps_t0);
            fps_t0 = t;
            fps_frames = 0;
        }
        g_mutex_unlock(&a->gal_lock);

        memset(ob, 0, sizeof(ob));
        int nob = 0;
        unsigned embedded_this_frame = 0;
        for (int i = 0; i < n; i++) {
            overlay_box *bx = nob < OVERLAY_MAX_BOXES ? &ob[nob++] : NULL;
            if (bx) {
                bx->x0 = faces[i].x0 / frame.w; bx->x1 = faces[i].x1 / frame.w;
                bx->y0 = faces[i].y0 / frame.h; bx->y1 = faces[i].y1 / frame.h;
                bx->state = OV_PENDING;
            }
            if (landmarks_eye_distance(&faces[i].lm) < min_eye) continue;
            faces_gated++;
            f_gated++;
            if (!a->emb || embedded_this_frame >= cfg->max_embed_per_frame) continue;
            embedded_this_frame++;

            double a0 = now_s();
            if (align_face(&frame, &faces[i].lm, &face)) continue;
            double a1 = now_s();
            int dim = embed_face(a->emb, &face, q, AURIX_MAX_DIM);
            double a2 = now_s();
            acc(&t_align, (a1 - a0) * 1e3);
            acc(&t_emb, (a2 - a1) * 1e3);
            metrics_stage_time(ST_ALIGN, (a1 - a0) * 1e3);
            metrics_stage_time(ST_EMBED, (a2 - a1) * 1e3);
            if (dim <= 0) continue;
            faces_embedded++;
            f_emb++;

            g_mutex_lock(&a->gal_lock);
            float score = -1.0f;
            int idx = (a->gallery.count && (uint32_t)dim == a->gallery.dim) ? gallery_best(&a->gallery, q, &score) : -1;
            acc(&t_match, (now_s() - a2) * 1e3);
            metrics_stage_time(ST_MATCH, (now_s() - a2) * 1e3);
            int hit = idx >= 0 && score >= a->threshold;
            int threat = hit && a->gallery.category[idx] == AURIX_CAT_THREAT;
            if (a->gallery.count) metrics_match(hit ? a->gallery.ids[idx] : NULL, threat, score, hit);
            if (bx) {
                bx->state = hit ? (threat ? OV_THREAT : OV_ALLOW) : OV_UNKNOWN;
                if (hit)
                    snprintf(bx->label, sizeof(bx->label), "%s%s %.2f", threat ? "THREAT: " : "",
                             a->gallery.ids[idx], score);
                else if (idx >= 0)
                    snprintf(bx->label, sizeof(bx->label), "unknown (%.2f)", score);
                else
                    snprintf(bx->label, sizeof(bx->label), "unknown");
            }
            if (!hit && idx >= 0 && now_s() - a->last_nomatch_log >= MATCH_LOG_INTERVAL_S) {
                a->last_nomatch_log = now_s();
                syslog(LOG_INFO, "no match: closest %s score=%.3f (threshold %.2f)", a->gallery.ids[idx], score,
                       a->threshold);
            }
            if (hit) {
                matches++;
                double t = now_s();
                if (t - a->last_logged[idx] >= MATCH_LOG_INTERVAL_S) {
                    a->last_logged[idx] = t;
                    /* TODO: Axis event (and Device Data Hub on AXIS OS 13) instead of syslog. */
                    syslog(threat ? LOG_WARNING : LOG_INFO, "%s id=%s score=%.3f",
                           threat ? "THREAT" : "MATCH", a->gallery.ids[idx], score);
                }
            }
            g_mutex_unlock(&a->gal_lock);
        }

        metrics_faces(n, f_gated, f_emb);
        g_mutex_lock(&a->gal_lock);
        gboolean show = a->overlay_on;
        g_mutex_unlock(&a->gal_lock);
        if (show) overlay_publish(ob, nob);

        if (++frames % cfg->stats_every == 0)
            syslog(LOG_INFO,
                   "stats frames=%u faces det=%u gated=%u emb=%u match=%u | ms: capture %.1f detect %.1f "
                   "align %.2f embed %.1f match %.2f",
                   frames, faces_detected, faces_gated, faces_embedded, matches, avg(&t_cap), avg(&t_det),
                   avg(&t_align), avg(&t_emb), avg(&t_match));
    }
    return NULL;
}

static gboolean on_signal(gpointer data)
{
    app_ctx *a = data;
    g_atomic_int_set(&a->running, 0);
    g_main_loop_quit(a->loop);
    return G_SOURCE_REMOVE;
}

static gboolean metrics_tick(gpointer unused)
{
    (void)unused;
    metrics_sample(5.0, AURIX_APP_DIR "/localdata");
    return G_SOURCE_CONTINUE;
}

int main(void)
{
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    static app_ctx a;
    config_defaults(&a.cfg);
    g_mutex_init(&a.gal_lock);
    g_mutex_init(&a.pharos_lock);
    a.min_eye_px = (float)a.cfg.min_eye_px;
    a.threshold = a.cfg.match_threshold;
    a.overlay_on = TRUE;
    a.file_gallery_path = (char *)a.cfg.gallery_path;
    a.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, &a);
    g_unix_signal_add(SIGINT, on_signal, &a);
    syslog(LOG_INFO, "starting, larod device %s, embedder kind %s", a.cfg.device, a.cfg.embed_kind);

    GError *err = NULL;
    a.params = ax_parameter_new(APP_NAME, &err);
    if (!a.params) {
        syslog(LOG_WARNING, "app settings unavailable: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    char *v;
    if ((v = param_get(a.params, "MatchThreshold"))) { on_param(".MatchThreshold", v, &a); g_free(v); }
    if ((v = param_get(a.params, "Overlay")))        { on_param(".Overlay", v, &a); g_free(v); }

    a.det = detector_open(a.cfg.detect_model, a.cfg.detect_meta, a.cfg.device);
    a.emb = embedder_open(a.cfg.embed_model, a.cfg.embed_meta, a.cfg.device);
    if (!a.emb) syslog(LOG_WARNING, "no embedder - running detection benchmark only");

    char *gal = param_get(a.params, "Gallery");
    rebuild_gallery(&a, gal);
    g_free(gal);

    if (a.params) {
        const char *names[] = { "Gallery", "MatchThreshold", "Overlay", "PharosUrl", "PharosDeviceId",
                                "PharosToken", "PharosServerCert" };
        for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
            if (!ax_parameter_register_callback(a.params, names[i], on_param, &a, &err)) {
                syslog(LOG_WARNING, "watch %s: %s", names[i], err ? err->message : "?");
                g_clear_error(&err);
            }
    }

    {
        char model[64], serial[64], fw[64];
        static char model_s[64], serial_s[64], fw_s[64], mv[128];
        read_param(a.params, "Brand.ProdNbr", model, sizeof model);
        read_param(a.params, "Properties.System.SerialNumber", serial, sizeof serial);
        read_param(a.params, "Properties.Firmware.Version", fw, sizeof fw);
        snprintf(model_s, sizeof model_s, "%s%s", model[0] ? "AXIS " : "", model);
        snprintf(serial_s, sizeof serial_s, "%s", serial);
        snprintf(fw_s, sizeof fw_s, "%s", fw);
        snprintf(mv, sizeof mv, "mobilefacenet-128-int8-%s", a.cfg.embed_kind);
        metrics_identity id = {
            .app_version = AURIX_VERSION,
#if defined(__aarch64__)
            .chip = "ARTPEC-8", .device = "DLPU",
#else
            .chip = "ARTPEC-7", .device = "CPU",
#endif
            .embed_kind = a.cfg.embed_kind, .model_version = mv, .hw_model = model_s, .serial = serial_s,
            .firmware = fw_s, .target_fps = a.cfg.fps, .frame_w = (int)a.cfg.width, .frame_h = (int)a.cfg.height,
        };
        metrics_init(&id);
        metrics_recognition(a.threshold, a.min_eye_px, "local settings");
        metrics_sample(5.0, AURIX_APP_DIR "/localdata");    /* prime CPU counters */
        {
            /* matching cost on this chip: compare one face with 4096 synthetic entries, 25 times */
            double ns = gallery_benchmark_ns_per_entry(a.emb ? (uint32_t)embedder_dim(a.emb) : 128, 4096, 25);
            metrics_match_cost(ns, 4096);
            syslog(LOG_INFO, "matching benchmark: %.1f ns per gallery entry (%.2f ms per face per 10,000 people)",
                   ns, ns * 10000 / 1e6);
        }
        g_timeout_add_seconds(5, metrics_tick, NULL);
        static web_commissioning wc;
        wc = (web_commissioning){ AURIX_APP_DIR "/localdata/pharos", web_status_cb, web_changed_cb, &a };
        web_start(AURIX_APP_DIR "/web/dashboard.html", &wc);
    }
    if (a.params) pharos_restart_thread(&a);          /* initial start (synchronous, no client yet) */

    /* fontconfig (used by cairo text) needs a writable cache dir */
    g_setenv("XDG_CACHE_HOME", AURIX_APP_DIR "/localdata", TRUE);
    overlay_init();
    a.cap = a.det ? capture_open(a.cfg.width, a.cfg.height, a.cfg.fps) : NULL;

    GThread *th = NULL;
    if (a.det && a.cap) {
        g_atomic_int_set(&a.running, 1);
        th = g_thread_new("aurix-pipeline", worker, &a);
    } else {
        /* runMode is "respawn": stay alive (idle) rather than exit-and-restart in a loop */
        syslog(LOG_ERR, "pipeline not ready (detector missing or VDO failed) - idling");
    }

    g_main_loop_run(a.loop);

    g_atomic_int_set(&a.running, 0);
    if (th) g_thread_join(th);
    g_mutex_lock(&a.pharos_lock);
    pharos_stop(a.pharos);
    a.pharos = NULL;
    g_mutex_unlock(&a.pharos_lock);
    overlay_cleanup();
    capture_close(a.cap);
    detector_close(a.det);
    embedder_close(a.emb);
    if (a.params) ax_parameter_free(a.params);
    g_mutex_lock(&a.gal_lock);
    gallery_free(&a.gallery);
    free(a.last_logged);
    g_mutex_unlock(&a.gal_lock);
    g_main_loop_unref(a.loop);
    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
