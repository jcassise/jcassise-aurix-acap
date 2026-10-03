/* Renders the real overlay code (included) at 3840x2160 with a stand-in axoverlay. */
#include "../../app/src/overlay.c"
#include <stdio.h>
#include <stdlib.h>
static cairo_surface_t *surf;
/* stand-ins: axoverlay_redraw calls the render callback once on our surface */
void axoverlay_redraw(GError **e) { (void)e; cairo_t *cr = cairo_create(surf);
    render_cb(cr, overlay_id, NULL, 0, 0, 0, cairo_image_surface_get_width(surf), cairo_image_surface_get_height(surf), NULL);
    cairo_destroy(cr); }
gboolean axoverlay_is_backend_supported(enum axoverlay_backend_type b) { (void)b; return TRUE; }
void axoverlay_init_axoverlay_settings(struct axoverlay_settings *s) { memset(s, 0, sizeof *s); }
void axoverlay_init(struct axoverlay_settings *s, GError **e) { (void)s; (void)e; }
gint axoverlay_get_max_resolution_width(gint c, GError **e) { (void)c; (void)e; return 3840; }
gint axoverlay_get_max_resolution_height(gint c, GError **e) { (void)c; (void)e; return 2160; }
void axoverlay_init_overlay_data(struct axoverlay_overlay_data *d) { memset(d, 0, sizeof *d); }
gint axoverlay_create_overlay(struct axoverlay_overlay_data *d, gpointer u, GError **e) { (void)d; (void)u; (void)e; return 1; }
void axoverlay_destroy_overlay(gint i, GError **e) { (void)i; (void)e; }
void axoverlay_cleanup(void) {}

static double now_ms(void) { return g_get_monotonic_time() / 1000.0; }

