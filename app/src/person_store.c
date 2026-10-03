#include "person_store.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ps_init(ps_store *s) { memset(s, 0, sizeof *s); s->sorted = 1; }

void ps_free(ps_store *s)
{
    free(s->v);
    json_decref(s->policies);
    memset(s, 0, sizeof *s);
    s->sorted = 1;
}

void ps_move(ps_store *dst, ps_store *src)
{
    ps_free(dst);
    *dst = *src;
    ps_init(src);
}

static int cmp_person(const void *a, const void *b)
{
    return strcmp(((const ps_person *)a)->person_id, ((const ps_person *)b)->person_id);
}

static void ensure_sorted(ps_store *s)
{
    if (!s->sorted) { qsort(s->v, (size_t)s->n, sizeof *s->v, cmp_person); s->sorted = 1; }
}

ps_person *ps_find(ps_store *s, const char *id)
{
    if (!id || !s->n) return NULL;
    ensure_sorted(s);
    ps_person key;
    snprintf(key.person_id, sizeof key.person_id, "%s", id);
    return bsearch(&key, s->v, (size_t)s->n, sizeof *s->v, cmp_person);
}

int ps_upsert(ps_store *s, const ps_person *p)
{
    ps_person *e = ps_find(s, p->person_id);
    if (e) { *e = *p; return 0; }
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        ps_person *v = realloc(s->v, sizeof *v * (size_t)cap);
        if (!v) return -1;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = *p;
    s->sorted = s->n <= 1 || strcmp(s->v[s->n - 2].person_id, p->person_id) < 0 ? s->sorted : 0;
    return 0;
}

int ps_delete(ps_store *s, const char *id)
{
    ps_person *e = ps_find(s, id);
    if (!e) return -1;
    size_t i = (size_t)(e - s->v);
    memmove(e, e + 1, sizeof *e * (size_t)(s->n - (int)i - 1));
    s->n--;
    return 0;
}

static const char *jstr(const json_t *o, const char *k)
{
    const char *v = json_string_value(json_object_get(o, k));
    return v ? v : "";
}

static long long jtime(const json_t *o, const char *k)
{
    json_t *v = json_object_get(o, k);
    return json_is_integer(v) ? json_integer_value(v) : -1;
}

int ps_from_json(const json_t *j, const ps_person *old, ps_person *p)
{
    memset(p, 0, sizeof *p);
    const char *id = jstr(j, "personId"), *name = jstr(j, "displayName"), *wl = jstr(j, "watchlist");
    if (!*id || strlen(id) >= PS_ID_LEN || !*name) return -1;
    snprintf(p->person_id, sizeof p->person_id, "%s", id);
    snprintf(p->display_name, sizeof p->display_name, "%s", name);
    snprintf(p->external_ref, sizeof p->external_ref, "%s", jstr(j, "externalRef"));
    snprintf(p->person_type, sizeof p->person_type, "%s", jstr(j, "personType"));
    p->revision = json_integer_value(json_object_get(j, "revision"));
    p->watchlist = !strcmp(wl, "threat") ? PS_WL_THREAT : !strcmp(wl, "concern") ? PS_WL_CONCERN : PS_WL_NO_CONCERN;
    p->valid_from = jtime(j, "validFrom");
    p->valid_until = jtime(j, "validUntil");
    size_t i;
    json_t *x;
    json_array_foreach(json_object_get(j, "policyIds"), i, x)
        if (p->npolicies < PS_MAX_POLICIES && json_is_string(x))
            snprintf(p->policy_ids[p->npolicies++], PS_ID_LEN, "%s", json_string_value(x));
    json_array_foreach(json_object_get(j, "photos"), i, x) {
        if (p->nphotos >= PS_MAX_PHOTOS) break;
        const char *pid = jstr(x, "photoId"), *sha = jstr(x, "sha256");
        if (!*pid || strlen(pid) >= PS_ID_LEN || strlen(sha) >= PS_ID_LEN) continue;
        ps_photo *ph = &p->photos[p->nphotos++];
        snprintf(ph->photo_id, sizeof ph->photo_id, "%s", pid);
        snprintf(ph->sha256, sizeof ph->sha256, "%s", sha);
        snprintf(ph->kind, sizeof ph->kind, "%s", jstr(x, "kind"));
        ph->state = TPL_PENDING;
        for (int k = 0; old && k < old->nphotos; k++)          /* same photo, same bytes: keep the template */
            if (!strcmp(old->photos[k].photo_id, pid) && !strcmp(old->photos[k].sha256, sha)) {
                *ph = old->photos[k];
                snprintf(ph->kind, sizeof ph->kind, "%s", jstr(x, "kind"));
                break;
            }
    }
    return 0;
}

