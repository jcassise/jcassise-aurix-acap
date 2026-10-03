/* Host harness: runs the real AURIX Pharos client with fake hooks; prints machine-readable lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "pharos.h"
#include "jpeg.h"

static int resyncs;

/* Fake enrolment: decodes the JPEG for real, derives a template from its pixels (no larod on the host). */
static int on_enroll(const unsigned char *jpeg, size_t len, int8_t emb[PS_DIM], char *why, size_t n, void *u)
{
    (void)u;
    aurix_image im;
    if (jpeg_decode_rgb(jpeg, len, 1600, &im, why, n)) return -1;
    unsigned h = 2166136261u;
    for (int i = 0; i < im.w * im.h * 3; i += 7) h = (h ^ im.data[i]) * 16777619u;
    for (int i = 0; i < PS_DIM; i++) { h = h * 1103515245u + 12345u; emb[i] = (int8_t)((h >> 16) % 200 - 100); }
    free(im.data);
    printf("ENROLL ok\n"); fflush(stdout);
    return 0;
}

static void on_people(const ps_store *s, void *u)
{
    (void)u;
    int r, f, p;
    ps_counts(s, "mobilefacenet-128-int8-dlpu", &r, &f, &p);
    printf("PEOPLE n=%d ready=%d failed=%d pending=%d rev=%lld names=", s->n, r, f, p, s->revision);
    for (int i = 0; i < s->n; i++) printf("%s%s", i ? "," : "", s->v[i].display_name);
    printf("\n");
    fflush(stdout);
}

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
    if (!strcmp(type, "captureScene")) { resyncs++; snprintf(detail, n, "capture #%d", resyncs); printf("EXEC captureScene %d\n", resyncs); fflush(stdout); return "done"; }
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
    s.max_people = 20000;
    pharos_hooks h = { on_apply, on_snapshot, on_state, on_cmd, on_enroll, on_people, NULL };
    pharos *p = pharos_start(&s, &h);
    usleep((useconds_t)(atof(argv[6]) * 1e6));
    pharos_stop(p);
    printf("END\n");
    return 0;
}
