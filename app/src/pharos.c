#include "pharos.h"
#include "pharos_http.h"
#include "sync.h"
#include "event_queue.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PROTOCOL 1
#define MAX_REMEMBERED_COMMANDS 256
#define STATE_FILE "pharos_state.json"

static const char *STATE_NAMES[] = {
    "Not commissioned", "Connecting", "Connected", "Credentials rejected", "Revoked",
    "Protocol mismatch", "TLS pin mismatch", "Server certificate not trusted", "Pharos unreachable",
    "Commissioning error",
};
const char *pharos_state_name(pharos_state s) { return STATE_NAMES[s]; }

typedef struct { char id[65]; char status[16]; char detail[160]; } cmd_rec;

struct pharos {
    pharos_settings s;
    pharos_hooks h;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int stop;

    ph_client *http;
    pharos_state state;
    long long config_rev;
    pc_config cfg;
    json_t *applied, *rejected, *unsupported;
    cmd_rec cmds[MAX_REMEMBERED_COMMANDS];   /* ring of executed commands (persisted) */
    int ncmds, cmd_head;
    long long clock_drift_ms;
    int status_interval_ms;
    unsigned long long cpu_prev_total, cpu_prev_idle;
    sync_ctx *sync;
    json_t *desired;              /* last config Pharos sent (to re-merge when local settings change) */
};

static volatile long long g_clock_offset_ms;

/* Local settings (from the camera's console): the base that Pharos's keys override. */
static pthread_mutex_t g_local_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_local;          /* {key: value} */
static json_t *g_managed;        /* keys Pharos sent last: [key] */
static volatile int g_local_dirty;

void pharos_set_local_settings(const json_t *local)
{
    pthread_mutex_lock(&g_local_mu);
    json_decref(g_local);
    g_local = local ? json_deep_copy(local) : json_object();
    g_local_dirty = 1;
    pthread_mutex_unlock(&g_local_mu);
}

json_t *pharos_managed_keys(void)
{
    pthread_mutex_lock(&g_local_mu);
    json_t *k = g_managed ? json_deep_copy(g_managed) : json_array();
    pthread_mutex_unlock(&g_local_mu);
    return k;
}

/* local settings with Pharos's keys on top; remembers which keys Pharos manages */
static json_t *merged_config(const json_t *desired)
{
    pthread_mutex_lock(&g_local_mu);
    json_t *m = g_local ? json_deep_copy(g_local) : json_object();
    json_decref(g_managed);
    g_managed = json_array();
    const char *k;
    json_t *v;
    if (json_is_object(desired)) json_object_foreach((json_t *)desired, k, v) json_array_append_new(g_managed, json_string(k));
    pthread_mutex_unlock(&g_local_mu);
    if (json_is_object(desired)) json_object_update(m, (json_t *)desired);
    return m;
}
long long pharos_clock_offset_ms(void) { return g_clock_offset_ms; }

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long long pharos_ms(void) { return now_ms() + g_clock_offset_ms; }

static void note_server_time(pharos *p, long long server, long long t0, long long t1)
{
    if (server <= 0) return;
    long long off = server - (t0 + t1) / 2;          /* Pharos minus camera, half the round trip */
    /* smooth small jitter; jump straight to large corrections (first reading, clock step) */
    g_clock_offset_ms = (llabs(off - g_clock_offset_ms) > 2000 || !g_clock_offset_ms) ? off : (g_clock_offset_ms * 7 + off) / 8;
    p->clock_drift_ms = -g_clock_offset_ms;
}

/* Sleeps up to ms; returns 1 if asked to stop. */
static int nap(pharos *p, long long ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&p->mu);
    while (!p->stop && pthread_cond_timedwait(&p->cv, &p->mu, &until) != ETIMEDOUT) {}
    int s = p->stop;
    pthread_mutex_unlock(&p->mu);
    return s;
}