int main(void)
{
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 3840, 2160);
    overlay_init();
    overlay_box b[2] = { 0 };
    b[0] = (overlay_box){ .x0 = 0.20f, .y0 = 0.30f, .x1 = 0.28f, .y1 = 0.45f, .state = OV_ALLOW, .confidence = 1.0f, .label = "Dana Ruiz" };
    b[1] = (overlay_box){ .x0 = 0.70f, .y0 = 0.32f, .x1 = 0.78f, .y1 = 0.47f, .state = OV_THREAT, .confidence = 1.0f, .label = "THREAT: Second Person" };
    memcpy(boxes, b, sizeof b); nboxes = 2;
    last_redraw_us = 0;
    redraw_idle(NULL);
    cairo_surface_flush(surf);
    /* stray line check: the space between the two faces (y between their centres) must be empty */
    unsigned char *px = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf), painted = 0;
    for (int x = (int)(0.36 * 3840); x < (int)(0.62 * 3840); x++)
        for (int y = (int)(0.30 * 2160); y < (int)(0.48 * 2160); y++)
            painted += px[y * stride + x * 4 + 3] > 0;
    printf("%s no line between the faces (%d painted pixels in the gap)\n", painted == 0 ? "PASS" : "FAIL", painted);
    cairo_surface_write_to_png(surf, "overlay_2faces.png");

    /* cost: 50 redraws with faces moving */
    double t0 = now_ms();
    for (int k = 0; k < 50; k++) {
        b[0].x0 += 0.002f; b[0].x1 += 0.002f;
        memcpy(boxes, b, sizeof b); nboxes = 2; last_redraw_us = 0;
        redraw_idle(NULL);
    }
    double fast = (now_ms() - t0) / 50;
    /* old behaviour for comparison: full clear every time (system-initiated path) */
    t0 = now_ms();
    for (int k = 0; k < 50; k++) {
        cairo_t *cr = cairo_create(surf);
        render_cb(cr, overlay_id, NULL, 0, 0, 0, 3840, 2160, NULL);
        cairo_destroy(cr);
    }
    double full = (now_ms() - t0) / 50;
    printf("INFO redraw at 3840x2160: %.2f ms clearing only around faces vs %.2f ms clearing everything (%.1fx)\n", fast, full, full / fast);
    /* partial clear correctness: after a face moves away, its old spot must be empty */
    b[0] = (overlay_box){ .x0 = 0.05f, .y0 = 0.60f, .x1 = 0.13f, .y1 = 0.75f, .state = OV_ALLOW, .confidence = 1.0f, .label = "Dana Ruiz" };
    memcpy(boxes, b, sizeof b); nboxes = 2; last_redraw_us = 0; redraw_idle(NULL);
    memcpy(boxes, b, sizeof b); nboxes = 2; last_redraw_us = 0; redraw_idle(NULL);
    cairo_surface_flush(surf);
    int ghost = 0;
    for (int x = (int)(0.30 * 3840); x < (int)(0.34 * 3840); x++)
        for (int y = (int)(0.25 * 2160); y < (int)(0.50 * 2160); y++) ghost += px[y * stride + x * 4 + 3] > 0;
    printf("%s no trail left where a face used to be (%d pixels)\n", ghost == 0 ? "PASS" : "FAIL", ghost);
    /* jitter below 0.5%% does not trigger a redraw */
    memcpy(shown, b, sizeof b); nshown = 2;
    overlay_box j[2]; memcpy(j, b, sizeof b); j[0].x0 += 0.001f;
    printf("%s sub-pixel jitter does not redraw\n", visibly_different(shown, 2, j, 2) == 0 ? "PASS" : "FAIL");
    j[0].label[0] = 'X';
    printf("%s a new label does redraw\n", visibly_different(shown, 2, j, 2) == 1 ? "PASS" : "FAIL");

    /* where is the drawn ellipse? centre of its painted pixels in a window */
    #define CENTRE_Y(S, X0, X1) ({ unsigned char *d_ = cairo_image_surface_get_data(S); int st_ = cairo_image_surface_get_stride(S), \
        H_ = cairo_image_surface_get_height(S), lo_ = H_, hi_ = -1; for (int y_ = 0; y_ < H_; y_++) for (int x_ = (X0); x_ < (X1); x_++) \
        if (d_[y_ * st_ + x_ * 4 + 3] > 200) { if (y_ < lo_) lo_ = y_; if (y_ > hi_) hi_ = y_; } (lo_ + hi_) / 2.0 / H_; })

    /* a 4:3 sensor (P3267, 2592x1944) seen through a 16:9 stream: the stream is a centred crop */
    cap_w = 2592; cap_h = 1944;
    cairo_surface_destroy(surf);
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 1080);
    overlay_box f = { .state = OV_ALLOW, .confidence = 1.0f, .cx = 0.5f, .cy = 0.30f, .rx = 0.04f, .ry = 0.06f };
    memcpy(boxes, &f, sizeof f); nboxes = 1; last_redraw_us = 0; prev_area[0].empty = prev_area[1].empty = 1;
    cairo_t *c0 = cairo_create(surf); cairo_set_operator(c0, CAIRO_OPERATOR_CLEAR); cairo_paint(c0); cairo_destroy(c0);
    redraw_idle(NULL);
    cairo_surface_flush(surf);
    double y169 = CENTRE_Y(surf, 900, 1020), want = (0.30 - 0.125) / 0.75;
    printf("%s 4:3 sensor on a 16:9 stream: face at %.3f of the stream (expected %.3f)\n", fabs(y169 - want) < 0.01 ? "PASS" : "FAIL", y169, want);
    /* the same face on a full 4:3 stream sits where it is on the sensor */
    cairo_surface_destroy(surf);
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1440, 1080);
    memcpy(boxes, &f, sizeof f); nboxes = 1; last_redraw_us = 0; prev_area[0].empty = prev_area[1].empty = 1;
    redraw_idle(NULL);
    cairo_surface_flush(surf);
    double y43 = CENTRE_Y(surf, 660, 780);
    printf("%s full 4:3 stream: face at %.3f (expected 0.300)\n", fabs(y43 - 0.30) < 0.01 ? "PASS" : "FAIL", y43);
    /* tuning: +0.02 offset moves it down by 0.02 of the picture */
    overlay_set_tuning(0.02f, 1.0f, 0);
    cairo_t *c1 = cairo_create(surf); cairo_set_operator(c1, CAIRO_OPERATOR_CLEAR); cairo_paint(c1); cairo_destroy(c1);
    memcpy(boxes, &f, sizeof f); nboxes = 1; last_redraw_us = 0; redraw_idle(NULL);
    cairo_surface_flush(surf);
    double yt = CENTRE_Y(surf, 660, 780);
    printf("%s vertical tuning moves the ellipse (%.3f -> %.3f)\n", fabs(yt - 0.32) < 0.01 ? "PASS" : "FAIL", y43, yt);
    overlay_set_tuning(0, 1.0f, 0);
    /* no trail on a cropped stream either */
    cairo_surface_destroy(surf);
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 1080);
    c1 = cairo_create(surf); cairo_set_operator(c1, CAIRO_OPERATOR_CLEAR); cairo_paint(c1); cairo_destroy(c1);
    f.cx = 0.3f; memcpy(boxes, &f, sizeof f); nboxes = 1; last_redraw_us = 0; redraw_idle(NULL);
    f.cx = 0.7f; for (int k = 0; k < 3; k++) { memcpy(boxes, &f, sizeof f); nboxes = 1; last_redraw_us = 0; redraw_idle(NULL); }
    cairo_surface_flush(surf);
    unsigned char *d2 = cairo_image_surface_get_data(surf); int st2 = cairo_image_surface_get_stride(surf), trail = 0;
    for (int y = 0; y < 1080; y++) for (int x = 400; x < 760; x++) trail += d2[y * st2 + x * 4 + 3] > 0;
    printf("%s no trail on a cropped stream (%d pixels)\n", trail == 0 ? "PASS" : "FAIL", trail);
    return 0;
}
