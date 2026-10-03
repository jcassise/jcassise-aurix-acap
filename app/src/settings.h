/* AURIX - every adjustable setting, for the camera's console and for Pharos (same keys, same rules).
 * Local values live in localdata/settings.json; Pharos's keys override them (pharos.c merges). */
#ifndef AURIX_SETTINGS_H
#define AURIX_SETTINGS_H
#include <jansson.h>
#include "pharos_config.h"

json_t *settings_load(const char *path);                    /* {} if none */
int     settings_save(const char *path, const json_t *local);

/* Applies posted {key: value|null} (null = back to default) to `local`. Refuses unknown keys, keys
 * Pharos manages, and invalid values. Returns the new local settings (caller owns) or NULL + why. */
json_t *settings_update(const json_t *local, const json_t *posted, const json_t *managed, double default_thr,
                        char *why, size_t why_len);

/* For the console: {"groups":[{"name","settings":[{key,label,help,type,min,max,step,unit,options,value,
 * default,source}]}]}. source = pharos | local | default. */
json_t *settings_describe(const pc_config *effective, const json_t *local, const json_t *managed, double default_thr);

#endif