static void set_state(pharos *p, pharos_state st, const char *detail)
{
    if (st == p->state && !detail) return;
    int changed = st != p->state;
    p->state = st;
    if (changed) syslog(st == PS_CONNECTED ? LOG_INFO : LOG_WARNING, "pharos: %s%s%s", STATE_NAMES[st],
                        detail && *detail ? " - " : "", detail ? detail : "");
    if (p->h.state_changed) p->h.state_changed(st, detail, p->h.user);
}

/* ---------- persistence ---------- */

static void state_path(const char *dir, char *out, size_t n) { snprintf(out, n, "%s/%s", dir, STATE_FILE); }

static void persist(pharos *p, const json_t *desired)
{
    json_t *root = json_object();
    json_object_set_new(root, "configRevision", json_integer(p->config_rev));
    if (desired) json_object_set(root, "config", (json_t *)desired);
    json_t *cmds = json_array();
    for (int i = 0; i < p->ncmds; i++) {
        const cmd_rec *c = &p->cmds[(p->cmd_head + i) % MAX_REMEMBERED_COMMANDS];
        json_array_append_new(cmds, json_pack("{s:s,s:s,s:s}", "id", c->id, "status", c->status, "detail", c->detail));
    }
    json_object_set_new(root, "commands", cmds);
    char path[300], tmp[310];
    state_path(p->s.state_dir, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (json_dump_file(root, tmp, JSON_COMPACT) == 0) rename(tmp, path);   /* atomic replace */
    json_decref(root);
}

int pharos_load_saved_config(const char *dir, double thr, pc_config *out, long long *rev)
{
    char path[300];
    state_path(dir, path, sizeof path);
    json_t *root = json_load_file(path, 0, NULL);
    pc_defaults(out, thr);
    *rev = 0;
    json_t *cfg = root ? json_object_get(root, "config") : NULL;
    if (!cfg) {                                    /* nothing from Pharos yet: local settings only */
        pc_config prev = *out;
        json_t *a, *r, *u, *m = merged_config(NULL);
        pc_apply(m, &prev, thr, out, &a, &r, &u);
        json_decref(m); json_decref(a); json_decref(r); json_decref(u);
        json_decref(root);
        return root ? 0 : -1;
    }
    if (cfg) {
        pc_config prev = *out;
        json_t *a, *r, *u;
        json_t *m = merged_config(cfg);
        pc_apply(m, &prev, thr, out, &a, &r, &u);
        json_decref(m);
        json_decref(a); json_decref(r); json_decref(u);
        *rev = json_integer_value(json_object_get(root, "configRevision"));
    }
    json_decref(root);
    return 0;
}

static void load_commands(pharos *p)
{
    char path[300];
    state_path(p->s.state_dir, path, sizeof path);
    json_t *root = json_load_file(path, 0, NULL);
    if (!root) return;
    size_t i;
    json_t *c;
    json_array_foreach(json_object_get(root, "commands"), i, c) {
        if (p->ncmds >= MAX_REMEMBERED_COMMANDS) break;
        cmd_rec *r = &p->cmds[p->ncmds++];
        snprintf(r->id, sizeof r->id, "%s", json_string_value(json_object_get(c, "id")) ?: "");
        snprintf(r->status, sizeof r->status, "%s", json_string_value(json_object_get(c, "status")) ?: "done");
        snprintf(r->detail, sizeof r->detail, "%s", json_string_value(json_object_get(c, "detail")) ?: "");
    }
    json_decref(root);
}

static const cmd_rec *find_cmd(pharos *p, const char *id)
{
    for (int i = 0; i < p->ncmds; i++) {
        const cmd_rec *c = &p->cmds[(p->cmd_head + i) % MAX_REMEMBERED_COMMANDS];
        if (!strcmp(c->id, id)) return c;
    }
    return NULL;
}

static const cmd_rec *remember_cmd(pharos *p, const char *id, const char *status, const char *detail)
{
    cmd_rec *r;
    if (p->ncmds < MAX_REMEMBERED_COMMANDS) r = &p->cmds[(p->cmd_head + p->ncmds++) % MAX_REMEMBERED_COMMANDS];
    else { r = &p->cmds[p->cmd_head]; p->cmd_head = (p->cmd_head + 1) % MAX_REMEMBERED_COMMANDS; }
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->status, sizeof r->status, "%s", status);
    snprintf(r->detail, sizeof r->detail, "%s", detail ? detail : "");
    return r;
}

