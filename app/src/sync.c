#include "sync.h"
#include <ctype.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define PAGE_LIMIT 500
#define MAX_PAGES 2000
#define SAVE_EVERY_MS 5000

struct sync_ctx {
    char path[300], device_id[80], model[64];   /* same size as ps_photo.model: compared verbatim */
    sync_hooks h;
    ps_store store;
    long long next_poll_ms, full_not_before_ms, last_save_ms, retry_after_ms;
    int full_wanted, full_jitter_ms, dirty;
    char scope_now[640];             /* zones + keepAllPeople of the current config */
    char in_progress[8];
    char last_error[200];
};

static void set_error(sync_ctx *s, const char *fmt, const char *a)
{
    snprintf(s->last_error, sizeof s->last_error, fmt, a ? a : "");
    syslog(LOG_WARNING, "sync: %s", s->last_error);
}

sync_ctx *sync_new(const char *dir, const char *device_id, const char *model, const sync_hooks *h)
{
    sync_ctx *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->path, sizeof s->path, "%s/people.json", dir);
    snprintf(s->device_id, sizeof s->device_id, "%s", device_id);
    snprintf(s->model, sizeof s->model, "%s", model);
    s->h = *h;
    s->full_jitter_ms = 30000;
    snprintf(s->in_progress, sizeof s->in_progress, "none");
    if (ps_load(&s->store, s->path) == 0) {
        int r, f, p;
        ps_counts(&s->store, s->model, &r, &f, &p);
        syslog(LOG_INFO, "sync: %d people from the last sync (revision %lld, %d templates ready)", s->store.n,
               s->store.revision, r);
    } else {
        ps_init(&s->store);
    }
    if (s->h.changed) s->h.changed(&s->store, s->h.user);
    return s;
}

void sync_free(sync_ctx *s)
{
    if (!s) return;
    if (s->dirty) ps_save(&s->store, s->path);
    ps_free(&s->store);
    free(s);
}

void sync_set_full_jitter_ms(sync_ctx *s, int ms) { s->full_jitter_ms = ms < 0 ? 0 : ms; }

void sync_request_full(sync_ctx *s, int immediate)
{
    s->full_wanted = 1;
    s->next_poll_ms = 0;
    s->retry_after_ms = 0;
    s->full_not_before_ms = 0;
    if (!immediate && s->full_jitter_ms > 0) s->full_not_before_ms = -1;   /* resolved to now + jitter in sync_step */
}

int sync_reenroll(sync_ctx *s, const char *const *ids, int n)
{
    int m = ps_mark_reenroll(&s->store, ids, n);
    s->dirty = 1;
    return m;
}

json_t *sync_status_json(sync_ctx *s)
{
    int r, f, p;
    ps_counts(&s->store, s->model, &r, &f, &p);
    json_t *o = json_pack("{s:I,s:I,s:i,s:i,s:i,s:s}", "revision", (json_int_t)s->store.revision,
                          "policiesRevision", (json_int_t)s->store.policies_revision, "people", s->store.n,
                          "templatesReady", r, "templatesFailed", f, "inProgress", s->in_progress);
    json_object_set_new(o, "lastSuccessAt", s->store.last_ok_ms ? json_integer(s->store.last_ok_ms) : json_null());
    json_object_set_new(o, "lastError", s->last_error[0] ? json_string(s->last_error) : json_null());
    return o;
}

/* ---------- HTTP helpers ---------- */

typedef enum { G_OK, G_NONE, G_SNAPSHOT, G_AUTH, G_REVOKED, G_ERR } get_result;

static get_result classify(const ph_response *r, char *why, size_t wl)
{
    if (r->err != PH_OK) { snprintf(why, wl, "%.190s", r->errmsg); return G_ERR; }
    switch (r->status) {
    case 200: return G_OK;
    case 204: return G_NONE;
    case 401: snprintf(why, wl, "credentials rejected"); return G_AUTH;
    case 409: snprintf(why, wl, "snapshot expired"); return G_SNAPSHOT;
    case 410: snprintf(why, wl, "device revoked"); return G_REVOKED;
    default: snprintf(why, wl, "HTTP %ld", r->status); return G_ERR;
    }
}