int ps_next_pending(ps_store *s, long long now, const char *model, int *pi, int *fi)
{
    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < s->v[i].nphotos; k++) {
            const ps_photo *ph = &s->v[i].photos[k];
            int due = ph->state == TPL_PENDING || (ph->state == TPL_FAILED && ph->next_try_ms <= now) ||
                      (ph->state == TPL_READY && strcmp(ph->model, model));
            if (due) { *pi = i; *fi = k; return 1; }
        }
    return 0;
}

void ps_counts(const ps_store *s, const char *model, int *ready, int *failed, int *pending)
{
    int r = 0, f = 0, p = 0;
    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < s->v[i].nphotos; k++) {
            const ps_photo *ph = &s->v[i].photos[k];
            if (ph->state == TPL_READY && !strcmp(ph->model, model)) r++;
            else if (ph->state == TPL_FAILED) f++;
            else p++;
        }
    if (ready) *ready = r;
    if (failed) *failed = f;
    if (pending) *pending = p;
}

int ps_mark_reenroll(ps_store *s, const char *const *ids, int n)
{
    int marked = 0;
    for (int i = 0; i < s->n; i++) {
        int pick = !ids;
        for (int k = 0; !pick && k < n; k++) pick = !strcmp(ids[k], s->v[i].person_id);
        if (!pick) continue;
        for (int k = 0; k < s->v[i].nphotos; k++) {
            s->v[i].photos[k].state = TPL_PENDING;
            s->v[i].photos[k].attempts = 0;
            s->v[i].photos[k].next_try_ms = 0;
            marked++;
        }
    }
    return marked;
}

/* ---------- persistence ---------- */

static json_t *b64(const int8_t *e)
{
    unsigned char out[4 * ((PS_DIM + 2) / 3) + 1];
    EVP_EncodeBlock(out, (const unsigned char *)e, PS_DIM);
    return json_string((const char *)out);
}

static int unb64(const char *s, int8_t *e)
{
    unsigned char tmp[PS_DIM + 4];
    if (!s || strlen(s) != 4 * ((PS_DIM + 2) / 3)) return -1;
    int n = EVP_DecodeBlock(tmp, (const unsigned char *)s, (int)strlen(s));
    if (n < PS_DIM) return -1;
    memcpy(e, tmp, PS_DIM);
    return 0;
}