/* ---------- health ---------- */

static double cpu_pct(pharos *p)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long u, n, s, i, w, x, y, z;
    int k = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &x, &y, &z);
    fclose(f);
    if (k < 4) return -1;
    unsigned long long idle = i + (k > 4 ? w : 0), total = u + n + s + i + (k > 4 ? w + x + y + z : 0);
    double pct = -1;
    if (p->cpu_prev_total && total > p->cpu_prev_total)
        pct = 100.0 * (1.0 - (double)(idle - p->cpu_prev_idle) / (double)(total - p->cpu_prev_total));
    p->cpu_prev_total = total;
    p->cpu_prev_idle = idle;
    return pct;
}

static double temp_c(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1000;
    long v = 0;
    int ok = fscanf(f, "%ld", &v) == 1;
    fclose(f);
    return ok ? v / 1000.0 : -1000;
}

static void own_ip(char *out, size_t n)
{
    out[0] = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa)) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo")) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
            break;
        }
    freeifaddrs(ifa);
}

/* ---------- protocol ---------- */

typedef enum { OC_OK, OC_RETRY, OC_AUTH, OC_REVOKED, OC_PROTOCOL, OC_DROP } outcome;

/* Maps a response to an action per §8 and updates state. *wait_ms receives the delay to apply. */
static outcome classify(pharos *p, const ph_response *r, int *backoff_ms, long long *wait_ms, const char *what)
{
    char d[300];
    if (r->err == PH_ERR_TLS_PIN) { set_state(p, PS_TLS_PIN_MISMATCH, r->errmsg); goto backoff; }
    if (r->err == PH_ERR_TLS_VERIFY) { set_state(p, PS_TLS_UNTRUSTED, r->errmsg); goto backoff; }
    if (r->err != PH_OK) { set_state(p, PS_UNREACHABLE, r->errmsg); goto backoff; }
    switch (r->status) {
    case 200: case 204:
        if (r->status == 200 && (!r->json || !r->body)) {
            snprintf(d, sizeof d, "%s: 200 without a JSON body (proxy or wrong port?)", what);
            set_state(p, PS_UNREACHABLE, d);
            goto backoff;
        }
        *backoff_ms = 1000;
        return OC_OK;
    case 400:
        syslog(LOG_ERR, "pharos: %s rejected as malformed (400): %.200s", what, r->body ? r->body : "");
        return OC_DROP;
    case 401: set_state(p, PS_CREDENTIALS_REJECTED, "check device ID and token"); *wait_ms = 60000; return OC_AUTH;
    case 410: set_state(p, PS_REVOKED, "device revoked in Pharos"); return OC_REVOKED;
    case 426: set_state(p, PS_PROTOCOL_MISMATCH, r->body ? r->body : NULL); *wait_ms = 3600000; return OC_PROTOCOL;
    case 429: case 503:
        if (r->retry_after_s > 0) { *wait_ms = r->retry_after_s * 1000LL; return OC_RETRY; }
        goto backoff;
    default:
        snprintf(d, sizeof d, "%s: HTTP %ld", what, r->status);
        set_state(p, PS_UNREACHABLE, d);
        goto backoff;
    }
backoff:
    *wait_ms = *backoff_ms / 2 + rand() % (*backoff_ms / 2 + 1);    /* jitter */
    *backoff_ms = *backoff_ms * 2 > 60000 ? 60000 : *backoff_ms * 2;
    return OC_RETRY;
}

static json_t *parse_body(const ph_response *r)
{
    return r->body ? json_loadb(r->body, r->body_len, 0, NULL) : NULL;
}