static get_result get_json(ph_client *c, const char *path, json_t **out, char *why, size_t wl)
{
    *out = NULL;
    ph_response r = ph_request(c, "GET", path, NULL, 0, NULL, 30);
    get_result g = classify(&r, why, wl);
    if (g == G_OK) {
        *out = (r.json && r.body) ? json_loadb(r.body, r.body_len, 0, NULL) : NULL;
        if (!json_is_object(*out)) { json_decref(*out); *out = NULL; snprintf(why, wl, "%s: reply is not the expected JSON", path); g = G_ERR; }
    }
    ph_response_free(&r);
    return g;
}

static void urlenc(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out[o++] = (char)ch;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", ch);
    }
    out[o] = 0;
}

static sync_result map_fatal(get_result g)
{
    return g == G_AUTH ? SY_AUTH : g == G_REVOKED ? SY_REVOKED : SY_NET;
}

/* ---------- people ---------- */

/* Fetches all pages of one sync into `into` (records built against `current` so templates carry over).
 * Returns G_OK with *snapshot, or the failure. `deleted` collects in-page deletions (delta). */
static get_result fetch_people(sync_ctx *s, ph_client *c, long long since, const ps_store *current, ps_store *into,
                               long long *snapshot, json_t *deleted, int *had_inline_deletions, char *why, size_t wl)
{
    char path[2600], cur[2048] = "", enc[2400];
    long long snap = -1, total = -1, got = 0;
    for (int page = 0; page < MAX_PAGES; page++) {
        if (page == 0 && since >= 0)
            snprintf(path, sizeof path, "/devices/%s/sync/people?since=%lld&limit=%d", s->device_id, since, PAGE_LIMIT);
        else if (page == 0)
            snprintf(path, sizeof path, "/devices/%s/sync/people?limit=%d", s->device_id, PAGE_LIMIT);
        else {
            urlenc(cur, enc, sizeof enc);
            snprintf(path, sizeof path, "/devices/%s/sync/people?cursor=%s&limit=%d", s->device_id, enc, PAGE_LIMIT);
        }
        json_t *b;
        get_result g = get_json(c, path, &b, why, wl);
        if (g != G_OK) return g;
        json_t *people = json_object_get(b, "people");
        json_t *jsnap = json_object_get(b, "snapshotRevision"), *jtot = json_object_get(b, "total");
        if (!json_is_array(people) || !json_is_integer(jsnap) || !json_is_integer(jtot)) {
            json_decref(b); snprintf(why, wl, "people page does not match the contract"); return G_ERR;
        }
        if (snap < 0) snap = json_integer_value(jsnap);
        else if (snap != json_integer_value(jsnap)) {
            json_decref(b); snprintf(why, wl, "pages from different snapshots"); return G_ERR;
        }
        total = json_integer_value(jtot);
        size_t i;
        json_t *pj;
        json_array_foreach(people, i, pj) {
            ps_person p;
            const char *id = json_string_value(json_object_get(pj, "personId"));
            const ps_person *old = id ? ps_find((ps_store *)current, id) : NULL;
            if (ps_from_json(pj, old, &p) || ps_upsert(into, &p)) {
                json_decref(b); snprintf(why, wl, "person record %zu on page %d is invalid", i, page + 1); return G_ERR;
            }
            got++;
        }
        json_t *dl = json_object_get(b, "deleted");
        if (json_is_array(dl)) {
            *had_inline_deletions = 1;
            json_array_extend(deleted, dl);
        }
        json_t *next = json_object_get(b, "nextCursor");
        int more = json_is_string(next) && json_string_length(next) > 0;
        if (more) snprintf(cur, sizeof cur, "%s", json_string_value(next));
        json_decref(b);
        if (!more) {
            if (got != total) {
                char m[64];
                snprintf(m, sizeof m, "received %lld of %lld people", got, total);
                snprintf(why, wl, "%s", m);
                return G_ERR;
            }
            *snapshot = snap;
            return G_OK;
        }
    }
    snprintf(why, wl, "too many pages");
    return G_ERR;
}

