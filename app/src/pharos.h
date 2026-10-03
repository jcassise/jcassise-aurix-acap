/* AURIX - AURIX-Pharos protocol v1 client (device side). Runs in its own thread.
 * Step 1 scope: /hello, status loop, config read-back, once-only commands, errors/backoff (§3, §4, §8).
 * Identity sync and events follow in later steps. Independent of ACAP APIs (host-testable). */
#ifndef AURIX_PHAROS_H
#define AURIX_PHAROS_H
#include <jansson.h>
#include "pharos_config.h"

typedef enum {
    PS_DISABLED = 0,       /* not commissioned */
    PS_CONNECTING,
    PS_CONNECTED,
    PS_CREDENTIALS_REJECTED,
    PS_REVOKED,
    PS_PROTOCOL_MISMATCH,
    PS_TLS_PIN_MISMATCH,
    PS_TLS_UNTRUSTED,
    PS_UNREACHABLE,
    PS_CONFIG_ERROR,
} pharos_state;

const char *pharos_state_name(pharos_state s);

typedef struct {
    char url[256], device_id[80], token[1024], trust[4096];
    char state_dir[256];               /* writable dir for pharos_state.json */
    char sw_version[32], model_version[128];
    char hw_model[64], serial[64], firmware[64];
    double default_threshold;
} pharos_settings;

typedef struct {
    long long sync_revision, policies_revision;
    int people, templates_ready, templates_failed;
    long long last_sync_ok_ms;         /* 0 = never */
    int events_pending, images_pending;
    double fps;
    int stream_ok;
} pharos_snapshot;

typedef struct {
    void (*apply_config)(const pc_config *cfg, void *user);           /* new effective config */
    void (*snapshot)(pharos_snapshot *out, void *user);               /* fill current state */
    void (*state_changed)(pharos_state s, const char *detail, void *user);
    /* Execute a command; return "done" | "failed" | "unsupported" and optionally a detail. */
    const char *(*command)(const char *type, const json_t *args, char *detail, size_t len, void *user);
    void *user;
} pharos_hooks;

typedef struct pharos pharos;

pharos *pharos_start(const pharos_settings *s, const pharos_hooks *h);
void pharos_stop(pharos *p);              /* joins the thread */

/* Last persisted config (applied immediately at start, before connecting). */
int pharos_load_saved_config(const char *state_dir, double default_threshold, pc_config *out, long long *rev);

#endif
