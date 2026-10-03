/* AURIX - access decision for a recognised person on a virtual-access device (protocol v1 §6).
 * Pure C + jansson + GLib (time zones). */
#ifndef AURIX_ACCESS_H
#define AURIX_ACCESS_H
#include <jansson.h>

typedef struct {
    int granted;
    const char *reason;        /* policy | watchlist | banned | schedule | zone | excluded_date | validity | no_policy */
    char policy_id[65];        /* the policy that granted, "" otherwise */
} access_result;

/* policies: PoliciesResponse.policies. zones: the device's site.zones. tz: IANA name ("" = camera local).
 * valid_from / valid_until: epoch ms, -1 = no bound. */
access_result access_evaluate(const char *const *policy_ids, int npolicies, long long valid_from, long long valid_until,
                              int is_threat, const json_t *policies, const char (*zones)[128], int nzones,
                              const char *tz, long long now_ms);

#endif
