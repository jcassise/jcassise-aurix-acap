#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "align.h"
#include "image.h"
#include "match.h"
#include "yunet.h"
#include "tensor.h"
#include "embed_meta.h"
#include "sysinfo.h"
#include "capacity.h"
#include "jpeg.h"
#include "tracker.h"
#include "snapshot.h"
#include "access.h"
#include <glib.h>

#ifndef FIXTURES
#define FIXTURES "fixtures"
#endif

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_align_recovers_known_transform(void)
{
    /* Place template points into a frame with scale 2.5, rotation 20 deg, offset (400,300). */
    const float s = 2.5f, th = 20.0f * (float)M_PI / 180.0f, tx = 400, ty = 300;
    const float a = s * cosf(th), b = s * sinf(th);
    aurix_landmarks lm;
    for (int i = 0; i < 5; i++) {
        float u = AURIX_TEMPLATE_112[i][0], v = AURIX_TEMPLATE_112[i][1];
        lm.x[i] = a * u - b * v + tx;
        lm.y[i] = b * u + a * v + ty;
    }
    float m[6];
    CHECK(align_similarity(&lm, 112, m) == 0, "similarity fit failed");
    const float want[6] = { a, -b, tx, b, a, ty };
    for (int k = 0; k < 6; k++)
        CHECK(fabsf(m[k] - want[k]) < 1e-3f, "m[%d]=%f want %f", k, m[k], want[k]);
    CHECK(fabsf(landmarks_eye_distance(&lm) - s * 35.24f) < 0.1f, "eye distance %f", landmarks_eye_distance(&lm));
}

static void test_resize_identity_and_constant(void)
{
    uint8_t src_px[8 * 6 * 3], dst_px[4 * 3 * 3];
    for (size_t i = 0; i < sizeof(src_px); i++) src_px[i] = 77;
    aurix_image src = { src_px, 8, 6, 8 * 3, 3 }, dst = { dst_px, 4, 3, 4 * 3, 3 };
    resize_bilinear(&src, &dst);
    for (size_t i = 0; i < sizeof(dst_px); i++) CHECK(dst_px[i] == 77, "resize constant px %zu = %u", i, dst_px[i]);

    uint8_t g[5 * 5], o[5 * 5];
    for (int i = 0; i < 25; i++) g[i] = (uint8_t)(i * 10);
    aurix_image gs = { g, 5, 5, 5, 1 }, go = { o, 5, 5, 5, 1 };
    const float id[6] = { 1, 0, 0, 0, 1, 0 };
    warp_affine_bilinear(&gs, &go, id);
    CHECK(memcmp(g, o, 25) == 0, "identity warp changed pixels");
}

static void test_nv12_gray(void)
{
    uint8_t y[4 * 2], uv[4 * 1], rgb[4 * 2 * 3];
    memset(y, 126, sizeof(y));   /* ~ mid grey in limited range */
    memset(uv, 128, sizeof(uv));
    nv12_to_rgb(y, uv, 4, 2, 4, rgb);
    for (size_t i = 0; i < sizeof(rgb); i++) CHECK(abs(rgb[i] - 128) <= 1, "nv12 grey px %zu = %u", i, rgb[i]);
}

static void test_dot_and_match(void)
{
    enum { DIM = 515 }; /* deliberately not a multiple of 16 to exercise the tail */
    float f[3][DIM];
    srand(1);
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < DIM; i++) f[k][i] = (float)rand() / RAND_MAX - 0.5f;

    int8_t q[3][DIM];
    for (int k = 0; k < 3; k++) embedding_quantize(f[k], DIM, q[k]);

    int32_t ref = 0;
    for (int i = 0; i < DIM; i++) ref += q[0][i] * q[1][i];
    CHECK(dot_s8(q[0], q[1], DIM) == ref, "dot_s8 mismatch");

    aurix_gallery g = { 0 };
    g.dim = DIM; g.count = 3;
    g.ids = calloc(3, AURIX_ID_LEN);
    g.emb = malloc(3 * DIM);
    g.inv_norm = malloc(3 * sizeof(float));
    for (int k = 0; k < 3; k++) {
        snprintf(g.ids[k], AURIX_ID_LEN, "person-%d", k);
        memcpy(g.emb + k * DIM, q[k], DIM);
        g.inv_norm[k] = 1.0f / sqrtf((float)dot_s8(q[k], q[k], DIM));
    }
    float score;
    int idx = gallery_best(&g, q[2], &score);
    CHECK(idx == 2 && score > 0.999f, "self-match idx=%d score=%f", idx, score);
    gallery_free(&g);
}