static ph_response post_json(pharos *p, const char *path, json_t *body)
{
    char *txt = json_dumps(body, JSON_COMPACT);
    ph_response r = ph_request(p->http, "POST", path, txt, txt ? strlen(txt) : 0, "application/json", 15);
    free(txt);
    return r;
}

static outcome do_hello(pharos *p, int *backoff, long long *wait)
{
    char ip[64];
    own_ip(ip, sizeof ip);
    json_t *caps = json_pack("[s,s,s,s]", "face-recognition", "watchlist", "access-decision", "scene-images");
    json_t *plat = json_pack("{s:s,s:s,s:s,s:s}", "vendor", "axis", "model", p->s.hw_model, "serial", p->s.serial,
                             "firmware", p->s.firmware);
    if (ip[0]) json_object_set_new(plat, "ip", json_string(ip));
    json_t *sw = json_pack("{s:s,s:s}", "name", "AURIX", "version", p->s.sw_version);
    if (p->s.model_version[0]) json_object_set_new(sw, "modelVersion", json_string(p->s.model_version));
    json_t *req = json_pack("{s:s,s:i,s:o,s:o,s:o}", "deviceId", p->s.device_id, "protocol", PROTOCOL,
                            "software", sw, "platform", plat, "capabilities", caps);
    if (p->s.max_people > 0)   /* proposed contract addition: device capacity */
        json_object_set_new(req, "limits", json_pack("{s:i,s:i}", "maxPeople", p->s.max_people, "maxPhotosPerPerson", PS_MAX_PHOTOS));
    long long t0 = now_ms();
    ph_response r = post_json(p, "/hello", req);
    long long t1 = now_ms();
    json_decref(req);
    outcome o = classify(p, &r, backoff, wait, "hello");
    if (o == OC_OK) {
        json_t *b = parse_body(&r);
        json_t *acc = json_object_get(b, "accepted");
        if (!b || !json_is_true(acc) || json_integer_value(json_object_get(b, "protocol")) != PROTOCOL) {
            set_state(p, PS_UNREACHABLE, "hello: unexpected reply");
            *wait = 5000;
            o = OC_RETRY;
        } else {
            note_server_time(p, json_integer_value(json_object_get(b, "serverTime")), t0, t1);
            if (llabs(p->clock_drift_ms) > 5000)
                syslog(LOG_WARNING, "pharos: camera clock differs from Pharos by %lld ms - check NTP", p->clock_drift_ms);
            int si = (int)json_integer_value(json_object_get(b, "statusIntervalMs"));
            p->status_interval_ms = si >= 500 && si <= 60000 ? si : 2000;
            char tls[128];
            if (!ph_client_tls_info(p->http, tls, sizeof tls)) syslog(LOG_INFO, "pharos: connected over %s", tls);
            ph_client_set_endpoint(p->http, json_string_value(json_object_get(json_object_get(b, "endpoints"), "base")));
            set_state(p, PS_CONNECTED, NULL);
        }
        json_decref(b);
    }
    ph_response_free(&r);
    return o;
}

static json_t *build_status(pharos *p, json_t *results)
{
    pharos_snapshot s = { 0 };
    if (p->h.snapshot) p->h.snapshot(&s, p->h.user);
    json_t *sync = sync_status_json(p->sync);
    eq_stats q = eq_get_stats();
    json_t *queue = json_pack("{s:i,s:i,s:o,s:i}", "eventsPending", q.events_pending, "imagesPending", q.images_pending,
                              "oldestPendingAt", q.oldest_ms ? json_integer(q.oldest_ms + g_clock_offset_ms) : json_null(),
                              "dropped", (int)q.dropped);
    json_t *health = json_pack("{s:f,s:I,s:[{s:s,s:b}]}", "fps", s.fps < 0 ? 0 : s.fps, "clockDriftMs",
                               (json_int_t)p->clock_drift_ms, "streams", "id", "main", "ok", s.stream_ok);
    double cpu = cpu_pct(p), t = temp_c();
    if (cpu >= 0) json_object_set_new(health, "cpuPct", json_real(cpu > 100 ? 100 : cpu));
    if (t > -999) json_object_set_new(health, "tempC", json_real(t));
    json_t *req = json_pack("{s:I,s:I,s:O,s:O,s:O,s:o,s:o,s:o,s:o}", "time", (json_int_t)pharos_ms(),
                            "appliedConfigRevision", (json_int_t)p->config_rev, "appliedConfig", p->applied,
                            "rejectedConfig", p->rejected, "unsupportedConfig", p->unsupported, "sync", sync,
                            "queue", queue, "health", health, "commandResults", results);
    return req;
}

