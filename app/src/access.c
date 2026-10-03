#include "access.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>

static const json_t *find_policy(const json_t *policies, const char *id)
{
    size_t i;
    json_t *p;
    json_array_foreach(policies, i, p) {
        const char *pid = json_string_value(json_object_get(p, "policyId"));
        if (pid && !strcmp(pid, id)) return p;
    }
    return NULL;
}

static int hhmm(const char *s)            /* "07:30" -> minutes; "24:00" -> 1440; -1 if bad */
{
    int h, m;
    if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 24 || m < 0 || m > 59 || (h == 24 && m)) return -1;
    return h * 60 + m;
}

static int zone_match(const json_t *rule, const char (*zones)[128], int nzones)
{
    size_t i;
    json_t *z;
    json_array_foreach(json_object_get(rule, "zones"), i, z)
        for (int k = 0; k < nzones; k++)
            if (json_is_string(z) && !strcmp(json_string_value(z), zones[k])) return 1;
    return 0;
}

access_result access_evaluate(const char *const *ids, int n, long long valid_from, long long valid_until, int threat,
                              const json_t *policies, const char (*zones)[128], int nzones, const char *tz, long long now_ms)
{
    access_result r = { 0, "no_policy", "" };
    if (threat) { r.reason = "watchlist"; return r; }
    if (n <= 0) return r;
    if ((valid_from >= 0 && now_ms < valid_from) || (valid_until >= 0 && now_ms >= valid_until)) { r.reason = "validity"; return r; }

    /* any banned policy wins */
    for (int i = 0; i < n; i++) {
        const json_t *p = find_policy(policies, ids[i]);
        if (p && json_is_true(json_object_get(p, "banned"))) { r.reason = "banned"; return r; }
    }

    /* local wall time in the site's time zone */
    GTimeZone *gtz;
#if GLIB_CHECK_VERSION(2, 68, 0)
    gtz = tz && *tz ? g_time_zone_new_identifier(tz) : g_time_zone_new_local();
    if (!gtz) gtz = g_time_zone_new_local();
#else
    gtz = tz && *tz ? g_time_zone_new(tz) : g_time_zone_new_local();
#endif
    GDateTime *utc = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *loc = g_date_time_to_timezone(utc, gtz);
    static const char *DAYS[] = { "", "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    const char *day = DAYS[g_date_time_get_day_of_week(loc)];
    int minute = g_date_time_get_hour(loc) * 60 + g_date_time_get_minute(loc);
    char date[11];
    snprintf(date, sizeof date, "%04d-%02d-%02d", g_date_time_get_year(loc), g_date_time_get_month(loc),
             g_date_time_get_day_of_month(loc));
    g_date_time_unref(loc);
    g_date_time_unref(utc);
    g_time_zone_unref(gtz);

    int saw_zone = 0, saw_excluded = 0, found_policy = 0;
    for (int i = 0; i < n; i++) {
        const json_t *p = find_policy(policies, ids[i]);
        if (!p) continue;
        found_policy = 1;
        if (json_is_true(json_object_get(p, "allZonesAllTimes"))) {
            r.granted = 1; r.reason = "policy";
            snprintf(r.policy_id, sizeof r.policy_id, "%s", ids[i]);
            return r;
        }
        size_t k;
        json_t *rule;
        json_array_foreach(json_object_get(p, "rules"), k, rule) {
            if (!zone_match(rule, zones, nzones)) continue;
            saw_zone = 1;
            int excluded = 0;
            size_t e;
            json_t *d;
            json_array_foreach(json_object_get(rule, "excludedDates"), e, d)
                if (json_is_string(d) && !strcmp(json_string_value(d), date)) excluded = 1;
            if (excluded) { saw_excluded = 1; continue; }
            size_t w;
            json_t *win;
            json_array_foreach(json_object_get(rule, "schedule"), w, win) {
                int on_day = 0;
                size_t q;
                json_t *dd;
                json_array_foreach(json_object_get(win, "days"), q, dd)
                    if (json_is_string(dd) && !strcmp(json_string_value(dd), day)) on_day = 1;
                int a = hhmm(json_string_value(json_object_get(win, "start")));
                int b = hhmm(json_string_value(json_object_get(win, "end")));
                if (on_day && a >= 0 && b > a && minute >= a && minute < b) {
                    r.granted = 1; r.reason = "policy";
                    snprintf(r.policy_id, sizeof r.policy_id, "%s", ids[i]);
                    return r;
                }
            }
        }
    }
    r.reason = !found_policy ? "no_policy" : saw_excluded ? "excluded_date" : saw_zone ? "schedule" : "zone";
    return r;
}
