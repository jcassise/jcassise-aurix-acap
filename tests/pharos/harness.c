/* Host harness: runs the real AURIX Pharos client with fake hooks; prints machine-readable lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "pharos.h"

static int resyncs;

static void on_apply(const pc_config *c, long long rev, void *u)
{
    (void)u; (void)rev;
    printf("APPLY threshold=%.3f minFace=%d mode=%s zones=%d\n", c->match_threshold, c->min_face_px,
           pc_mode_name(c->mode), c->nzones);
    fflush(stdout);
}
static void on_snapshot(pharos_snapshot *s, void *u) { (void)u; s->people = 1; s->templates_ready = 1; s->fps = 9.5; s->stream_ok = 1; }
static void on_state(pharos_state s, const char *d, void *u)
{
    (void)u;
    printf("STATE %s|%s\n", pharos_state_name(s), d ? d : "");
    fflush(stdout);
}
static const char *on_cmd(const char *type, const json_t *args, char *detail, size_t n, void *u)
{
    (void)args; (void)u;
    if (!strcmp(type, "resync")) { resyncs++; snprintf(detail, n, "resync #%d", resyncs); printf("EXEC resync %d\n", resyncs); fflush(stdout); return "done"; }
    return "unsupported";
}

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: url device token trust state_dir seconds\n"); return 2; }
    openlog("harness", LOG_PERROR, LOG_USER);
    pharos_settings s = { 0 };
    snprintf(s.url, sizeof s.url, "%s", argv[1]);
    snprintf(s.device_id, sizeof s.device_id, "%s", argv[2]);
    snprintf(s.token, sizeof s.token, "%s", argv[3]);
    snprintf(s.trust, sizeof s.trust, "%s", argv[4]);
    snprintf(s.state_dir, sizeof s.state_dir, "%s", argv[5]);
    snprintf(s.sw_version, sizeof s.sw_version, "0.4.0-test");
    snprintf(s.model_version, sizeof s.model_version, "mobilefacenet-128-int8-dlpu");
    snprintf(s.hw_model, sizeof s.hw_model, "P3267-LV");
    snprintf(s.serial, sizeof s.serial, "B8A44FD5707E");
    snprintf(s.firmware, sizeof s.firmware, "12.11.118");
    s.default_threshold = 0.45;
    pharos_hooks h = { on_apply, on_snapshot, on_state, on_cmd, NULL };
    pharos *p = pharos_start(&s, &h);
    usleep((useconds_t)(atof(argv[6]) * 1e6));
    pharos_stop(p);
    printf("END\n");
    return 0;
}