static void test_gallery_file_roundtrip(void)
{
    const char *path = "/tmp/aurix_test_gallery.bin";
    FILE *f = fopen(path, "wb");
    const uint32_t hdr[3] = { 1, 4, 2 };      /* version, dim, count (little-endian host) */
    fwrite("AURG", 1, 4, f);
    fwrite(hdr, 4, 3, f);
    char id[AURIX_ID_LEN] = "alice";
    int8_t e1[4] = { 127, 0, 0, 0 }, e2[4] = { 0, 127, 0, 0 };
    fwrite(id, 1, AURIX_ID_LEN, f); fwrite(e1, 1, 4, f);
    memset(id, 0, sizeof(id)); strcpy(id, "bob");
    fwrite(id, 1, AURIX_ID_LEN, f); fwrite(e2, 1, 4, f);
    fclose(f);

    aurix_gallery g;
    CHECK(gallery_load(path, &g) == 0, "gallery_load failed");
    CHECK(g.count == 2 && g.dim == 4, "count=%u dim=%u", g.count, g.dim);
    float score;
    int8_t probe[4] = { 10, 100, 0, 0 };
    int idx = gallery_best(&g, probe, &score);
    CHECK(idx == 1 && strcmp(g.ids[idx], "bob") == 0, "expected bob, got %d", idx);
    gallery_free(&g);
    remove(path);
}

static void test_yunet_decode_matches_reference(void)
{
    yunet_meta meta;
    CHECK(yunet_meta_load(FIXTURES "/yunet_astro.meta", &meta) == 0, "meta load failed");
    CHECK(meta.in_w == 640 && meta.in_h == 352, "meta size %dx%d", meta.in_w, meta.in_h);

    /* Outputs are concatenated in model-output order; size each from its role/stride. */
    static const int ch[YUNET_ROLES] = { 1, 1, 4, 10 };
    size_t size[YUNET_OUTPUTS] = { 0 };
    for (int l = 0; l < YUNET_LEVELS; l++)
        for (int r = 0; r < YUNET_ROLES; r++)
            size[meta.t[l][r].index] = (size_t)(640 / (8 << l)) * (352 / (8 << l)) * ch[r];
    size_t total = 0;
    for (int i = 0; i < YUNET_OUTPUTS; i++) total += size[i];

    FILE *f = fopen(FIXTURES "/yunet_astro_outputs.raw", "rb");
    CHECK(f != NULL, "missing outputs fixture");
    if (!f) return;
    int8_t *blob = malloc(total);
    CHECK(fread(blob, 1, total, f) == total, "short outputs fixture");
    fclose(f);
    const int8_t *outs[YUNET_OUTPUTS];
    size_t off = 0;
    for (int i = 0; i < YUNET_OUTPUTS; i++) { outs[i] = blob + off; off += size[i]; }

    aurix_face faces[8];
    int n = yunet_decode(&meta, outs, faces, 8, 0.6f, 0.3f);

    FILE *e = fopen(FIXTURES "/yunet_astro_expected.txt", "r");
    int want = -1;
    CHECK(e && fscanf(e, "%d", &want) == 1, "expected fixture unreadable");
    CHECK(n == want, "decoded %d faces, expected %d", n, want);
    for (int i = 0; i < n && i < want; i++) {
        float v[15];
        for (int k = 0; k < 15; k++) if (fscanf(e, "%f", &v[k]) != 1) v[k] = NAN;
        const aurix_face *d = &faces[i];
        const float got[15] = { d->score, d->x0, d->y0, d->x1, d->y1,
                                d->lm.x[0], d->lm.y[0], d->lm.x[1], d->lm.y[1], d->lm.x[2], d->lm.y[2],
                                d->lm.x[3], d->lm.y[3], d->lm.x[4], d->lm.y[4] };
        for (int k = 0; k < 15; k++)
            CHECK(fabsf(got[k] - v[k]) < 0.01f, "face %d field %d: got %.3f want %.3f", i, k, got[k], v[k]);
    }
    if (e) fclose(e);

    /* landmarks feed straight into alignment: image-left eye must be left of image-right eye */
    if (n > 0) CHECK(faces[0].lm.x[0] < faces[0].lm.x[1], "eye order wrong");
    free(blob);
}

static void test_tensor_repack(void)
{
    /* NHWC 1x3x5x2 int8 where each row is padded to 16 bytes and the image to 64. */
    const size_t dims[4] = { 1, 3, 5, 2 }, pitches[4] = { 64, 64, 16, 2 };
    uint8_t src[64], dst[30];
    memset(src, 0xEE, sizeof src);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 5; x++)
            for (int c = 0; c < 2; c++) src[y * 16 + x * 2 + c] = (uint8_t)(y * 10 + x * 2 + c);
    CHECK(tensor_is_padded(4, dims, pitches, 1) == 1, "padding not detected");
    tensor_repack(dst, src, 4, dims, pitches, 1);
    for (int i = 0; i < 30; i++) {
        int y = i / 10, rem = i % 10;
        CHECK(dst[i] == (uint8_t)(y * 10 + rem), "repack[%d]=%u", i, dst[i]);
    }
    const size_t packed[4] = { 30, 30, 10, 2 };
    CHECK(tensor_is_padded(4, dims, packed, 1) == 0, "packed layout flagged as padded");
    /* 2-D float embedding with padded innermost row: 1x4 floats in a 32-byte row */
    const size_t d2[2] = { 1, 4 }, p2[2] = { 32, 32 };
    float fs[8] = { 1, 2, 3, 4, -9, -9, -9, -9 }, fd[4];
    CHECK(tensor_is_padded(2, d2, p2, 4) == 1, "2-D padding not detected");
    tensor_repack(fd, fs, 2, d2, p2, 4);
    CHECK(fd[0] == 1 && fd[3] == 4, "2-D repack wrong");
}

