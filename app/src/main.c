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
    float threshold;
    gboolean overlay_on;
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
    int from_param = gallery_parse_param(&g, param_value, a->cfg.embed_kind);
    if (from_param < 0) {
        syslog(LOG_ERR, "Gallery setting is malformed - keeping file entries only");
        from_param = 0;
    }
    double *ll = calloc(g.count ? g.count : 1, sizeof(double));

    g_mutex_lock(&a->gal_lock);
    gallery_free(&a->gallery);
    free(a->last_logged);
    a->gallery = g;
    a->last_logged = ll;
    g_mutex_unlock(&a->gal_lock);

    syslog(LOG_INFO, "gallery: %u identities (%u from file, %d from settings, kind %s)",
           g.count, from_file, from_param, a->cfg.embed_kind);
    for (uint32_t i = 0; i < g.count; i++)
        syslog(LOG_INFO, "  %s [%s]", g.ids[i], g.category[i] == AURIX_CAT_THREAT ? "threat" : "allow");
}

static void on_param(const gchar *name, const gchar *value, gpointer data)
{
    app_ctx *a = data;   /* must not call ax_parameter_* here */
    if (g_str_has_suffix(name, ".Gallery")) {
        rebuild_gallery(a, value);
    } else if (g_str_has_suffix(name, ".MatchThreshold")) {
        int v = value ? atoi(value) : 45;
        g_mutex_lock(&a->gal_lock);
        a->threshold = (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
        g_mutex_unlock(&a->gal_lock);
        syslog(LOG_INFO, "match threshold now %.2f", (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0);
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

    while (g_atomic_int_get(&a->running)) {
        aurix_image frame;
        double t0 = now_s();
        if (capture_next(a->cap, &frame)) continue;
        double t1 = now_s();
        int n = detect_faces(a->det, &frame, faces, (int)max_faces, cfg->detect_threshold);
        double t2 = now_s();
        acc(&t_cap, (t1 - t0) * 1e3);
        acc(&t_det, (t2 - t1) * 1e3);
        if (n < 0) n = 0;
        faces_detected += (unsigned)n;

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
            if (landmarks_eye_distance(&faces[i].lm) < (float)cfg->min_eye_px) continue;
            faces_gated++;
            if (!a->emb || embedded_this_frame >= cfg->max_embed_per_frame) continue;
            embedded_this_frame++;

            double a0 = now_s();
            if (align_face(&frame, &faces[i].lm, &face)) continue;
            double a1 = now_s();
            int dim = embed_face(a->emb, &face, q, AURIX_MAX_DIM);
            double a2 = now_s();
            acc(&t_align, (a1 - a0) * 1e3);
            acc(&t_emb, (a2 - a1) * 1e3);
            if (dim <= 0) continue;
            faces_embedded++;

            g_mutex_lock(&a->gal_lock);
            float score = -1.0f;
            int idx = (a->gallery.count && (uint32_t)dim == a->gallery.dim) ? gallery_best(&a->gallery, q, &score) : -1;
            acc(&t_match, (now_s() - a2) * 1e3);
            int hit = idx >= 0 && score >= a->threshold;
            int threat = hit && a->gallery.category[idx] == AURIX_CAT_THREAT;
            if (bx) {
                bx->state = hit ? (threat ? OV_THREAT : OV_ALLOW) : OV_UNKNOWN;
                if (hit)
                    snprintf(bx->label, sizeof(bx->label), "%s%s %.2f", threat ? "THREAT: " : "",
                             a->gallery.ids[idx], score);
                else
                    snprintf(bx->label, sizeof(bx->label), "unknown");
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

int main(void)
{
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    static app_ctx a;
    config_defaults(&a.cfg);
    g_mutex_init(&a.gal_lock);
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
        const char *names[] = { "Gallery", "MatchThreshold", "Overlay" };
        for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
            if (!ax_parameter_register_callback(a.params, names[i], on_param, &a, &err)) {
                syslog(LOG_WARNING, "watch %s: %s", names[i], err ? err->message : "?");
                g_clear_error(&err);
            }
    }

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