static void apply_config(pharos *p, long long rev, json_t *desired)
{
    pc_config next;
    json_t *a, *r, *u;
    /* copy before releasing: `desired` may be p->desired itself (re-merge after a local change) */
    json_t *copy = desired ? json_deep_copy(desired) : json_object();
    json_decref(p->desired);
    p->desired = copy;
    json_t *m = merged_config(p->desired);
    pc_apply(m, &p->cfg, p->s.default_threshold, &next, &a, &r, &u);
    json_decref(m);
    int changed = memcmp(&next, &p->cfg, sizeof next) != 0 || rev != p->config_rev || g_local_dirty;
    g_local_dirty = 0;
    json_decref(p->applied); json_decref(p->rejected); json_decref(p->unsupported);
    p->applied = a; p->rejected = r; p->unsupported = u;
    p->cfg = next;
    if (changed) {
        if (json_array_size(r)) syslog(LOG_WARNING, "pharos: config revision %lld: %zu setting(s) rejected", rev, json_array_size(r));
        if (rev != p->config_rev) {
            /* unknown keys: once per revision, not every 2 s */
            size_t i; json_t *k;
            json_array_foreach(u, i, k) syslog(LOG_DEBUG, "pharos: unsupported config key %s", json_string_value(k));
        }
        p->config_rev = rev;
        if (p->h.apply_config) p->h.apply_config(&p->cfg, p->config_rev, p->h.user);
        persist(p, p->desired);
    }
}

static void persist_keep_config(pharos *p)
{
    /* rewrite commands while keeping the stored desired config */
    char path[300];
    state_path(p->s.state_dir, path, sizeof path);
    json_t *root = json_load_file(path, 0, NULL);
    json_t *cfg = root ? json_incref(json_object_get(root, "config")) : NULL;
    json_decref(root);
    persist(p, cfg);
    json_decref(cfg);
}

/* Executes new commands once; returns results for every command in this reply (re-reported until
 * Pharos stops sending it). */
static json_t *run_commands(pharos *p, json_t *cmds)
{
    json_t *res = json_array();
    size_t i;
    json_t *c;
    int dirty = 0;
    json_array_foreach(cmds, i, c) {
        const char *id = json_string_value(json_object_get(c, "commandId"));
        const char *type = json_string_value(json_object_get(c, "type"));
        if (!id || !*id || !type) continue;
        const cmd_rec *r = find_cmd(p, id);
        if (!r) {
            char detail[160] = "";
            const char *st;
            if (!strcmp(type, "resync")) {
                sync_request_full(p->sync, 1);
                st = "done";
                snprintf(detail, sizeof detail, "full sync started");
            } else if (!strcmp(type, "reenroll")) {
                json_t *ids = json_object_get(json_object_get(c, "args"), "personIds");
                const char *list[256];
                int n = 0;
                size_t k;
                json_t *x;
                json_array_foreach(ids, k, x) if (n < 256 && json_is_string(x)) list[n++] = json_string_value(x);
                int m = sync_reenroll(p->sync, n ? list : NULL, n);
                st = "done";
                snprintf(detail, sizeof detail, "%d photo(s) queued for new templates", m);
            } else {
                st = p->h.command ? p->h.command(type, json_object_get(c, "args"), detail, sizeof detail, p->h.user)
                                  : "unsupported";
            }
            syslog(LOG_INFO, "pharos: command %s (%s) -> %s%s%s", id, type, st, *detail ? ": " : "", detail);
            r = remember_cmd(p, id, st, detail);
            dirty = 1;
        }
        json_array_append_new(res, json_pack("{s:s,s:s,s:o}", "commandId", r->id, "status", r->status, "detail",
                                             r->detail[0] ? json_string(r->detail) : json_null()));
    }
    if (dirty) persist_keep_config(p);
    return res;
}