static void test_embed_meta(void)
{
    embed_meta m;
    CHECK(embed_meta_load(FIXTURES "/embed_dlpu.meta", &m) == 0, "embed meta load failed");
    CHECK(m.in_w == 112 && m.in_h == 112 && m.dim == 128, "embed meta %dx%d dim %d", m.in_w, m.in_h, m.dim);
    CHECK(m.zero_point >= -128 && m.zero_point <= 127 && m.scale > 0, "embed meta quant %f %d", m.scale, m.zero_point);
    CHECK(embed_meta_load(FIXTURES "/yunet_astro.meta", &m) != 0, "detector meta accepted as embedder meta");
}

static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static void test_gallery_param(void)
{
    enum { D = 128 };
    int8_t e1[D], e2[D];
    for (int i = 0; i < D; i++) { e1[i] = (int8_t)(i % 2 ? 90 : -40); e2[i] = (int8_t)(i < 64 ? 100 : -100); }
    char b1[200], b2[200], s[1024];
    b64((const uint8_t *)e1, D, b1);
    b64((const uint8_t *)e2, D, b2);

    aurix_gallery g;
    gallery_init(&g, D);
    snprintf(s, sizeof s, " John Cassise ,allow,dlpu,%s ; Bad Guy,threat,dlpu,%s;Other Cam,allow,cpu,%s;", b1, b2, b1);
    int skk = 0, sks = 0;
    int n = gallery_parse_param(&g, s, "dlpu", &skk, &sks);
    CHECK(n == 2 && g.count == 2 && skk == 1 && sks == 0, "parsed %d (count %u, skipped %d/%d), want 2", n, g.count, skk, sks);
    CHECK(!strcmp(g.ids[0], "John Cassise") && g.category[0] == AURIX_CAT_ALLOW, "entry 0 = '%s' cat %u", g.ids[0], g.category[0]);
    CHECK(!strcmp(g.ids[1], "Bad Guy") && g.category[1] == AURIX_CAT_THREAT, "entry 1 = '%s' cat %u", g.ids[1], g.category[1]);
    float score;
    CHECK(gallery_best(&g, e2, &score) == 1 && score > 0.999f, "lookup threat entry failed (score %f)", score);

    /* malformed: unknown category -> -1 and nothing from this call kept */
    snprintf(s, sizeof s, "Ok Person,allow,dlpu,%s;Broken,maybe,dlpu,%s", b1, b1);
    CHECK(gallery_parse_param(&g, s, "dlpu", NULL, NULL) == -1 && g.count == 2, "malformed string not rolled back (count %u)", g.count);
    /* wrong embedding size is skipped, not an error */
    CHECK(gallery_parse_param(&g, "Short,allow,dlpu,AAAA", "dlpu", &skk, &sks) == 0 && sks == 1 && g.count == 2, "short embedding accepted");
    CHECK(gallery_parse_param(&g, "", "dlpu", NULL, NULL) == 0 && gallery_parse_param(&g, NULL, "dlpu", NULL, NULL) == 0, "empty string not ok");
    /* '+' turned into ' ' by a web form must still decode to the same embedding */
    char mangled[1024];
    snprintf(mangled, sizeof mangled, "Form Mangled,allow,dlpu,%s", b2);
    for (char *c = mangled; *c; c++) if (*c == '+') *c = ' ';
    CHECK(gallery_parse_param(&g, mangled, "dlpu", NULL, NULL) == 1, "space-mangled base64 rejected");
    CHECK(gallery_best(&g, e2, &score) >= 0 && score > 0.999f, "space-mangled entry decoded wrong");
    gallery_free(&g);
}

static void test_gallery_v2_file(void)
{
    const char *path = "/tmp/aurix_test_gallery_v2.bin";
    FILE *f = fopen(path, "wb");
    const uint32_t hdr[3] = { 2, 4, 1 };
    fwrite("AURG", 1, 4, f); fwrite(hdr, 4, 3, f);
    char id[AURIX_ID_LEN] = "watched"; uint8_t cat = 1; int8_t e[4] = { 0, 0, 127, 0 };
    fwrite(id, 1, AURIX_ID_LEN, f); fwrite(&cat, 1, 1, f); fwrite(e, 1, 4, f);
    fclose(f);
    aurix_gallery g;
    CHECK(gallery_load(path, &g) == 0 && g.count == 1 && g.category[0] == AURIX_CAT_THREAT, "v2 gallery load failed");
    gallery_free(&g);
    remove(path);
}

static void test_sysinfo_parsers(void)
{
    uint64_t tot = 0, av = 0;
    CHECK(sysinfo_parse_meminfo("MemTotal:        1009152 kB\nMemFree:  100 kB\nMemAvailable:     612340 kB\n", &tot, &av) == 0
          && tot == 1009152 && av == 612340, "meminfo %llu %llu", (unsigned long long)tot, (unsigned long long)av);
    CHECK(sysinfo_parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 250 kB\n", &tot, &av) == 0
          && av == 400, "meminfo fallback %llu", (unsigned long long)av);
    cpu_counters c;
    int cores = 0;
    CHECK(sysinfo_parse_stat("cpu  100 0 50 800 50 0 0 0\ncpu0 50 0 25 400 25 0 0 0\ncpu1 50 0 25 400 25 0 0 0\nintr 1\n",
                             &c, &cores) == 0 && cores == 2 && c.total == 1000 && c.idle == 850,
          "stat cores %d total %llu idle %llu", cores, c.total, c.idle);
}

