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
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all tests passed\n");
    return 0;
}
