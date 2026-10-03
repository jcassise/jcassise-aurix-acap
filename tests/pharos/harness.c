/* Host harness: runs the real AURIX Pharos client with fake hooks; prints machine-readable lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "pharos.h"
#include "jpeg.h"
#include "events.h"
#include "event_queue.h"
#include "tracker.h"
#include <time.h>

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

static long long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* AURIX_TEST_EVENTS=N: drive the real tracker + event builder with N synthetic visits (open, then close),
 * each with face and scene JPEGs, into the real queue -> uploader -> Pharos. */
static void push_test_events(const char *device_id, int n, int with_scene)
{
    trk_params tp = { 0.45f, 0.33f, 2, 3, 3, 3000, 1000 };
    tracker tr;
    tracker_init(&tr, &tp);
    unsigned char px[64 * 64 * 3];
    for (int i = 0; i < (int)sizeof px; i++) px[i] = (unsigned char)(i * 7);
    aurix_image img = { px, 64, 64, 64 * 3, 3 };
    for (int v = 0; v < n; v++) {
        long long now = ms_now();
        float box[1][4] = { { 100.0f + v * 300, 100, 200.0f + v * 300, 220 } };
        int ti;
        tracker_associate(&tr, (const float (*)[4])box, 1, now, &ti);
        int known = v % 2 == 0;
        char key[16];
        snprintf(key, sizeof key, "p-%d", v);
        for (int f = 0; f < 3; f++)
            tracker_observe(&tr, ti, known ? key : "", known ? "Test Person" : "", known ? key : "", known ? 1 : 0,
                            known ? 0.7f : 0.1f, -1, 0.5f + 0.1f * f, 120, now);
        const char *wl = known ? "threat" : NULL;
        event_ctx c = { device_id, "mobilefacenet-128-int8-dlpu", "Lobby", 0.45f, wl, 0, { 0, "", "" }, 1, with_scene, 0 };
        size_t fl = 0, sl = 0;
        unsigned char *face = jpeg_encode_rgb(&img, 85, &fl);
        unsigned char *scene = with_scene ? jpeg_encode_rgb(&img, 75, &sl) : NULL;
        json_t *e1 = event_build(&tr.t[ti], 1, 0, now, &c);
        char *j1 = json_dumps(e1, JSON_COMPACT | JSON_REAL_PRECISION(6));
        json_decref(e1);
        eq_push(tr.t[ti].event_id, 1, event_priority(&tr.t[ti], wl), now, j1, face, fl, scene, sl);
        trk_track closed;
        tracker_close(&tr, ti, now + 500, &closed);
        json_t *e2 = event_build(&closed, 2, 1, now + 600, &c);
        char *j2 = json_dumps(e2, JSON_COMPACT | JSON_REAL_PRECISION(6));
        json_decref(e2);
        eq_push(closed.event_id, 2, event_priority(&closed, wl), now, j2, NULL, 0, NULL, 0);
    }
    printf("TESTEVENTS %d queued\n", n);
    fflush(stdout);
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
    if (getenv("AURIX_TEST_EVENTS")) push_test_events(s.device_id, atoi(getenv("AURIX_TEST_EVENTS")), !getenv("AURIX_TEST_NO_SCENE"));
    usleep((useconds_t)(atof(argv[6]) * 1e6));
    pharos_stop(p);
    printf("END\n");
    return 0;
}