static void test_capacity(void)
{
    /* 512 MB free RAM, 400 MB free storage, 25 ns per comparison */
    cap_result r = capacity_estimate(512u * 1024, 400u * 1024, 25.0);
    CHECK(r.by_memory == (512u - 96) * 1024u * 1024 / 1024, "by_memory %llu", (unsigned long long)r.by_memory);
    CHECK(r.by_storage == (400u - 32) * 1024u * 1024 / 8192, "by_storage %llu", (unsigned long long)r.by_storage);
    CHECK(r.by_matching == 200000, "by_matching %llu", (unsigned long long)r.by_matching);
    CHECK(r.limited_by == CAP_BY_STORAGE && r.estimate == r.by_storage, "limit %d", r.limited_by);
    CHECK(fabs(r.match_ms_at_estimate - r.estimate * 25e-6) < 1e-9, "match ms %f", r.match_ms_at_estimate);
    /* slow chip: matching binds */
    r = capacity_estimate(512u * 1024, 4000u * 1024, 2000.0);
    CHECK(r.limited_by == CAP_BY_MATCHING && r.estimate == 2500, "slow chip %d %llu", r.limited_by, (unsigned long long)r.estimate);
    /* unknown benchmark and tiny memory */
    r = capacity_estimate(50u * 1024, 0, 0);
    CHECK(r.limited_by == CAP_UNKNOWN && r.estimate == 0, "unknown %d", r.limited_by);
    double ns = gallery_benchmark_ns_per_entry(128, 2048, 5);
    CHECK(ns > 0 && ns < 100000, "benchmark %f ns", ns);
    printf("info: matching benchmark here %.1f ns per entry\n", ns);
}

static void test_jpeg_and_letterbox(void)
{
    FILE *f = fopen(FIXTURES "/astronaut_portrait.jpg", "rb");
    CHECK(f != NULL, "missing jpeg fixture");
    if (!f) return;
    static unsigned char buf[200000];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    aurix_image im;
    char why[128] = "";
    CHECK(jpeg_decode_rgb(buf, n, 1600, &im, why, sizeof why) == 0 && im.w == 300 && im.h == 450, "decode %dx%d %s", im.w, im.h, why);
    /* a skin-tone pixel near the face centre survives decoding (astronaut face ~ (150,120) in the crop) */
    const uint8_t *p = im.data + (120 * im.w + 150) * 3;
    CHECK(p[0] > p[2] && p[0] > 120, "face pixel %u,%u,%u", p[0], p[1], p[2]);
    aurix_image small;
    CHECK(jpeg_decode_rgb(buf, n, 100, &small, why, sizeof why) == 0 && small.h == 100 && small.w == 67, "downscale %dx%d", small.w, small.h);
    free(small.data);
    aurix_image c;
    int ox, oy;
    CHECK(image_letterbox(&im, 640.0 / 352.0, &c, &ox, &oy) == 0, "letterbox failed");
    CHECK(c.h == 450 && fabs((double)c.w / c.h - 640.0 / 352.0) < 0.01 && oy == 0 && ox == (c.w - 300) / 2,
          "letterbox %dx%d off %d,%d", c.w, c.h, ox, oy);
    CHECK(!memcmp(c.data + ((size_t)10 * c.w + ox) * 3, im.data + (size_t)10 * im.w * 3, (size_t)im.w * 3), "letterbox moved pixels");
    CHECK(c.data[0] == 128 && c.data[1] == 128, "padding not grey");
    free(c.data);
    free(im.data);
    CHECK(jpeg_decode_rgb((const unsigned char *)"not a jpeg at all", 17, 1600, &im, why, sizeof why) != 0 && why[0], "garbage accepted");
}

/* synthetic face embeddings: a fixed vector per person, plus per-frame noise (same person ~0.85-0.9) */
static void person_vec(int person, float *v)
{
    unsigned x = 2654435761u * (unsigned)(person + 1);
    for (int i = 0; i < TRK_DIM; i++) { x = x * 1103515245u + 12345u; v[i] = (float)((int)(x >> 16) % 2001 - 1000) / 1000.0f; }
}

static void face_emb(int person, unsigned *seed, int8_t *out, float noise)
{
    float v[TRK_DIM], n2 = 0;
    person_vec(person, v);
    for (int i = 0; i < TRK_DIM; i++) {
        *seed = *seed * 1103515245u + 12345u;
        v[i] += noise * (float)((int)(*seed >> 16) % 2001 - 1000) / 1000.0f;
        n2 += v[i] * v[i];
    }
    float inv = 1.0f / sqrtf(n2);
    for (int i = 0; i < TRK_DIM; i++) out[i] = (int8_t)lrintf(v[i] * inv * 127.0f);
}

/* gallery score of a face embedding against a person's template (the noise-free vector) */
static float gscore(const int8_t *e, int person)
{
    float v[TRK_DIM], d = 0, a = 0, b = 0;
    person_vec(person, v);
    for (int i = 0; i < TRK_DIM; i++) { d += e[i] * v[i]; a += e[i] * e[i]; b += v[i] * v[i]; }
    return d / sqrtf(a * b);
}

