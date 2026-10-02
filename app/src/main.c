/* AURIX - on-camera face matching ACAP (benchmark build).
 * Pipeline: VDO -> larod detect -> CPU 5-pt align -> larod embed -> NEON cosine match -> log.
 * Research weights are for internal benchmarking only and are never committed or shipped. */
#include <signal.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "align.h"
#include "capture.h"
#include "config.h"
#include "detect.h"
#include "embed.h"
#include "infer.h"
#include "match.h"

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

typedef struct { double total; unsigned n; } stat_acc;
static void acc(stat_acc *s, double v) { s->total += v; s->n++; }
static double avg(const stat_acc *s) { return s->n ? s->total / s->n : 0.0; }

static void idle_until_stopped(void)
{
    /* runMode is "respawn": exiting would restart-loop, so wait for SIGTERM instead. */
    while (running) sleep(1);
}

int main(void)
{
    openlog("aurix", LOG_PID | LOG_CONS, LOG_USER);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    aurix_config cfg;
    config_defaults(&cfg);
    syslog(LOG_INFO, "starting, larod device %s", cfg.device);

    aurix_gallery gallery;
    if (gallery_load(cfg.gallery_path, &gallery))
        syslog(LOG_WARNING, "no gallery at %s - running detect/embed benchmark only", cfg.gallery_path);
    else
        syslog(LOG_INFO, "gallery: %u identities, dim %u", gallery.count, gallery.dim);

    aurix_detector *det = detector_open(cfg.detect_model, cfg.detect_meta, cfg.device);
    aurix_model *emb = model_load(cfg.embed_model, cfg.device);
    /* PAD/liveness hook: load cfg.pad_model here when door-access mode is added. */
    if (!emb) syslog(LOG_WARNING, "no embedder - running detection benchmark only");
    aurix_capture *cap = det ? capture_open(cfg.width, cfg.height, cfg.fps) : NULL;

    if (!det || !cap) {
        syslog(LOG_ERR, "pipeline not ready (detector missing or VDO failed) - idling");
        idle_until_stopped();
        goto out;
    }

    aurix_face faces[AURIX_MAX_FACES];
    uint8_t face_px[AURIX_FACE_SIZE * AURIX_FACE_SIZE * 3];
    aurix_image face = { face_px, AURIX_FACE_SIZE, AURIX_FACE_SIZE, AURIX_FACE_SIZE * 3, 3 };
    int8_t q[AURIX_MAX_DIM];
    stat_acc t_cap = {0}, t_det = {0}, t_align = {0}, t_emb = {0}, t_match = {0};
    unsigned frames = 0, faces_detected = 0, faces_gated = 0, faces_embedded = 0;
    unsigned max_faces = cfg.max_faces < AURIX_MAX_FACES ? cfg.max_faces : AURIX_MAX_FACES;

    while (running) {
        aurix_image frame;
        double t0 = now_ms();
        if (capture_next(cap, &frame)) continue;
        double t1 = now_ms();
        int n = detect_faces(det, &frame, faces, (int)max_faces, cfg.detect_threshold);
        double t2 = now_ms();
        acc(&t_cap, t1 - t0);
        acc(&t_det, t2 - t1);
        if (n > 0) faces_detected += (unsigned)n;

        for (int i = 0; i < n; i++) {
            if (landmarks_eye_distance(&faces[i].lm) < (float)cfg.min_eye_px) continue;
            faces_gated++;
            if (!emb) continue;
            double a0 = now_ms();
            if (align_face(&frame, &faces[i].lm, &face)) continue;
            double a1 = now_ms();
            int dim = embed_face(emb, &face, q, AURIX_MAX_DIM, cfg.embed_zero_point);
            double a2 = now_ms();
            acc(&t_align, a1 - a0);
            acc(&t_emb, a2 - a1);
            faces_embedded++;
            if (dim <= 0 || gallery.count == 0 || (uint32_t)dim != gallery.dim) continue;

            float score;
            int idx = gallery_best(&gallery, q, &score);
            acc(&t_match, now_ms() - a2);
            /* TODO: replace syslog with Axis event (and later Device Data Hub on AXIS OS 13). */
            if (idx >= 0 && score >= cfg.match_threshold)
                syslog(LOG_INFO, "MATCH id=%s score=%.3f", gallery.ids[idx], score);
        }

        if (++frames % cfg.stats_every == 0)
            syslog(LOG_INFO,
                   "stats frames=%u faces det=%u gated=%u emb=%u | ms: capture %.1f detect %.1f align %.2f embed %.1f match %.2f",
                   frames, faces_detected, faces_gated, faces_embedded, avg(&t_cap), avg(&t_det), avg(&t_align), avg(&t_emb), avg(&t_match));
    }

out:
    capture_close(cap);
    detector_close(det);
    model_free(emb);
    gallery_free(&gallery);
    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