int ps_save(const ps_store *s, const char *path)
{
    json_t *people = json_array();
    for (int i = 0; i < s->n; i++) {
        const ps_person *p = &s->v[i];
        json_t *pol = json_array(), *photos = json_array();
        for (int k = 0; k < p->npolicies; k++) json_array_append_new(pol, json_string(p->policy_ids[k]));
        for (int k = 0; k < p->nphotos; k++) {
            const ps_photo *ph = &p->photos[k];
            json_t *o = json_pack("{s:s,s:s,s:s,s:i,s:s,s:i,s:I,s:s}", "photoId", ph->photo_id, "sha256", ph->sha256,
                                  "kind", ph->kind, "state", (int)ph->state, "model", ph->model, "attempts", ph->attempts,
                                  "nextTryMs", (json_int_t)ph->next_try_ms, "error", ph->error);
            if (ph->state == TPL_READY) json_object_set_new(o, "template", b64(ph->emb));
            json_array_append_new(photos, o);
        }
        json_array_append_new(people, json_pack("{s:s,s:s,s:s,s:s,s:I,s:i,s:o,s:I,s:I,s:o}", "personId", p->person_id,
            "externalRef", p->external_ref, "displayName", p->display_name, "personType", p->person_type,
            "revision", (json_int_t)p->revision, "watchlist", (int)p->watchlist, "policyIds", pol,
            "validFrom", (json_int_t)p->valid_from, "validUntil", (json_int_t)p->valid_until, "photos", photos));
    }
    json_t *root = json_pack("{s:i,s:I,s:I,s:I,s:o}", "version", 1, "revision", (json_int_t)s->revision,
                             "policiesRevision", (json_int_t)s->policies_revision, "lastOkMs", (json_int_t)s->last_ok_ms,
                             "people", people);
    json_object_set_new(root, "policies", s->policies ? json_incref(s->policies) : json_array());
    json_object_set_new(root, "scope", json_string(s->scope));
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int rc = json_dump_file(root, tmp, JSON_COMPACT);
    json_decref(root);
    if (rc || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int ps_load(ps_store *s, const char *path)
{
    ps_init(s);
    json_t *root = json_load_file(path, 0, NULL);
    if (!root || json_integer_value(json_object_get(root, "version")) != 1) { json_decref(root); return -1; }
    s->revision = json_integer_value(json_object_get(root, "revision"));
    s->policies_revision = json_integer_value(json_object_get(root, "policiesRevision"));
    s->last_ok_ms = json_integer_value(json_object_get(root, "lastOkMs"));
    s->policies = json_incref(json_object_get(root, "policies"));
    snprintf(s->scope, sizeof s->scope, "%s", jstr(root, "scope"));
    size_t i, k;
    json_t *pj, *phj;
    json_array_foreach(json_object_get(root, "people"), i, pj) {
        ps_person p;
        memset(&p, 0, sizeof p);
        snprintf(p.person_id, sizeof p.person_id, "%s", jstr(pj, "personId"));
        snprintf(p.external_ref, sizeof p.external_ref, "%s", jstr(pj, "externalRef"));
        snprintf(p.display_name, sizeof p.display_name, "%s", jstr(pj, "displayName"));
        snprintf(p.person_type, sizeof p.person_type, "%s", jstr(pj, "personType"));
        p.revision = json_integer_value(json_object_get(pj, "revision"));
        p.watchlist = (ps_watchlist)json_integer_value(json_object_get(pj, "watchlist"));
        p.valid_from = json_integer_value(json_object_get(pj, "validFrom"));
        p.valid_until = json_integer_value(json_object_get(pj, "validUntil"));
        json_t *x;
        json_array_foreach(json_object_get(pj, "policyIds"), k, x)
            if (p.npolicies < PS_MAX_POLICIES) snprintf(p.policy_ids[p.npolicies++], PS_ID_LEN, "%s", json_string_value(x));
        json_array_foreach(json_object_get(pj, "photos"), k, phj) {
            if (p.nphotos >= PS_MAX_PHOTOS) break;
            ps_photo *ph = &p.photos[p.nphotos++];
            snprintf(ph->photo_id, sizeof ph->photo_id, "%s", jstr(phj, "photoId"));
            snprintf(ph->sha256, sizeof ph->sha256, "%s", jstr(phj, "sha256"));
            snprintf(ph->kind, sizeof ph->kind, "%s", jstr(phj, "kind"));
            snprintf(ph->model, sizeof ph->model, "%s", jstr(phj, "model"));
            snprintf(ph->error, sizeof ph->error, "%s", jstr(phj, "error"));
            ph->state = (ps_tpl_state)json_integer_value(json_object_get(phj, "state"));
            ph->attempts = (int)json_integer_value(json_object_get(phj, "attempts"));
            ph->next_try_ms = json_integer_value(json_object_get(phj, "nextTryMs"));
            if (ph->state == TPL_READY && unb64(jstr(phj, "template"), ph->emb)) ph->state = TPL_PENDING;
        }
        if (p.person_id[0]) ps_upsert(s, &p);
    }
    json_decref(root);
    return 0;
}