enum { P_DANA = 1, P_OTHER = 2, P_KYLE = 3, P_WOMAN = 4 };

/* one detection's identity check, as main.c does it; gallery = { Dana, Other, Kyle } (the woman is not enrolled) */
static int check(tracker *tr, int ti, int person, unsigned *seed, float noise, long long t)
{
    int8_t e[TRK_DIM];
    face_emb(person, seed, e, noise);
    static const int gal[3] = { P_DANA, P_OTHER, P_KYLE };
    static const char *keys[3] = { "dana", "other", "kyle" }, *names[3] = { "Dana", "Other", "Kyle" };
    int best = 0;
    float bs = -1;
    for (int g = 0; g < 3; g++) { float sc = gscore(e, gal[g]); if (sc > bs) { bs = sc; best = g; } }
    /* real embeddings are not this clean: scale scores so an enrolled face scores ~0.55-0.65 */
    bs *= 0.65f;
    float own = -1;
    if (tr->t[ti].state == TS_KNOWN)
        for (int g = 0; g < 3; g++) if (!strcmp(tr->t[ti].key, keys[g])) own = gscore(e, gal[g]) * 0.65f;
    return tracker_observe(tr, ti, e, TRK_DIM, keys[best], names[best], keys[best], 0, bs, own, 0.6f, 120, t);
}

static void test_tracker(void)
{
    trk_params p = { 0.45f, 0.33f, 2, 3, 3, 3000, 1000, 0.30f };
    tracker tr;
    tracker_init(&tr, &p);
    unsigned seed = 1;
    float b[2][4] = { { 100, 100, 200, 220 }, { 600, 100, 700, 220 } };
    int ti[2];
    long long t = 1000;
    /* T1 a known face locks after two confident frames, one track */
    tracker_associate(&tr, (const float (*)[4])b, 1, t, ti);
    check(&tr, ti[0], P_DANA, &seed, 0.25f, t);
    CHECK(tr.t[ti[0]].state == TS_PENDING, "locked after one frame");
    t += 100; tracker_associate(&tr, (const float (*)[4])b, 1, t, ti);
    check(&tr, ti[0], P_DANA, &seed, 0.25f, t);
    CHECK(tr.t[ti[0]].state == TS_KNOWN && tr.t[ti[0]].needs_emit && !strcmp(tr.t[ti[0]].name, "Dana"), "not locked after two");
    int first = ti[0];
    char eid[37]; memcpy(eid, tr.t[first].event_id, 37);
    tr.t[first].needs_emit = 0; tr.t[first].revision = 1;
    /* T2 head turns: weak gallery scores, but still the same face -> name kept, no new event */
    for (int k = 0; k < 5; k++) {
        t += 100; tracker_associate(&tr, (const float (*)[4])b, 1, t, ti);
        int8_t e[TRK_DIM];
        face_emb(P_DANA, &seed, e, 0.25f);
        int split = tracker_observe(&tr, ti[0], e, TRK_DIM, "other", "Other", "other", 0, 0.30f, 0.20f, 0.3f, 100, t);
        CHECK(!split && ti[0] == first && tr.t[first].state == TS_KNOWN && !tr.t[first].needs_emit, "head turn changed the track");
    }
    /* T3 a different face in the track -> split after two checks, whatever the gallery says */
    int split = 0;
    for (int k = 0; k < 2 && !split; k++) {
        t += 100; tracker_associate(&tr, (const float (*)[4])b, 1, t, ti);
        split = check(&tr, ti[0], P_WOMAN, &seed, 0.25f, t);
        CHECK(k == 1 || !split, "split on a single odd frame");
    }
    CHECK(split == 1, "a different face did not split the track");
    tracker_detach(&tr, first, t);
    /* T4 Dana comes back within the close window -> same event continues (no duplicate) */
    t += 300;
    for (int k = 0; k < 3; k++) { t += 100; tracker_associate(&tr, (const float (*)[4])b, 1, t, ti); check(&tr, ti[0], P_DANA, &seed, 0.25f, t); }
    CHECK(tr.t[ti[0]].state == TS_KNOWN && !strcmp(tr.t[ti[0]].event_id, eid) && tr.t[ti[0]].revision == 1,
          "returning person did not continue their event");
    /* T5 leaves: closed after close_ms, ended at last sighting */
    long long last = t;
    trk_track out[TRK_MAX];
    int n0 = tracker_expire(&tr, last + 2900, out, TRK_MAX);
    CHECK(n0 == 0, "closed too early");
    int nc = tracker_expire(&tr, last + 3100, out, TRK_MAX);
    int dana_closed = 0;
    for (int k = 0; k < nc; k++) dana_closed += !strcmp(out[k].event_id, eid) && out[k].ended_ms == last;
    CHECK(dana_closed == 1, "Dana's event not closed exactly once after the absence (%d)", nc);
    /* T6 two people side by side keep their tracks */
    t += 5000; tracker_associate(&tr, (const float (*)[4])b, 2, t, ti);
    int a0 = ti[0], a1 = ti[1];
    for (int k = 0; k < 5; k++) {
        b[0][0] += 8; b[0][2] += 8; b[1][0] -= 8; b[1][2] -= 8;
        t += 100; tracker_associate(&tr, (const float (*)[4])b, 2, t, ti);
        CHECK(ti[0] == a0 && ti[1] == a1, "people swapped tracks");
    }
    /* T7 low frame rate: moved past its old box, same size, close by -> same track */
    float j[1][4] = { { b[0][0] + 85, b[0][1] + 10, b[0][2] + 85, b[0][3] + 10 } };
    t += 600; tracker_associate(&tr, (const float (*)[4])j, 1, t, ti);
    CHECK(ti[0] == a0, "jump at low frame rate started a new track");
}