static sync_result full_sync(sync_ctx *s, ph_client *c, long long now)
{
    char why[200] = "";
    snprintf(s->in_progress, sizeof s->in_progress, "full");
    for (int attempt = 0; attempt < 3; attempt++) {
        ps_store staging;
        ps_init(&staging);
        json_t *deleted = json_array();
        int inline_del = 0;
        long long snap = 0;
        get_result g = fetch_people(s, c, -1, &s->store, &staging, &snap, deleted, &inline_del, why, sizeof why);
        json_decref(deleted);
        if (g == G_OK) {
            /* all pages arrived and the count matches: swap in atomically */
            staging.policies = s->store.policies ? json_incref(s->store.policies) : NULL;
            staging.policies_revision = s->store.policies_revision;
            staging.revision = snap;
            staging.last_ok_ms = now;
            snprintf(staging.scope, sizeof staging.scope, "%s", s->scope_now);
            int before = s->store.n;
            ps_move(&s->store, &staging);
            s->full_wanted = 0;
            s->last_error[0] = 0;
            s->dirty = 1;
            syslog(LOG_INFO, "sync: full sync applied - %d people (was %d), revision %lld", s->store.n, before, snap);
            snprintf(s->in_progress, sizeof s->in_progress, "none");
            return SY_OK;
        }
        ps_free(&staging);
        if (g == G_SNAPSHOT) continue;                         /* restart from the first page */
        snprintf(s->in_progress, sizeof s->in_progress, "none");
        set_error(s, "full sync failed, keeping the current list: %s", why);
        return g == G_AUTH || g == G_REVOKED ? map_fatal(g) : SY_NET;
    }
    snprintf(s->in_progress, sizeof s->in_progress, "none");
    set_error(s, "full sync failed, keeping the current list: %s", "snapshot kept expiring");
    return SY_NET;
}

static sync_result delta_sync(sync_ctx *s, ph_client *c, long long now)
{
    char why[200] = "";
    snprintf(s->in_progress, sizeof s->in_progress, "delta");
    for (int attempt = 0; attempt < 3; attempt++) {
        ps_store changed;
        ps_init(&changed);
        json_t *deleted = json_array();
        int inline_del = 0;
        long long snap = 0;
        get_result g = fetch_people(s, c, s->store.revision, &s->store, &changed, &snap, deleted, &inline_del, why, sizeof why);
        if (g == G_OK && !inline_del) {
            char path[200];
            json_t *b = NULL;
            snprintf(path, sizeof path, "/devices/%s/sync/deletions?since=%lld", s->device_id, s->store.revision);
            g = get_json(c, path, &b, why, sizeof why);
            if (g == G_NONE) g = G_OK;
            if (g == G_OK && b) json_array_extend(deleted, json_object_get(b, "deleted"));
            json_decref(b);
        }
        if (g == G_OK) {
            /* only now apply: upserts then deletions; revision advances last */
            for (int i = 0; i < changed.n; i++) ps_upsert(&s->store, &changed.v[i]);
            size_t i;
            json_t *d;
            int removed = 0;
            json_array_foreach(deleted, i, d) removed += ps_delete(&s->store, json_string_value(json_object_get(d, "personId"))) == 0;
            s->store.revision = snap;
            s->store.last_ok_ms = now;
            s->last_error[0] = 0;
            s->dirty = 1;
            if (changed.n || removed)
                syslog(LOG_INFO, "sync: %d added or changed, %d removed (revision %lld)", changed.n, removed, snap);
            ps_free(&changed);
            json_decref(deleted);
            snprintf(s->in_progress, sizeof s->in_progress, "none");
            return SY_OK;
        }
        ps_free(&changed);
        json_decref(deleted);
        if (g == G_SNAPSHOT) continue;
        snprintf(s->in_progress, sizeof s->in_progress, "none");
        set_error(s, "update from Pharos failed, will retry: %s", why);
        return g == G_AUTH || g == G_REVOKED ? map_fatal(g) : SY_NET;
    }
    snprintf(s->in_progress, sizeof s->in_progress, "none");
    set_error(s, "update from Pharos failed: %s", "snapshot kept expiring");
    return SY_NET;
}

