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
#include <math.h>
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
#include "enroll.h"
#include "capacity.h"
#include "person_store.h"
#include "tracker.h"
#include "events.h"
#include "event_queue.h"
#include "access.h"
#include "jpeg.h"

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

    GMutex model_lock;           /* detector + embedder: live video and enrolment take turns */
    char model_version[64];
    double match_ns;             /* matching benchmark, for the capacity estimate */
    GMutex gal_lock;             /* protects everything below */
    aurix_gallery pharos_part;   /* templates of people synced from Pharos */
    pc_config pcfg;              /* effective Pharos config (defaults when not commissioned) */
    char device_id[80];          /* "" = not commissioned: events are tracked but not sent */
    struct acc_person { char ref[PS_ID_LEN]; int n; char pol[PS_MAX_POLICIES][PS_ID_LEN]; long long vf, vu; int wl; } *acc;
    int nacc;
    json_t *acc_policies;
    char *gallery_param;         /* last Gallery setting value */
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
static void rebuild_gallery(app_ctx *a, const char *param_in)
{
    if (!a->emb) return;
    char *param_value;
    g_mutex_lock(&a->gal_lock);
    if (param_in) { g_free(a->gallery_param); a->gallery_param = g_strdup(param_in); }
    param_value = g_strdup(a->gallery_param);
    g_mutex_unlock(&a->gal_lock);
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
    g_free(param_value);
    /* people synced from Pharos */
    uint32_t from_pharos = 0;
    g_mutex_lock(&a->gal_lock);
    for (uint32_t i = 0; i < a->pharos_part.count && a->pharos_part.dim == dim; i++)
        from_pharos += gallery_add_ref(&g, a->pharos_part.ids[i], a->pharos_part.refs[i],
                                       (aurix_category)a->pharos_part.category[i],
                                       a->pharos_part.emb + (size_t)i * dim) == 0;
    g_mutex_unlock(&a->gal_lock);
    double *ll = calloc(g.count ? g.count : 1, sizeof(double));

    g_mutex_lock(&a->gal_lock);
    gallery_free(&a->gallery);
    free(a->last_logged);
    a->gallery = g;
    a->last_logged = ll;
    g_mutex_unlock(&a->gal_lock);

    syslog(LOG_INFO, "gallery: %u templates (%u from file, %d from settings, %u from Pharos, kind %s)",
           g.count, from_file, from_param, from_pharos, a->cfg.embed_kind);
    {
        int threats = 0;
        for (uint32_t i = 0; i < g.count; i++) threats += g.category[i] != AURIX_CAT_ALLOW;
        char src[96];
        snprintf(src, sizeof src, "%s%s%s%s%s", from_pharos ? "Pharos" : "",
                 from_pharos && (from_param || from_file) ? " + " : "", from_param ? "Settings" : "",
                 from_param && from_file ? " + " : "", from_file ? "Gallery file" : "");
        metrics_gallery((int)g.count, (int)g.count - threats, threats, src[0] ? src : "Nobody enrolled", gallery_bytes(&g));
    }
    for (uint32_t i = 0; i < g.count && i < 20; i++)
        if (!g.refs[i][0]) syslog(LOG_INFO, "  %s [%s]", g.ids[i], g.category[i] == AURIX_CAT_THREAT ? "threat" : "allow");
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
    char modetxt[48];
    snprintf(modetxt, sizeof modetxt, "%s", c->role == PC_ROLE_VIRTUAL_ACCESS ? "virtual access" : "watchlist");
    metrics_recognition(c->match_threshold, c->min_face_px / 2.25, modetxt);
    metrics_pharos(NULL, NULL, rev);
    g_mutex_lock(&a->gal_lock);
    a->pharos_managed = TRUE;
    a->pcfg = *c;
    a->threshold = (float)c->match_threshold;
    a->min_eye_px = c->min_face_px / 2.25f;      /* face box width -> inter-eye distance */
    g_mutex_unlock(&a->gal_lock);
    syslog(LOG_INFO, "pharos config: role %s, threshold %.2f, min face %d px, mode %s, %d zone(s)%s%s",
           pc_role_name(c->role), c->match_threshold, c->min_face_px, pc_mode_name(c->mode), c->nzones,
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

static int pharos_enroll_cb(const unsigned char *jpeg, size_t len, int8_t emb[PS_DIM], char *why, size_t n, void *user)
{
    app_ctx *a = user;
    g_mutex_lock(&a->model_lock);                 /* waits for the current video frame to finish */
    int rc = enroll_photo(a->det, a->emb, jpeg, len, emb, PS_DIM, why, n);
    g_mutex_unlock(&a->model_lock);
    return rc;
}

static void pharos_people_cb(const ps_store *st, void *user)
{
    app_ctx *a = user;
    aurix_gallery pg;
    gallery_init(&pg, PS_DIM);
    int ready = 0, failed = 0, pending = 0;
    ps_counts(st, a->model_version, &ready, &failed, &pending);
    for (int i = 0; i < st->n; i++) {
        const ps_person *p = &st->v[i];
        aurix_category cat = p->watchlist == PS_WL_THREAT ? AURIX_CAT_THREAT
                           : p->watchlist == PS_WL_CONCERN ? AURIX_CAT_CONCERN : AURIX_CAT_ALLOW;
        for (int k = 0; k < p->nphotos; k++)
            if (p->photos[k].state == TPL_READY && !strcmp(p->photos[k].model, a->model_version))
                gallery_add_ref(&pg, p->display_name, p->person_id, cat, p->photos[k].emb);
    }
    struct acc_person *acc = calloc(st->n ? (size_t)st->n : 1, sizeof *acc);
    for (int i = 0; acc && i < st->n; i++) {          /* store is sorted by personId: bsearch-ready */
        const ps_person *p = &st->v[i];
        snprintf(acc[i].ref, sizeof acc[i].ref, "%s", p->person_id);
        acc[i].n = p->npolicies;
        memcpy(acc[i].pol, p->policy_ids, sizeof acc[i].pol);
        acc[i].vf = p->valid_from;
        acc[i].vu = p->valid_until;
        acc[i].wl = (int)p->watchlist;
    }
    json_t *pol = st->policies ? json_deep_copy(st->policies) : json_array();
    g_mutex_lock(&a->gal_lock);
    gallery_free(&a->pharos_part);
    a->pharos_part = pg;
    free(a->acc);
    a->acc = acc;
    a->nacc = acc ? st->n : 0;
    json_decref(a->acc_policies);
    a->acc_policies = pol;
    g_mutex_unlock(&a->gal_lock);
    metrics_sync(st->revision, st->last_ok_ms, st->n, ready, failed, pending);
    rebuild_gallery(a, NULL);
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
    snprintf(s.model_version, sizeof s.model_version, "%s", a->model_version);
    read_param(a->params, "Brand.ProdNbr", s.hw_model, sizeof s.hw_model);
    read_param(a->params, "Properties.System.SerialNumber", s.serial, sizeof s.serial);
    read_param(a->params, "Properties.Firmware.Version", s.firmware, sizeof s.firmware);
    if (!s.hw_model[0]) snprintf(s.hw_model, sizeof s.hw_model, "unknown");
    if (!s.serial[0]) snprintf(s.serial, sizeof s.serial, "unknown");
    s.default_threshold = a->cfg.match_threshold;
    {   /* capacity estimate reported to Pharos in /hello */
        sys_reading sr;
        cpu_counters cc = { 0, 0 };
        sysinfo_read(&sr, &cc, AURIX_APP_DIR "/localdata");
        cap_result cr = capacity_estimate(sr.mem_available_kb, sr.storage_free_kb, a->match_ns);
        s.max_people = cr.estimate > 1000000 ? 1000000 : (int)cr.estimate;
    }
    pharos_hooks h = { pharos_apply, pharos_snapshot_cb, pharos_state_cb, pharos_command_cb,
                       pharos_enroll_cb, pharos_people_cb, a };
    g_mutex_lock(&a->gal_lock);
    snprintf(a->device_id, sizeof a->device_id, "%s", s.url[0] && s.token[0] ? s.device_id : "");
    g_mutex_unlock(&a->gal_lock);
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

static long long wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int cmp_acc(const void *k, const void *e) { return strcmp(k, ((const struct acc_person *)e)->ref); }

/* Face crop for the event (>= 240 px short side, or native size if the face is smaller; never upscaled). */
static unsigned char *face_jpeg(const aurix_image *f, const trk_track *t, size_t *len)
{
    float cx = (t->x0 + t->x1) / 2, cy = (t->y0 + t->y1) / 2;
    float side = fmaxf(t->x1 - t->x0, t->y1 - t->y0) * 1.5f;
    int x0 = (int)fmaxf(0, cx - side / 2), y0 = (int)fmaxf(0, cy - side / 2);
    int x1 = (int)fminf((float)f->w, cx + side / 2), y1 = (int)fminf((float)f->h, cy + side / 2);
    if (x1 - x0 < 16 || y1 - y0 < 16) return NULL;
    aurix_image crop = { f->data + (size_t)y0 * f->stride + (size_t)x0 * 3, x1 - x0, y1 - y0, f->stride, 3 };
    int cw = crop.w, ch = crop.h;
    if (ch > 480) { cw = cw * 480 / ch; ch = 480; }            /* cap the size, keep detail */
    if (cw == crop.w) return jpeg_encode_rgb(&crop, 85, len);
    unsigned char *px = malloc((size_t)cw * ch * 3);
    if (!px) return NULL;
    aurix_image small = { px, cw, ch, cw * 3, 3 };
    resize_bilinear(&crop, &small);
    unsigned char *j = jpeg_encode_rgb(&small, 85, len);
    free(px);
    return j;
}

static unsigned char *scene_jpeg(const aurix_image *f, size_t *len)
{
    int sw = 1280, sh = 720;
    if (f->w <= sw) return jpeg_encode_rgb(f, 75, len);
    unsigned char *px = malloc((size_t)sw * sh * 3);
    if (!px) return NULL;
    aurix_image small = { px, sw, sh, sw * 3, 3 };
    resize_bilinear(f, &small);
    unsigned char *j = jpeg_encode_rgb(&small, 75, len);
    free(px);
    return j;
}

static const char *wl_name(int cat)
{
    return cat == AURIX_CAT_THREAT ? "threat" : cat == AURIX_CAT_CONCERN ? "concern" : "no_concern";
}

/* Emits (queues) the next revision of a track's event when it should be reported. */
static void emit_event(app_ctx *a, trk_track *t, int ended, const aurix_image *frame, long long now,
                       const pc_config *pc, const char *device_id)
{
    const char *wl = t->state == TS_KNOWN ? wl_name(t->category) : NULL;
    if (!event_should_report(t, pc, wl)) return;
    event_ctx c = { device_id, a->model_version, pc->nzones ? pc->zones[0] : NULL, a->threshold, wl, 0,
                    { 0, "stranger", "" }, 0, 0, pharos_clock_offset_ms() };
    if (pc->role == PC_ROLE_VIRTUAL_ACCESS || pc->report_decisions) {
        c.has_access = 1;
        if (t->state == TS_KNOWN && t->ref[0]) {
            g_mutex_lock(&a->gal_lock);
            const struct acc_person *ap = a->nacc ? bsearch(t->ref, a->acc, (size_t)a->nacc, sizeof *a->acc, cmp_acc) : NULL;
            const char *ids[PS_MAX_POLICIES];
            int n = 0;
            for (int k = 0; ap && k < ap->n; k++) ids[n++] = ap->pol[k];
            c.access = access_evaluate(ids, n, ap ? ap->vf : -1, ap ? ap->vu : -1, t->category == AURIX_CAT_THREAT,
                                       a->acc_policies, (const char (*)[128])pc->zones, pc->nzones, pc->time_zone, now);
            g_mutex_unlock(&a->gal_lock);
        } else if (t->state == TS_KNOWN) {
            c.access.reason = "no_policy";                     /* local test entry, not a Pharos person */
        }
    }
    unsigned char *face = NULL, *scene = NULL;
    size_t fl = 0, sl = 0;
    if (frame && t->seen_now && (t->revision == 0 || t->better_face)) face = face_jpeg(frame, t, &fl);
    if (frame && t->revision == 0 && pc->scene_images) scene = scene_jpeg(frame, &sl);
    c.has_face = face != NULL || t->revision > 0;
    c.has_scene = scene != NULL || (t->revision > 0 && pc->scene_images);
    int rev = t->revision + 1;
    json_t *e = event_build(t, rev, ended, now, &c);
    char *js = json_dumps(e, JSON_COMPACT | JSON_REAL_PRECISION(6));
    json_decref(e);
    if (device_id[0]) eq_push(t->event_id, rev, event_priority(t, wl), now, js, face, fl, scene, sl);
    else { free(js); free(face); free(scene); }
    if (rev == 1 || ended)
        syslog(t->state == TS_KNOWN && wl && strcmp(wl, "no_concern") ? LOG_WARNING : LOG_INFO,
               "event %s %s%s%s%s%s", ended ? "closed" : "opened",
               t->state == TS_KNOWN ? t->name : "stranger", t->ref[0] ? " person=" : "", t->ref,
               c.has_access ? (c.access.granted ? " access=granted" : " access=denied:") : "",
               c.has_access && !c.access.granted ? c.access.reason : "");
    t->revision = rev;
    t->needs_emit = 0;
    t->better_face = 0;
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
    unsigned frames = 0, faces_detected = 0, faces_gated = 0, faces_embedded = 0, events_opened = 0;
    const unsigned max_faces = cfg->max_faces < AURIX_MAX_FACES ? cfg->max_faces : AURIX_MAX_FACES;
    double fps_t0 = now_s();
    unsigned fps_frames = 0;
    trk_params tp = { 0.45f, 0.33f, 2, 3, 3, 3000, 1000, 0.30f };   /* same_face 0.30: measured, see tracker.h */
    tracker tr;
    tracker_init(&tr, &tp);

    while (g_atomic_int_get(&a->running)) {
        aurix_image frame;
        double t0 = now_s();
        if (capture_next(a->cap, &frame)) continue;
        double t1 = now_s();
        long long now = wall_ms();

        /* settings for this frame */
        g_mutex_lock(&a->gal_lock);
        const float min_eye = a->min_eye_px;
        pc_config pc = a->pcfg;
        char device_id[80];
        snprintf(device_id, sizeof device_id, "%s", a->device_id);
        tp.lock_thr = a->threshold;
        tp.keep_thr = a->threshold - 0.12f > 0.25f ? a->threshold - 0.12f : 0.25f;
        tp.close_ms = pc.track_close_sec * 1000LL;
        a->stream_ok = TRUE;
        if (++fps_frames >= 20) {
            double t = now_s();
            a->fps = fps_frames / (t - fps_t0);
            fps_t0 = t;
            fps_frames = 0;
        }
        g_mutex_unlock(&a->gal_lock);
        tracker_set_params(&tr, &tp);

        g_mutex_lock(&a->model_lock);             /* shared with on-camera enrolment */
        int n = detect_faces(a->det, &frame, faces, (int)max_faces, cfg->detect_threshold);
        double t2 = now_s();
        acc(&t_cap, (t1 - t0) * 1e3);
        acc(&t_det, (t2 - t1) * 1e3);
        metrics_frame();
        double cap_wait, cap_conv;
        capture_last_timing(a->cap, &cap_wait, &cap_conv);
        metrics_stage_time(ST_CAPTURE, cap_conv);  /* waiting for the camera counts as idle */
        metrics_stage_time(ST_DETECT, (t2 - t1) * 1e3);
        if (n < 0) n = 0;
        faces_detected += (unsigned)n;

        /* 1. follow faces from frame to frame */
        float boxes[AURIX_MAX_FACES][4];
        int tix[AURIX_MAX_FACES];
        for (int i = 0; i < n; i++) { boxes[i][0] = faces[i].x0; boxes[i][1] = faces[i].y0; boxes[i][2] = faces[i].x1; boxes[i][3] = faces[i].y1; }
        tracker_associate(&tr, (const float (*)[4])boxes, n, now, tix);

        /* 2. identify only what needs it: undecided tracks first, then periodic re-checks of known ones */
        int order[AURIX_MAX_FACES], no = 0, f_gated = 0, f_emb = 0;
        for (int want = 2; want >= 1; want--)
            for (int i = 0; i < n; i++)
                if (tix[i] >= 0 && landmarks_eye_distance(&faces[i].lm) >= min_eye &&
                    tracker_wants_embed(&tr, tix[i], now) == want) order[no++] = i;
        for (int i = 0; i < n; i++) f_gated += landmarks_eye_distance(&faces[i].lm) >= min_eye;
        faces_gated += (unsigned)f_gated;
        int splits[AURIX_MAX_FACES], nsplit = 0;
        for (int k = 0; k < no && a->emb && k < (int)cfg->max_embed_per_frame; k++) {
            int i = order[k], ti = tix[i];
            double e0 = now_s();
            if (align_face(&frame, &faces[i].lm, &face)) continue;
            double e1 = now_s();
            int dim = embed_face(a->emb, &face, q, AURIX_MAX_DIM);
            double e2 = now_s();
            acc(&t_align, (e1 - e0) * 1e3);
            acc(&t_emb, (e2 - e1) * 1e3);
            metrics_stage_time(ST_ALIGN, (e1 - e0) * 1e3);
            metrics_stage_time(ST_EMBED, (e2 - e1) * 1e3);
            if (dim <= 0) continue;
            faces_embedded++;
            f_emb++;
            g_mutex_lock(&a->gal_lock);
            float score = -1.0f, own = -1.0f;
            int idx = (a->gallery.count && (uint32_t)dim == a->gallery.dim) ? gallery_best(&a->gallery, q, &score) : -1;
            char key[TRK_KEY] = "", name[TRK_KEY] = "", ref[TRK_KEY] = "";
            int cat = 0;
            if (idx >= 0) {
                snprintf(ref, sizeof ref, "%s", a->gallery.refs[idx]);
                snprintf(name, sizeof name, "%s", a->gallery.ids[idx]);
                snprintf(key, sizeof key, "%s", ref[0] ? ref : name);
                cat = a->gallery.category[idx];
            }
            if (tr.t[ti].state == TS_KNOWN) own = gallery_score_for(&a->gallery, q, tr.t[ti].key);
            g_mutex_unlock(&a->gal_lock);
            acc(&t_match, (now_s() - e2) * 1e3);
            metrics_stage_time(ST_MATCH, (now_s() - e2) * 1e3);
            float eye = landmarks_eye_distance(&faces[i].lm);
            float quality = faces[i].score * (eye >= 60 ? 1.0f : eye / 60.0f);
            int was_known = tr.t[ti].state == TS_KNOWN;
            if (tracker_observe(&tr, ti, q, dim, key, name, ref, cat, score, own, quality, (int)(faces[i].x1 - faces[i].x0), now))
                splits[nsplit++] = ti;
            if (!was_known && tr.t[ti].state == TS_KNOWN)
                metrics_match(tr.t[ti].name, tr.t[ti].category == AURIX_CAT_THREAT, tr.t[ti].best_score, 1);
        }
        g_mutex_unlock(&a->model_lock);
        metrics_faces(n, f_gated, f_emb);

        /* 3. events: new identities, better faces, splits, and visits that ended */
        for (int k = 0; k < nsplit; k++)        /* a different face: its event may continue if they come back */
            tracker_detach(&tr, splits[k], now);
        for (int k = 0; k < TRK_MAX; k++) {
            trk_track *t = &tr.t[k];
            if (!t->active || !t->seen_now) continue;
            if (t->needs_emit || (t->better_face && t->revision)) {
                int first = t->revision == 0;
                emit_event(a, t, 0, &frame, now, &pc, device_id);
                events_opened += first && t->revision;
            }
        }
        trk_track closed[TRK_MAX];
        int nc = tracker_expire(&tr, now, closed, TRK_MAX);
        for (int k = 0; k < nc; k++)
            if (closed[k].revision) emit_event(a, &closed[k], 1, NULL, now, &pc, device_id);

        /* 4. overlay from the smoothed tracks */
        memset(ob, 0, sizeof(ob));
        int nob = 0;
        const int va = pc.role == PC_ROLE_VIRTUAL_ACCESS;
        for (int k = 0; k < TRK_MAX && nob < OVERLAY_MAX_BOXES; k++) {
            const trk_track *t = &tr.t[k];
            if (!t->active || now - t->last_seen_ms > 600) continue;
            overlay_box *bx = &ob[nob++];
            bx->x0 = t->x0 / frame.w; bx->x1 = t->x1 / frame.w;
            bx->y0 = t->y0 / frame.h; bx->y1 = t->y1 / frame.h;
            if (t->state == TS_PENDING) {
                bx->state = OV_PENDING;
                bx->confidence = 0.3f;
            } else if (t->state == TS_STRANGER) {
                bx->state = va ? OV_ALERT : OV_UNKNOWN;
                bx->confidence = 0.8f;
                snprintf(bx->label, sizeof bx->label, va ? "Stranger" : "Unknown");
            } else {
                int threat = t->category == AURIX_CAT_THREAT, concern = t->category == AURIX_CAT_CONCERN;
                bx->state = threat ? OV_THREAT : concern ? OV_CONCERN : OV_ALLOW;
                bx->confidence = t->score >= tp.lock_thr ? 1.0f : 0.65f;
                snprintf(bx->label, sizeof bx->label, "%s%s", threat ? "THREAT: " : concern ? "CONCERN: " : "", t->name);
                if (va && !threat && t->ref[0]) {           /* virtual access: show the decision */
                    g_mutex_lock(&a->gal_lock);
                    const struct acc_person *ap = a->nacc ? bsearch(t->ref, a->acc, (size_t)a->nacc, sizeof *a->acc, cmp_acc) : NULL;
                    const char *ids[PS_MAX_POLICIES];
                    int np = 0;
                    for (int j = 0; ap && j < ap->n; j++) ids[np++] = ap->pol[j];
                    access_result ar = access_evaluate(ids, np, ap ? ap->vf : -1, ap ? ap->vu : -1, 0, a->acc_policies,
                                                       (const char (*)[128])pc.zones, pc.nzones, pc.time_zone, now);
                    g_mutex_unlock(&a->gal_lock);
                    if (!ar.granted) {
                        bx->state = OV_DENIED;
                        snprintf(bx->label, sizeof bx->label, "Not authorised: %s", t->name);
                    }
                }
            }
        }
        g_mutex_lock(&a->gal_lock);
        gboolean show = a->overlay_on;
        g_mutex_unlock(&a->gal_lock);
        if (show) overlay_publish(ob, nob);

        if (++frames % cfg->stats_every == 0) {
            int active = 0;
            for (int k = 0; k < TRK_MAX; k++) active += tr.t[k].active;
            syslog(LOG_INFO,
                   "stats frames=%u faces det=%u gated=%u emb=%u tracks=%d events=%u | ms: capture %.1f detect %.1f "
                   "align %.2f embed %.1f match %.2f",
                   frames, faces_detected, faces_gated, faces_embedded, active, events_opened, avg(&t_cap), avg(&t_det),
                   avg(&t_align), avg(&t_emb), avg(&t_match));
        }
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
    double ms;
    unsigned renders;
    overlay_stats(&ms, &renders);              /* main loop: same thread as the renders */
    metrics_overlay(ms, renders / 5.0);
    return G_SOURCE_CONTINUE;
}

int main(void)
{
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    static app_ctx a;
    config_defaults(&a.cfg);
    g_mutex_init(&a.gal_lock);
    g_mutex_init(&a.pharos_lock);
    g_mutex_init(&a.model_lock);
    snprintf(a.model_version, sizeof a.model_version, "mobilefacenet-128-int8-%s", a.cfg.embed_kind);
    a.min_eye_px = (float)a.cfg.min_eye_px;
    pc_defaults(&a.pcfg, a.cfg.match_threshold);
    a.acc_policies = json_array();
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
            a.match_ns = ns;
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