/* John's video: Kyle (enrolled) right, the woman (not enrolled) left; the photos are swapped.
 * mode 0: they vanish for 0.4 s and reappear swapped.  mode 1: they slide past each other. */
static void test_photo_swap(int mode)
{
    trk_params p = { 0.45f, 0.33f, 2, 3, 3, 3000, 1000, 0.30f };
    tracker tr;
    tracker_init(&tr, &p);
    unsigned seed = 99;
    long long t = 0;
    float L[4] = { 100, 100, 200, 220 }, R[4] = { 400, 100, 500, 220 };
    int who_at[2] = { P_WOMAN, P_KYLE };                   /* person in box 0 (left-moving) and box 1 */
    char kyle_event[37] = "", woman_event[37] = "";
    int events_opened = 0, wrong_name_frames = 0;
    for (int f = 0; f < 80; f++) {
        t += 100;
        float bx[2][4];
        int n = 2;
        if (mode == 0) {
            if (f >= 20 && f < 24) n = 0;                 /* hands cover the photos */
            if (f >= 24) { who_at[0] = P_KYLE; who_at[1] = P_WOMAN; }
            memcpy(bx[0], L, sizeof L); memcpy(bx[1], R, sizeof R);
        } else {
            float k = f < 20 ? 0 : f > 40 ? 1 : (f - 20) / 20.0f;   /* slide past each other */
            for (int c = 0; c < 4; c++) { bx[0][c] = L[c] + (R[c] - L[c]) * k; bx[1][c] = R[c] + (L[c] - R[c]) * k; }
            bx[0][1] += 30 * k * (1 - k) * 4; bx[0][3] += 30 * k * (1 - k) * 4;   /* one passes slightly lower */
        }
        int ti[2];
        tracker_associate(&tr, (const float (*)[4])bx, n, t, ti);
        int splits[2], ns = 0;
        for (int d = 0; d < n; d++) {
            if (!tracker_wants_embed(&tr, ti[d], t)) continue;
            if (check(&tr, ti[d], who_at[d], &seed, 0.25f, t)) splits[ns++] = ti[d];
        }
        for (int k = 0; k < ns; k++) tracker_detach(&tr, splits[k], t);
        for (int d = 0; d < n; d++) {
            trk_track *tk = &tr.t[ti[d]];
            if (!tk->active) continue;
            if (tk->needs_emit) {
                if (tk->revision == 0) events_opened++;
                if (tk->state == TS_KNOWN && !kyle_event[0]) memcpy(kyle_event, tk->event_id, 37);
                if (tk->state == TS_STRANGER && !woman_event[0]) memcpy(woman_event, tk->event_id, 37);
                tk->revision++;
                tk->needs_emit = 0;
            }
            /* the bug John saw: the woman's face shown with Kyle's name */
            if (who_at[d] == P_WOMAN && tk->state == TS_KNOWN && !strcmp(tk->name, "Kyle") && f > 2) wrong_name_frames++;
        }
        trk_track out[TRK_MAX];
        tracker_expire(&tr, t, out, TRK_MAX);
    }
    int kyle_ok = 0, woman_ok = 0;
    for (int k = 0; k < TRK_MAX; k++) {
        const trk_track *tk = &tr.t[k];
        if (!tk->active || !tk->seen_now) continue;
        int person = tk->x0 < 300 ? P_KYLE : P_WOMAN;   /* both cases: Kyle ends up on the left */
        if (person == P_KYLE) kyle_ok = tk->state == TS_KNOWN && !strcmp(tk->name, "Kyle") && !strcmp(tk->event_id, kyle_event);
        else woman_ok = tk->state == TS_STRANGER && !strcmp(tk->event_id, woman_event);
    }
    CHECK(wrong_name_frames <= 2, "mode %d: woman shown as Kyle for %d frames", mode, wrong_name_frames);
    CHECK(kyle_ok && woman_ok, "mode %d: after the swap Kyle=%d (same event) woman=stranger %d (same event)", mode, kyle_ok, woman_ok);
    CHECK(events_opened == 2, "mode %d: %d events opened (want 2: Kyle and the woman, continued through the swap)", mode, events_opened);
}

static long long la_ms(int y, int mo, int d, int h, int mi)
{
    GTimeZone *tz = g_time_zone_new_identifier("America/Los_Angeles");
    GDateTime *dt = g_date_time_new(tz, y, mo, d, h, mi, 0);
    long long ms = (long long)g_date_time_to_unix(dt) * 1000;
    g_date_time_unref(dt);
    g_time_zone_unref(tz);
    return ms;
}