/* ---------- events (§7) ---------- */

typedef enum { UP_OK, UP_STOP, UP_AUTH, UP_REVOKED } upload_result;

static upload_result upload_events(pharos *p, long long deadline)
{
    eq_item it;
    char last[37] = "";
    while (now_ms() < deadline && eq_take(&it)) {
        if (!strcmp(last, it.event_id)) { eq_item_free(&it); break; }   /* no progress on this one now */
        snprintf(last, sizeof last, "%s", it.event_id);
        int rev = -1, face = -1, scene = -1;
        upload_result res = UP_OK;
        char path[128];
        if (it.sent_revision < it.revision && it.json) {
            snprintf(path, sizeof path, "/events/%s", it.event_id);
            ph_response r = ph_request(p->http, "PUT", path, it.json, strlen(it.json), "application/json", 15);
            if (r.err != PH_OK || r.status == 429 || r.status >= 500) res = UP_STOP;
            else if (r.status == 401) res = UP_AUTH;
            else if (r.status == 410) res = UP_REVOKED;
            else if (r.status == 200) rev = it.revision;              /* applied, or stale: both final */
            else { syslog(LOG_ERR, "pharos: event %s rejected (HTTP %ld): %.200s", it.event_id, r.status, r.body ? r.body : "");
                   rev = it.revision; face = scene = 1; }             /* our bug: drop it, keep going */
            ph_response_free(&r);
        }
        int event_known = rev >= it.revision || it.sent_revision >= it.revision;
        const struct { const char *kind; unsigned char *data; size_t len; int *flag; } imgs[2] = {
            { "face", it.face, it.face_len, &face }, { "scene", it.scene, it.scene_len, &scene } };
        for (int k = 0; k < 2 && res == UP_OK && event_known; k++) {
            if (!imgs[k].data) continue;
            snprintf(path, sizeof path, "/events/%s/images/%s", it.event_id, imgs[k].kind);
            const unsigned char *d = imgs[k].data;
            size_t L = imgs[k].len;
            int whole = L > 4 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF && d[L - 2] == 0xFF && d[L - 1] == 0xD9;
            ph_response r = ph_request(p->http, "PUT", path, imgs[k].data, imgs[k].len, "image/jpeg", 30);
            /* what left the camera: declared length, bytes libcurl actually sent, and the JPEG's ends */
            static unsigned logged;
            if (!whole || r.uploaded != (long long)L || ++logged <= 20)
                syslog(!whole || r.uploaded != (long long)L ? LOG_WARNING : LOG_INFO,
                       "pharos: %s image of event %.8s: declared %zu bytes, sent %lld, starts %02x%02x%02x, ends %02x%02x%s -> HTTP %ld",
                       imgs[k].kind, it.event_id, L, r.uploaded, L > 2 ? d[0] : 0, L > 2 ? d[1] : 0, L > 2 ? d[2] : 0,
                       L > 1 ? d[L - 2] : 0, L > 1 ? d[L - 1] : 0, whole ? " (whole JPEG)" : " (NOT a whole JPEG)", r.status);
            if (r.err != PH_OK || r.status == 429 || r.status >= 500) res = UP_STOP;
            else if (r.status == 401) res = UP_AUTH;
            else if (r.status == 410) res = UP_REVOKED;
            else if (r.status == 204 || r.status == 200) *imgs[k].flag = 1;
            else if (r.status == 413 || r.status == 400) {          /* refused for good: log why, drop it */
                json_t *eb = r.body ? json_loads(r.body, 0, NULL) : NULL;
                const char *code = json_string_value(json_object_get(json_object_get(eb, "error"), "code"));
                syslog(LOG_WARNING, "pharos: %s image of event %s refused (HTTP %ld %s, %zu bytes, starts %02x%02x%02x)",
                       imgs[k].kind, it.event_id, r.status, code ? code : "no error code", imgs[k].len,
                       imgs[k].len > 0 ? imgs[k].data[0] : 0, imgs[k].len > 1 ? imgs[k].data[1] : 0,
                       imgs[k].len > 2 ? imgs[k].data[2] : 0);
                json_decref(eb);
                *imgs[k].flag = 1;
            }
            /* 404 event_unknown: the event is still queued behind; retry later */
            ph_response_free(&r);
        }
        eq_mark(it.event_id, rev, face, scene);
        eq_item_free(&it);
        if (res != UP_OK) return res;
    }
    return UP_OK;
}

