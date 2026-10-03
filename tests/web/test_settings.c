/* Settings: the same rules for the console and Pharos; Pharos's keys win; refusals say why. */
#include <stdio.h>
#include <string.h>
#include "settings.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("PASS %s\n", #c); } while (0)

static const json_t *find(const json_t *d, const char *key)
{
    size_t i, k; json_t *g, *s;
    json_array_foreach(json_object_get(d, "groups"), i, g)
        json_array_foreach(json_object_get(g, "settings"), k, s)
            if (!strcmp(json_string_value(json_object_get(s, "key")), key)) return s;
    return NULL;
}

int main(void)
{
    char why[200] = "";
    json_t *local = json_object(), *managed = json_pack("[s]", "recognition.matchThreshold");
    json_t *n = settings_update(local, json_pack("{s:f,s:b}", "tracking.sameFace", 0.4, "overlay.showScores", 1), managed, 0.45, why, sizeof why);
    CHECK(n && json_real_value(json_object_get(n, "tracking.sameFace")) == 0.4, "valid local change (%s)", why);
    json_t *x = settings_update(n, json_pack("{s:f}", "recognition.matchThreshold", 0.6), managed, 0.45, why, sizeof why);
    CHECK(!x && strstr(why, "set by Pharos"), "a Pharos-managed key is refused (%s)", why);
    x = settings_update(n, json_pack("{s:f}", "tracking.sameFace", 0.9), managed, 0.45, why, sizeof why);
    CHECK(!x && strstr(why, "Same-face") && strstr(why, "range"), "out-of-range value refused with the reason (%s)", why);
    x = settings_update(n, json_pack("{s:s}", "no.such.key", "x"), managed, 0.45, why, sizeof why);
    CHECK(!x && strstr(why, "unknown"), "unknown key refused (%s)", why);
    json_t *r = settings_update(n, json_pack("{s:n}", "tracking.sameFace"), managed, 0.45, why, sizeof why);
    CHECK(r && !json_object_get(r, "tracking.sameFace") && json_object_get(r, "overlay.showScores"), "null resets one setting to its default");
    /* the console view: values, defaults and where each value comes from */
    pc_config eff, prev;
    pc_defaults(&prev, 0.45);
    json_t *a, *rj, *u, *m = json_deep_copy(n);
    json_object_set_new(m, "recognition.matchThreshold", json_real(0.55));      /* what Pharos sent */
    pc_apply(m, &prev, 0.45, &eff, &a, &rj, &u);
    json_t *d = settings_describe(&eff, n, managed, 0.45);
    const json_t *thr = find(d, "recognition.matchThreshold"), *sf = find(d, "tracking.sameFace"), *ld = find(d, "tracking.lockFrames");
    CHECK(thr && !strcmp(json_string_value(json_object_get(thr, "source")), "pharos") && json_real_value(json_object_get(thr, "value")) == 0.55,
          "Pharos value shown as set by Pharos");
    CHECK(sf && !strcmp(json_string_value(json_object_get(sf, "source")), "local") && json_real_value(json_object_get(sf, "value")) == 0.4,
          "local value shown as changed here");
    CHECK(ld && !strcmp(json_string_value(json_object_get(ld, "source")), "default") && json_integer_value(json_object_get(ld, "value")) == 2,
          "untouched setting shown as default");
    size_t groups = json_array_size(json_object_get(d, "groups"));
    CHECK(groups == 4, "four groups (Recognition, Tracking, Reporting, Live view): %zu", groups);
    printf(fails ? "%d failure(s)\n" : "all settings checks passed\n", fails);
    return fails != 0;
}