static void test_access(void)
{
    json_t *pol = json_loads("[{\"policyId\":\"staff\",\"name\":\"Staff\",\"banned\":false,\"allZonesAllTimes\":false,"
        "\"rules\":[{\"zones\":[\"Lobby\"],\"schedule\":[{\"days\":[\"mon\",\"tue\",\"wed\",\"thu\",\"fri\"],\"start\":\"07:00\",\"end\":\"19:00\"}],"
        "\"excludedDates\":[\"2026-12-25\"]}]},"
        "{\"policyId\":\"admin\",\"name\":\"Admin\",\"banned\":false,\"allZonesAllTimes\":true,\"rules\":[]},"
        "{\"policyId\":\"ban\",\"name\":\"Banned\",\"banned\":true,\"allZonesAllTimes\":false,\"rules\":[]}]", 0, NULL);
    const char zones[1][128] = { "Lobby" }, other[1][128] = { "Vault" };
    const char *staff[] = { "staff" }, *admin[] = { "admin" }, *both[] = { "admin", "ban" };
    const char *tz = "America/Los_Angeles";
    access_result r;
    r = access_evaluate(staff, 1, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 7, 18, 59));   /* Wednesday */
    CHECK(r.granted && !strcmp(r.policy_id, "staff"), "18:59 should be granted (%s)", r.reason);
    r = access_evaluate(staff, 1, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 7, 19, 1));
    CHECK(!r.granted && !strcmp(r.reason, "schedule"), "19:01 should be denied by schedule (%s)", r.reason);
    r = access_evaluate(staff, 1, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 12, 25, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "excluded_date"), "excluded date (%s)", r.reason);
    r = access_evaluate(staff, 1, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 10, 10, 0));   /* Saturday */
    CHECK(!r.granted && !strcmp(r.reason, "schedule"), "weekend (%s)", r.reason);
    r = access_evaluate(staff, 1, -1, -1, 0, pol, other, 1, tz, la_ms(2026, 10, 7, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "zone"), "other zone (%s)", r.reason);
    r = access_evaluate(admin, 1, -1, -1, 0, pol, other, 1, tz, la_ms(2026, 10, 10, 3, 0));
    CHECK(r.granted && !strcmp(r.policy_id, "admin"), "all zones all times (%s)", r.reason);
    r = access_evaluate(both, 2, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 7, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "banned"), "banned wins (%s)", r.reason);
    r = access_evaluate(admin, 1, -1, -1, 1, pol, zones, 1, tz, la_ms(2026, 10, 7, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "watchlist"), "threat always denied (%s)", r.reason);
    r = access_evaluate(admin, 1, la_ms(2026, 11, 1, 0, 0), -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 7, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "validity"), "not yet valid (%s)", r.reason);
    r = access_evaluate(NULL, 0, -1, -1, 0, pol, zones, 1, tz, la_ms(2026, 10, 7, 10, 0));
    CHECK(!r.granted && !strcmp(r.reason, "no_policy"), "no policy (%s)", r.reason);
    json_decref(pol);
}

/* The behaviour John asked for: someone lingering and turning their head = one event, not one per match. */
static void test_linger_one_event(void)
{
    trk_params p = { 0.45f, 0.33f, 2, 3, 3, 3000, 1000, 0.30f };
    tracker tr;
    tracker_init(&tr, &p);
    long long t = 0;
    int opens = 0, closes = 0, splits = 0, ti;
    unsigned seed = 7, es = 5;
    for (int f = 0; f < 600; f++) {                       /* 60 s at 10 fps */
        seed = seed * 1103515245u + 12345u;
        float jx = (float)((seed >> 16) % 13) - 6, jy = (float)((seed >> 8) % 9) - 4;   /* box jitter */
        float b[1][4] = { { 500 + jx, 300 + jy, 640 + jx, 470 + jy } };
        t += 100;
        tracker_associate(&tr, (const float (*)[4])b, 1, t, &ti);
        if (tracker_wants_embed(&tr, ti, t)) {
            /* head turns: every third check is weak (0.28), the rest ~0.55 */
            float sc = (f % 30 < 10) ? 0.28f : 0.55f;
            float own = tr.t[ti].state == TS_KNOWN ? sc : -1;
            int8_t e[TRK_DIM];
            face_emb(P_DANA, &es, e, f % 30 < 10 ? 0.45f : 0.25f);    /* turned head: noisier embedding */
            splits += tracker_observe(&tr, ti, e, TRK_DIM, "p-1", "Dana", "p-1", 0, sc, own, 0.6f, 140, t);
        }
        if (tr.t[ti].needs_emit) { opens += tr.t[ti].revision == 0; tr.t[ti].revision++; tr.t[ti].needs_emit = 0; }
    }
    trk_track out[TRK_MAX];
    int nc = tracker_expire(&tr, t + 3500, out, TRK_MAX);   /* walks away */
    for (int k = 0; k < nc; k++) closes += out[k].revision > 0;
    CHECK(opens == 1 && closes == 1 && splits == 0, "linger: %d opens, %d closes, %d splits", opens, closes, splits);
}