static void *run(void *arg)
{
    pharos *p = arg;
    int backoff = 1000;
    char status_path[160];
    snprintf(status_path, sizeof status_path, "/devices/%s/status", p->s.device_id);
    json_t *results = json_array();          /* command results to report in the next status */

    while (!p->stop) {
        long long wait = 0;
        char why[256] = "";
        if (!p->http) {
            p->http = ph_client_new(p->s.url, p->s.device_id, p->s.token, p->s.trust, why, sizeof why);
            if (!p->http) { set_state(p, PS_CONFIG_ERROR, why); if (nap(p, 30000)) break; continue; }
        }
        if (p->state == PS_DISABLED || p->state == PS_CONNECTED) set_state(p, PS_CONNECTING, NULL);

        ph_err pe = ph_client_prepare(p->http, why, sizeof why);       /* fingerprint pre-flight, no token sent */
        if (pe != PH_OK) {
            set_state(p, pe == PH_ERR_TLS_PIN ? PS_TLS_PIN_MISMATCH : PS_UNREACHABLE, why);
            wait = backoff / 2 + rand() % (backoff / 2 + 1);
            backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
            if (nap(p, wait)) break;
            continue;
        }

        outcome o = do_hello(p, &backoff, &wait);
        if (o == OC_REVOKED) break;                                        /* stop all traffic */
        if (o != OC_OK) { if (nap(p, wait ? wait : 5000)) break; continue; }

        /* ---- connected: status loop ---- */
        long long interval = p->status_interval_ms;
        while (!p->stop) {
            if (g_local_dirty) apply_config(p, p->config_rev, p->desired);
            json_t *req = build_status(p, results);
            results = json_array();
            long long t0 = now_ms();
            ph_response r = post_json(p, status_path, req);
            long long t1 = now_ms();
            json_decref(req);
            wait = 0;
            o = classify(p, &r, &backoff, &wait, "status");
            if (o == OC_OK) {
                json_t *b = parse_body(&r);
                json_t *cfg = json_object_get(b, "config");
                json_t *rev = json_object_get(b, "configRevision");
                json_t *cmds = json_object_get(b, "commands");
                if (!json_is_object(cfg) || !json_is_integer(rev) || !json_is_array(cmds)) {
                    set_state(p, PS_UNREACHABLE, "status: reply does not match the contract");
                    json_decref(b);
                    ph_response_free(&r);
                    o = OC_RETRY;
                    wait = 5000;
                } else {
                    if (p->state != PS_CONNECTED) set_state(p, PS_CONNECTED, NULL);
                    note_server_time(p, json_integer_value(json_object_get(b, "serverTime")), t0, t1);
                    apply_config(p, json_integer_value(rev), cfg);
                    json_decref(results);
                    results = run_commands(p, cmds);
                    long long n = json_integer_value(json_object_get(b, "nextStatusInMs"));
                    interval = n >= 500 && n <= 60000 ? n : p->status_interval_ms;
                    json_decref(b);
                    ph_response_free(&r);
                    /* identity sync in the time left before the next status report */
                    /* events first (they are time-critical), then identity sync, in the time left */
                    upload_result ur = upload_events(p, t0 + interval * 4 / 10);
                    if (ur == UP_AUTH) { set_state(p, PS_CREDENTIALS_REJECTED, "check device ID and token"); o = OC_AUTH; wait = 60000; break; }
                    if (ur == UP_REVOKED) { set_state(p, PS_REVOKED, "device revoked in Pharos"); o = OC_REVOKED; break; }
                    long long budget = interval * 7 / 10 - (now_ms() - t0);
                    sync_result sr = sync_step(p->sync, p->http, &p->cfg, pharos_ms(), budget > 100 ? budget : 100);
                    if (sr == SY_AUTH) { set_state(p, PS_CREDENTIALS_REJECTED, "check device ID and token"); o = OC_AUTH; wait = 60000; break; }
                    if (sr == SY_REVOKED) { set_state(p, PS_REVOKED, "device revoked in Pharos"); o = OC_REVOKED; break; }
                    long long left = interval - (now_ms() - t0);
                    if (nap(p, left > 100 ? left : 100)) break;
                    continue;
                }
            }
            ph_response_free(&r);
            if (o == OC_DROP) { if (nap(p, interval)) break; continue; }   /* our bug; keep running */
            if (o == OC_RETRY && r.err == PH_OK && (r.status == 429 || r.status == 503)) {
                if (nap(p, wait)) break;                                  /* server busy: keep session */
                continue;
            }
            break;   /* auth, revoked, protocol, network/TLS: back to /hello (reconnect) */
        }
        if (p->stop || o == OC_REVOKED) break;
        if (nap(p, wait ? wait : 1000)) break;
    }
    json_decref(results);
    return NULL;
}

