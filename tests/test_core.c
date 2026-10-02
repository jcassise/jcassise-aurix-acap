#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "align.h"
#include "image.h"
#include "match.h"
#include "yunet.h"

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

int main(void)
{
    test_align_recovers_known_transform();
    test_resize_identity_and_constant();
    test_nv12_gray();
    test_dot_and_match();
    test_gallery_file_roundtrip();
    test_yunet_decode_matches_reference();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all tests passed\n");
    return 0;
}