static void test_face_geometry(void)
{
    /* the reference (frontal) face, in 112x112 template coordinates */
    aurix_landmarks lm = { { 38.2946f, 73.5318f, 56.0252f, 41.5493f, 70.7299f }, { 51.6963f, 51.5014f, 71.7366f, 92.3655f, 92.2041f } };
    float yaw, pitch, cx, cy, rx, ry;
    landmarks_pose(&lm, &yaw, &pitch);
    CHECK(fabsf(yaw) < 3 && fabsf(pitch) < 3, "frontal face pose %.1f/%.1f", yaw, pitch);
    landmarks_face_ellipse(&lm, &cx, &cy, &rx, &ry);
    CHECK(fabsf(cx - 56) < 2 && fabsf(cy - 67.9f) < 1.5f && fabsf(ry - 61) < 2 && rx > 0.55f * ry && rx < 0.8f * ry,
          "frontal ellipse centre %.1f,%.1f radii %.1f,%.1f", cx, cy, rx, ry);
    /* the ellipse centre sits above the box centre the old outline used (the cause of 'a little low') */
    CHECK(cy < (51.6f + 92.3f) / 2, "ellipse not above the eye-mouth midpoint");
    /* nose shifted toward image right = turned right */
    aurix_landmarks t = lm;
    t.x[2] += 12;
    landmarks_pose(&t, &yaw, &pitch);
    CHECK(yaw > 25 && yaw < 45, "turned face yaw %.1f", yaw);
    /* nose lower between eyes and mouth = looking down */
    t = lm; t.y[2] += 8;
    landmarks_pose(&t, &yaw, &pitch);
    CHECK(pitch > 20 && fabsf(yaw) < 5, "tilted face pitch %.1f yaw %.1f", pitch, yaw);
    /* a rolled (tilted sideways) frontal face is still frontal */
    t = lm;
    float c = cosf(0.4f), sn = sinf(0.4f);
    for (int k = 0; k < 5; k++) { float x = lm.x[k] - 56, y = lm.y[k] - 70; t.x[k] = 56 + c * x - sn * y; t.y[k] = 70 + sn * x + c * y; }
    landmarks_pose(&t, &yaw, &pitch);
    CHECK(fabsf(yaw) < 4 && fabsf(pitch) < 4, "rolled frontal face pose %.1f/%.1f", yaw, pitch);
}

static void test_snapshots(void)
{
    char why[64];
    for (int shape = 0; shape < 2; shape++) {               /* P3267 (4:3) and P3248 (16:9) analysis frames */
        int W = shape ? 1920 : 1440, H = 1080;
        unsigned char *px = calloc((size_t)W * H * 3, 1);
        aurix_image f = { px, W, H, W * 3, 3 };
        /* a red disc where the face is (centre 700,400) */
        for (int y = 380; y < 420; y++) for (int x = 680; x < 720; x++) { unsigned char *p = px + ((size_t)y * W + x) * 3; p[0] = 255; }
        size_t n = 0;
        unsigned char *j = snapshot_scene(&f, &n);
        aurix_image d;
        CHECK(j && jpeg_decode_rgb(j, n, 4000, &d, why, sizeof why) == 0 && d.w == 1280 && d.h == (shape ? 720 : 960),
              "scene %dx%d -> %dx%d (keeps the camera's shape)", W, H, d.w, d.h);
        free(d.data); free(j);
        /* face close-up centred on the face (landmarks), even though the detector box runs low */
        trk_track t;
        memset(&t, 0, sizeof t);
        t.x0 = 650; t.x1 = 750; t.y0 = 360; t.y1 = 480;             /* box centre 420: lower than the face */
        t.has_face_geo = 1; t.fcx = 700; t.fcy = 400; t.frx = 45; t.fry = 60;
        j = snapshot_face(&f, &t, &n);
        CHECK(j && jpeg_decode_rgb(j, n, 4000, &d, why, sizeof why) == 0, "face crop failed");
        if (j) {
            long sx = 0, sy = 0, cnt = 0;
            for (int y = 0; y < d.h; y++) for (int x = 0; x < d.w; x++) {
                const unsigned char *p = d.data + ((size_t)y * d.w + x) * 3;
                if (p[0] > 150 && p[1] < 80) { sx += x; sy += y; cnt++; }
            }
            float mx = cnt ? (float)sx / cnt / d.w : 0, my = cnt ? (float)sy / cnt / d.h : 0;
            CHECK(cnt && fabsf(mx - 0.5f) < 0.03f && fabsf(my - 0.5f) < 0.03f && d.w >= 120,
                  "face crop centred on the face: marker at %.2f,%.2f of a %dx%d crop", mx, my, d.w, d.h);
            free(d.data); free(j);
        }
        free(px);
    }
}

int main(void)
{
    test_align_recovers_known_transform();
    test_resize_identity_and_constant();
    test_nv12_gray();
    test_dot_and_match();
    test_gallery_file_roundtrip();
    test_yunet_decode_matches_reference();
    test_tensor_repack();
    test_embed_meta();
    test_gallery_param();
    test_gallery_v2_file();
    test_sysinfo_parsers();
    test_capacity();
    test_jpeg_and_letterbox();
    test_tracker();
    test_access();
    test_linger_one_event();
    test_photo_swap(0);
    test_photo_swap(1);
    test_face_geometry();
    test_snapshots();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all tests passed\n");
    return 0;
}
