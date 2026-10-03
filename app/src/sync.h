/* AURIX - identity sync with Pharos (protocol v1 §5): people, policies, photos -> on-camera templates.
 * Runs inside the Pharos client thread between status reports. Pure C + jansson + pharos_http. */
#ifndef AURIX_SYNC_H
#define AURIX_SYNC_H
#include <jansson.h>
#include "person_store.h"
#include "pharos_config.h"
#include "pharos_http.h"

typedef struct {
    /* Build a template from a JPEG enrolment photo. 0 = ok (emb filled), else why. Called on the Pharos thread. */
    int (*enroll)(const unsigned char *jpeg, size_t len, int8_t emb[PS_DIM], char *why, size_t why_len, void *user);
    /* The people set or its templates changed; `s` is valid only during the call. */
    void (*changed)(const ps_store *s, void *user);
    void *user;
} sync_hooks;

typedef enum { SY_OK = 0, SY_AUTH, SY_REVOKED, SY_NET } sync_result;

typedef struct sync_ctx sync_ctx;

sync_ctx *sync_new(const char *state_dir, const char *device_id, const char *model_version, const sync_hooks *h);
void sync_free(sync_ctx *s);

/* Random delay (0..ms) before a full sync AURIX starts on its own (spec: 0-30 s). */
void sync_set_full_jitter_ms(sync_ctx *s, int ms);

/* Does due work within budget_ms: poll, delta or full sync, then photos. */
sync_result sync_step(sync_ctx *s, ph_client *c, const pc_config *cfg, long long now_ms, long long budget_ms);

void sync_request_full(sync_ctx *s, int immediate);          /* resync command / config change */
int  sync_reenroll(sync_ctx *s, const char *const *ids, int n);

/* Fills the StatusRequest.sync object. */
json_t *sync_status_json(sync_ctx *s);

#endif
