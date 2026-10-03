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
    b[0] = (overlay_box){ 0.20f, 0.30f, 0.28f, 0.45f, OV_ALLOW, 1.0f, "Dana Ruiz" };
    b[1] = (overlay_box){ 0.70f, 0.32f, 0.78f, 0.47f, OV_THREAT, 1.0f, "THREAT: Second Person" };
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
    b[0] = (overlay_box){ 0.05f, 0.60f, 0.13f, 0.75f, OV_ALLOW, 1.0f, "Dana Ruiz" };
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
    return 0;
}