pharos *pharos_start(const pharos_settings *s, const pharos_hooks *h)
{
    pharos *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = *s;
    p->h = *h;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    mkdir(s->state_dir, 0700);
    pharos_load_saved_config(s->state_dir, s->default_threshold, &p->cfg, &p->config_rev);
    {   /* remember Pharos's last keys so a local change re-merges under them */
        char sp[300];
        state_path(s->state_dir, sp, sizeof sp);
        json_t *root = json_load_file(sp, 0, NULL);
        json_t *c = root ? json_object_get(root, "config") : NULL;
        p->desired = c ? json_deep_copy(c) : json_object();
        json_t *m = merged_config(c);              /* also records the managed keys */
        json_decref(m);
        json_decref(root);
    }
    json_t *a, *r, *u;
    pc_config tmp;
    pc_apply(NULL, &p->cfg, s->default_threshold, &tmp, &a, &r, &u);
    json_decref(a);
    p->applied = pc_to_json(&p->cfg);
    p->rejected = r;
    p->unsupported = u;
    load_commands(p);
    p->status_interval_ms = 2000;
    srand((unsigned)now_ms());
    sync_hooks sh = { h->enroll, h->people_changed, h->user };
    p->sync = sync_new(s->state_dir, s->device_id, s->model_version, &sh);
    const char *jit = getenv("AURIX_SYNC_JITTER_MS");      /* tests: 0 */
    if (jit && p->sync) sync_set_full_jitter_ms(p->sync, atoi(jit));
    if (h->apply_config) h->apply_config(&p->cfg, p->config_rev, h->user);
    if (!s->url[0] || !s->device_id[0] || !s->token[0]) {
        set_state(p, PS_DISABLED, NULL);
        return p;                       /* not commissioned: no thread */
    }
    p->state = PS_DISABLED;
    if (pthread_create(&p->th, NULL, run, p)) { free(p); return NULL; }
    return p;
}

void pharos_stop(pharos *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    if (p->th) pthread_join(p->th, NULL);
    sync_free(p->sync);
    json_decref(p->desired);
    ph_client_free(p->http);
    json_decref(p->applied); json_decref(p->rejected); json_decref(p->unsupported);
    explicit_bzero(p->s.token, sizeof p->s.token);
    free(p);
}