/* ---------- photos ---------- */

static void sha256_hex(const unsigned char *d, size_t n, char out[65])
{
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(d, n, h);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static void photo_failed(ps_photo *ph, long long now, const char *why)
{
    ph->state = TPL_FAILED;
    ph->attempts++;
    long long back = 60000LL << (ph->attempts > 6 ? 6 : ph->attempts - 1);
    ph->next_try_ms = now + (back > 3600000 ? 3600000 : back);
    snprintf(ph->error, sizeof ph->error, "%s", why);
}

static long long wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* deadline is on the wall clock; `now` (possibly Pharos-corrected) stamps retries */
static sync_result do_photos(sync_ctx *s, ph_client *c, long long now, long long deadline)
{
    int pi, fi;
    while (wall_ms() < deadline && ps_next_pending(&s->store, now, s->model, &pi, &fi)) {
        ps_person *p = &s->store.v[pi];
        ps_photo *ph = &p->photos[fi];
        char path[300], why[160] = "";
        snprintf(path, sizeof path, "/people/%s/photos/%s", p->person_id, ph->photo_id);
        ph_response r = ph_request(c, "GET", path, NULL, 0, NULL, 30);
        get_result g = classify(&r, why, sizeof why);
        if (g == G_AUTH || g == G_REVOKED) { ph_response_free(&r); return map_fatal(g); }
        if (g == G_ERR && r.err != PH_OK) { ph_response_free(&r); return SY_NET; }   /* network: stop, retry next step */
        if (g != G_OK) {
            photo_failed(ph, now, r.status == 404 ? "photo not found in Pharos" : why);
        } else {
            char hex[65];
            sha256_hex((const unsigned char *)r.body, r.body_len, hex);
            if (ph->sha256[0] && strcmp(hex, ph->sha256)) {
                photo_failed(ph, now, "photo bytes do not match their sha256");
            } else {
                int8_t emb[PS_DIM];
                char ew[96] = "";
                int rc = s->h.enroll ? s->h.enroll((const unsigned char *)r.body, r.body_len, emb, ew, sizeof ew, s->h.user) : -1;
                if (rc == 0) {
                    memcpy(ph->emb, emb, sizeof emb);
                    ph->state = TPL_READY;
                    snprintf(ph->model, sizeof ph->model, "%s", s->model);
                    ph->attempts = 0;
                    ph->error[0] = 0;
                } else {
                    photo_failed(ph, now, ew[0] ? ew : "could not build a template");
                }
            }
        }
        if (ph->state == TPL_FAILED)
            syslog(LOG_WARNING, "sync: photo %s of %s failed: %s", ph->photo_id, p->display_name, ph->error);
        ph_response_free(&r);
        s->dirty = 1;
    }
    return SY_OK;
}

/* ---------- step ---------- */

/* The list on the camera was synced for one scope (zones + keepAllPeople). If the configured scope
 * differs - changed while running or while the camera was off - a full sync is needed. */
static void cfg_watch(sync_ctx *s, const pc_config *cfg)
{
    char key[sizeof s->scope_now];
    int o = snprintf(key, sizeof key, "keepAll=%d;zones=", cfg->keep_all_people ? 1 : 0);
    for (int i = 0; i < cfg->nzones && o < (int)sizeof key - 2; i++)
        o += snprintf(key + o, sizeof key - (size_t)o, "%s%.120s", i ? "," : "", cfg->zones[i]);
    snprintf(s->scope_now, sizeof s->scope_now, "%s", key);
    if (s->store.revision > 0 && strcmp(s->store.scope, key) && !s->full_wanted) {
        syslog(LOG_INFO, "sync: zones or keepAllPeople changed - full sync");
        sync_request_full(s, 0);
    }
}

sync_result sync_step(sync_ctx *s, ph_client *c, const pc_config *cfg, long long now, long long budget_ms)
{
    long long deadline = wall_ms() + budget_ms;
    sync_result res = SY_OK;
    cfg_watch(s, cfg);

    if (s->store.revision == 0 && !s->full_wanted) sync_request_full(s, 0);               /* first connection */
    if (cfg->max_delta_age_sec > 0 && s->store.last_ok_ms &&
        now - s->store.last_ok_ms > cfg->max_delta_age_sec * 1000LL && !s->full_wanted)
        sync_request_full(s, 0);
    if (s->full_not_before_ms == -1)
        s->full_not_before_ms = now + (s->full_jitter_ms ? rand() % (s->full_jitter_ms + 1) : 0);

    if (now >= s->next_poll_ms && now >= s->retry_after_ms) {
        s->next_poll_ms = now + cfg->sync_interval_ms;
        char why[200] = "", path[200];
        json_t *st = NULL;
        snprintf(path, sizeof path, "/devices/%s/sync/state", s->device_id);
        get_result g = get_json(c, path, &st, why, sizeof why);
        if (g != G_OK) {
            json_decref(st);
            if (g == G_AUTH || g == G_REVOKED) return map_fatal(g);
            set_error(s, "cannot read sync state: %s", why);
            s->retry_after_ms = now + 5000;
            return SY_NET;
        }
        long long rev = json_integer_value(json_object_get(st, "revision"));
        long long prev = json_integer_value(json_object_get(st, "policiesRevision"));
        json_decref(st);

        if (prev != s->store.policies_revision) {
            json_t *pol = NULL;
            snprintf(path, sizeof path, "/devices/%s/sync/policies", s->device_id);
            g = get_json(c, path, &pol, why, sizeof why);
            if (g == G_OK && json_is_array(json_object_get(pol, "policies"))) {
                json_decref(s->store.policies);
                s->store.policies = json_incref(json_object_get(pol, "policies"));
                s->store.policies_revision = json_integer_value(json_object_get(pol, "revision"));
                s->dirty = 1;
                syslog(LOG_INFO, "sync: %zu access policies (revision %lld)", json_array_size(s->store.policies),
                       s->store.policies_revision);
            } else if (g == G_AUTH || g == G_REVOKED) {
                json_decref(pol);
                return map_fatal(g);
            }
            json_decref(pol);
        }
        if (s->full_wanted && now >= s->full_not_before_ms) res = full_sync(s, c, now);
        else if (!s->full_wanted && rev > s->store.revision) res = delta_sync(s, c, now);
        if (res == SY_AUTH || res == SY_REVOKED) return res;
        if (res != SY_OK) s->retry_after_ms = now + 10000;
        if (s->dirty && s->h.changed) s->h.changed(&s->store, s->h.user);
    }

    if (cfg->download_photos && res == SY_OK) {
        sync_result pr = do_photos(s, c, now, deadline);
        if (pr == SY_AUTH || pr == SY_REVOKED) return pr;
    }
    if (s->dirty) {
        int r, f, p;
        ps_counts(&s->store, s->model, &r, &f, &p);
        if (now - s->last_save_ms >= SAVE_EVERY_MS || p == 0) {
            if (ps_save(&s->store, s->path)) syslog(LOG_ERR, "sync: cannot save %s", s->path);
            s->last_save_ms = now;
            s->dirty = 0;
            if (s->h.changed) s->h.changed(&s->store, s->h.user);
        }
    }
    return res;
}
