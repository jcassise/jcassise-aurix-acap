/* AURIX - Pharos configuration keys (protocol v1 §4): validate, apply, read back. Pure C + jansson. */
#ifndef AURIX_PHAROS_CONFIG_H
#define AURIX_PHAROS_CONFIG_H
#include <jansson.h>
#include <stdbool.h>

#define PC_MAX_ZONES 16
#define PC_ZONE_LEN 128

typedef enum { PC_MODE_WATCHLIST = 0, PC_MODE_ENROLLED, PC_MODE_ALL } pc_mode;
typedef enum { PC_ROLE_WATCHLIST = 0, PC_ROLE_VIRTUAL_ACCESS } pc_role;

typedef struct {
    char zones[PC_MAX_ZONES][PC_ZONE_LEN];
    int nzones;
    char time_zone[64];              /* "" = camera's own */
    pc_role role;                    /* device.role: what the device is for (events follow it in step 3) */
    pc_mode mode;
    bool strangers, no_concern_sightings, unrecognizable;
    double match_threshold;
    int min_face_px;
    bool liveness;
    int track_close_sec;
    bool scene_images;
    int sync_interval_ms;
    bool keep_all_people;
    long long max_delta_age_sec;
    bool download_photos;
    bool report_decisions;
} pc_config;

/* AURIX defaults (a key Pharos omits means "device default"). */
void pc_defaults(pc_config *c, double default_threshold);

/* Applies `desired` on top of defaults. Invalid values keep the value from `prev` and are listed in
 * *rejected ([{key,reason}]); unknown / unsupported keys go to *unsupported ([key]).
 * *applied receives the full effective configuration. Caller owns the three json_t*. */
void pc_apply(const json_t *desired, const pc_config *prev, double default_threshold, pc_config *out,
              json_t **applied, json_t **rejected, json_t **unsupported);

/* Effective config as a Config JSON object. */
json_t *pc_to_json(const pc_config *c);

const char *pc_mode_name(pc_mode m);
const char *pc_role_name(pc_role r);

#endif
