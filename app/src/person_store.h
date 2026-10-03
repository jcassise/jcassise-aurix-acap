/* AURIX - people synced from Pharos, with on-camera templates (pure C + jansson, host-testable).
 * Persisted to localdata/pharos/people.json, always replaced atomically. Templates are keyed by
 * photoId + sha256 + model version: an unchanged photo keeps its template across syncs. */
#ifndef AURIX_PERSON_STORE_H
#define AURIX_PERSON_STORE_H
#include <jansson.h>
#include <stdint.h>

#define PS_MAX_PHOTOS 5
#define PS_MAX_POLICIES 16
#define PS_DIM 128
#define PS_ID_LEN 65

typedef enum { PS_WL_NO_CONCERN = 0, PS_WL_CONCERN, PS_WL_THREAT } ps_watchlist;
typedef enum { TPL_PENDING = 0, TPL_READY, TPL_FAILED } ps_tpl_state;

typedef struct {
    char photo_id[PS_ID_LEN], sha256[PS_ID_LEN], kind[16];
    ps_tpl_state state;
    int8_t emb[PS_DIM];
    char model[64];                 /* model version that built emb */
    int attempts;
    long long next_try_ms;          /* retry time after a failure */
    char error[96];
} ps_photo;

typedef struct {
    char person_id[PS_ID_LEN], external_ref[PS_ID_LEN], display_name[128], person_type[32];
    long long revision;
    ps_watchlist watchlist;
    char policy_ids[PS_MAX_POLICIES][PS_ID_LEN];
    int npolicies;
    long long valid_from, valid_until;   /* -1 = no bound */
    ps_photo photos[PS_MAX_PHOTOS];
    int nphotos;
} ps_person;

typedef struct {
    ps_person *v;
    int n, cap;
    int sorted;                     /* v[] ordered by person_id (for lookups) */
    long long revision, policies_revision, last_ok_ms;
    json_t *policies;               /* PoliciesResponse.policies as received (kept for access decisions) */
    char scope[640];                /* zones + keepAllPeople this list was fully synced for */
} ps_store;

void ps_init(ps_store *s);
void ps_free(ps_store *s);
void ps_move(ps_store *dst, ps_store *src);     /* dst takes src's contents; src emptied */

int  ps_load(ps_store *s, const char *path);    /* 0 = loaded */
int  ps_save(const ps_store *s, const char *path);

ps_person *ps_find(ps_store *s, const char *person_id);
int  ps_upsert(ps_store *s, const ps_person *p);
int  ps_delete(ps_store *s, const char *person_id);

/* Person record JSON -> ps_person. Photos whose photoId+sha256 match `old` keep its template.
 * Returns 0, or -1 if the record lacks required fields. */
int  ps_from_json(const json_t *j, const ps_person *old, ps_person *out);

/* Next photo needing work (pending, failed and due, or built by another model). */
int  ps_next_pending(ps_store *s, long long now_ms, const char *model, int *person_idx, int *photo_idx);

void ps_counts(const ps_store *s, const char *model, int *ready, int *failed, int *pending);

/* Mark templates for rebuilding (personIds NULL = everyone). Returns photos marked. */
int  ps_mark_reenroll(ps_store *s, const char *const *person_ids, int n);

#endif
