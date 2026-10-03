#include "overlay.h"
#include <axoverlay.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <syslog.h>

static GMutex lock;
static overlay_box boxes[OVERLAY_MAX_BOXES];       /* latest from the video thread */
static int nboxes;
static overlay_box shown[OVERLAY_MAX_BOXES];       /* what the last redraw drew */
static int nshown;
static gboolean redraw_pending;
static gint overlay_id = -1;
static gboolean ready;

/* Main-loop only: what one redraw paints, and the screen area it must clear. Clearing only around
 * faces (now and in the last two redraws, in case the surface is double-buffered) instead of the
 * whole 4K surface is most of the saving. */
typedef struct { double x0, y0, x1, y1; int empty; } nrect;
static overlay_box frame_boxes[OVERLAY_MAX_BOXES];
static int frame_n;
static nrect clear_area, prev_area[2];
static gboolean in_redraw;
static gint64 last_redraw_us;
static double render_ms_acc;
static unsigned render_count;

static nrect area_of(const overlay_box *b, int n)
{
    nrect r = { 1, 1, 0, 0, 1 };
    for (int i = 0; i < n; i++) {
        double w = b[i].x1 - b[i].x0, h = b[i].y1 - b[i].y0;
        /* ellipse + glow + label pill above/below */
        double x0 = b[i].x0 - w * 0.35 - 0.12, x1 = b[i].x1 + w * 0.35 + 0.12;
        double y0 = b[i].y0 - h * 0.35 - 0.09, y1 = b[i].y1 + h * 0.35 + 0.09;
        if (r.empty) { r = (nrect){ x0, y0, x1, y1, 0 }; continue; }
        if (x0 < r.x0) r.x0 = x0;
        if (y0 < r.y0) r.y0 = y0;
        if (x1 > r.x1) r.x1 = x1;
        if (y1 > r.y1) r.y1 = y1;
    }
    return r;
}

static void grow(nrect *a, const nrect *b)
{
    if (b->empty) return;
    if (a->empty) { *a = *b; return; }
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

void overlay_stats(double *avg_render_ms, unsigned *renders)
{
    *avg_render_ms = render_count ? render_ms_acc / render_count : 0;
    *renders = render_count;
    render_ms_acc = 0;
    render_count = 0;
}

static void color_for(overlay_state s, double *r, double *g, double *b)
{
    switch (s) {
    case OV_ALLOW:  *r = 0.10; *g = 0.85; *b = 0.20; break;   /* green */
    case OV_THREAT: *r = 0.95; *g = 0.10; *b = 0.10; break;   /* red */
    case OV_CONCERN:*r = 1.00; *g = 0.65; *b = 0.00; break;   /* amber */
    case OV_DENIED: *r = 0.85; *g = 0.20; *b = 0.85; break;   /* magenta */
    case OV_ALERT:  *r = 1.00; *g = 0.35; *b = 0.10; break;   /* orange-red */
    case OV_UNKNOWN:*r = 0.15; *g = 0.45; *b = 1.00; break;   /* blue */
    default:        *r = 0.70; *g = 0.70; *b = 0.70; break;   /* grey */
    }
}

static void render_cb(gpointer ctx_ptr, gint id, struct axoverlay_stream_data *stream,
                      enum axoverlay_position_type postype, gfloat ox, gfloat oy,
                      gint w, gint h, gpointer user_data)
{
    (void)stream; (void)postype; (void)ox; (void)oy; (void)user_data;
    if (id != overlay_id) return;
    cairo_t *cr = ctx_ptr;
    gint64 t0 = g_get_monotonic_time();

    /* clear: only around faces when this is one of our redraws; everything otherwise (a stream
     * that just started, or a redraw the system asked for) */
    cairo_save(cr);
    if (in_redraw && !clear_area.empty) {
        double x0 = fmax(0, clear_area.x0) * w, y0 = fmax(0, clear_area.y0) * h;
        double x1 = fmin(1, clear_area.x1) * w, y1 = fmin(1, clear_area.y1) * h;
        cairo_rectangle(cr, floor(x0), floor(y0), ceil(x1 - x0) + 1, ceil(y1 - y0) + 1);
        cairo_clip(cr);
    }
    if (!in_redraw || !clear_area.empty) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
    }
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    overlay_box local[OVERLAY_MAX_BOXES];
    int n;
    if (in_redraw) {
        n = frame_n;
        memcpy(local, frame_boxes, sizeof(overlay_box) * (size_t)n);
    } else {
        g_mutex_lock(&lock);
        n = nshown;
        memcpy(local, shown, sizeof(overlay_box) * (size_t)n);
        g_mutex_unlock(&lock);
    }

    const double line = h / 160.0 > 3.0 ? h / 160.0 : 3.0;   /* ~7 px at 1080p: easy to follow */
    const double font = h / 42.0 > 13.0 ? h / 42.0 : 13.0;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font);

    for (int i = 0; i < n; i++) {
        const overlay_box *bx = &local[i];
        double cx = (bx->x0 + bx->x1) / 2 * w, cy = (bx->y0 + bx->y1) / 2 * h;
        double rx = (bx->x1 - bx->x0) / 2 * w * 1.08, ry = (bx->y1 - bx->y0) / 2 * h * 1.15;   /* faces are taller */
        if (rx < 4 || ry < 4) continue;
        double r, g, b;
        color_for(bx->state, &r, &g, &b);
        double conf = bx->confidence < 0.3 ? 0.3 : bx->confidence > 1 ? 1 : bx->confidence;
        double lw = bx->state == OV_PENDING ? line * 0.5 : line * (0.6 + 0.4 * conf);

        /* ellipse path in a scaled frame, stroked in device space so the line width stays even.
         * new_path: the label's text leaves a current point that would otherwise draw a line here */
        cairo_new_path(cr);
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
        cairo_restore(cr);
        if (bx->state != OV_PENDING) {               /* soft glow under the line: one wide stroke, fast edges */
            cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
            cairo_set_source_rgba(cr, r, g, b, 0.30 * conf);
            cairo_set_line_width(cr, lw * 2.6);
            cairo_stroke_preserve(cr);
            cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
        }
        cairo_set_source_rgba(cr, fmin(1, r * 1.1 + 0.08), fmin(1, g * 1.1 + 0.08), fmin(1, b * 1.1 + 0.08),
                              bx->state == OV_PENDING ? 0.7 : 0.55 + 0.45 * conf);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);

        if (bx->label[0]) {                           /* rounded name pill above the head */
            cairo_text_extents_t te;
            cairo_text_extents(cr, bx->label, &te);
            double pw = te.x_advance + font * 1.0, ph = font * 1.5, pr = ph / 2;
            double px = cx - pw / 2, py = cy - ry - ph - line * 1.5;
            if (py < 2) py = cy + ry + line * 1.5;    /* no room above: put it below the chin */
            if (px < 2) px = 2;
            if (px + pw > w - 2) px = w - 2 - pw;
            cairo_new_sub_path(cr);
            cairo_arc(cr, px + pw - pr, py + pr, pr, -G_PI / 2, G_PI / 2);
            cairo_arc(cr, px + pr, py + pr, pr, G_PI / 2, 3 * G_PI / 2);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, r * 0.55, g * 0.55, b * 0.55, 0.85);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_move_to(cr, px + font * 0.5, py + ph / 2 + te.height / 2 - 1);
            cairo_show_text(cr, bx->label);
            cairo_new_path(cr);
        }
    }
    render_ms_acc += (g_get_monotonic_time() - t0) / 1000.0;
    render_count++;
}

static void adjust_cb(gint id, struct axoverlay_stream_data *stream, enum axoverlay_position_type *postype,
                      gfloat *ox, gfloat *oy, gint *w, gint *h, gpointer user_data)
{
    (void)id; (void)postype; (void)ox; (void)oy; (void)user_data;
    *w = stream->width;
    *h = stream->height;
    if (stream->rotation == 90 || stream->rotation == 270) {
        *w = stream->height;
        *h = stream->width;
    }
}

#define OVERLAY_MIN_INTERVAL_US 120000      /* at most ~8 redraws a second */

static gboolean redraw_idle(gpointer unused)
{
    (void)unused;
    gint64 now = g_get_monotonic_time();
    if (now - last_redraw_us < OVERLAY_MIN_INTERVAL_US) {   /* too soon: try again when allowed */
        g_timeout_add((guint)((OVERLAY_MIN_INTERVAL_US - (now - last_redraw_us)) / 1000) + 1, redraw_idle, NULL);
        return G_SOURCE_REMOVE;
    }
    last_redraw_us = now;
    g_mutex_lock(&lock);
    frame_n = nboxes;
    memcpy(frame_boxes, boxes, sizeof(overlay_box) * (size_t)nboxes);
    memcpy(shown, boxes, sizeof(overlay_box) * (size_t)nboxes);
    nshown = nboxes;
    redraw_pending = FALSE;
    g_mutex_unlock(&lock);
    nrect cur = area_of(frame_boxes, frame_n);
    clear_area = cur;
    grow(&clear_area, &prev_area[0]);
    grow(&clear_area, &prev_area[1]);
    prev_area[1] = prev_area[0];
    prev_area[0] = cur;
    GError *err = NULL;
    in_redraw = TRUE;
    axoverlay_redraw(&err);
    in_redraw = FALSE;
    if (err) {
        syslog(LOG_WARNING, "overlay redraw: %s", err->message);
        g_error_free(err);
    }
    return G_SOURCE_REMOVE;
}

/* A redraw is worth it only if something visibly changed: a face moved more than ~0.5% of the
 * picture, or a colour, label or confidence changed. Sub-pixel smoothing jitter is ignored. */
static int visibly_different(const overlay_box *a, int na, const overlay_box *b, int nb)
{
    if (na != nb) return 1;
    for (int i = 0; i < na; i++) {
        if (a[i].state != b[i].state || strcmp(a[i].label, b[i].label) || fabsf(a[i].confidence - b[i].confidence) > 0.05f)
            return 1;
        if (fabsf(a[i].x0 - b[i].x0) > 0.005f || fabsf(a[i].x1 - b[i].x1) > 0.005f ||
            fabsf(a[i].y0 - b[i].y0) > 0.005f || fabsf(a[i].y1 - b[i].y1) > 0.005f)
            return 1;
    }
    return 0;
}

int overlay_init(void)
{
    GError *err = NULL;
    g_mutex_init(&lock);
    if (!axoverlay_is_backend_supported(AXOVERLAY_CAIRO_IMAGE_BACKEND)) {
        syslog(LOG_WARNING, "overlay: cairo backend not supported - no live-view boxes");
        return -1;
    }
    struct axoverlay_settings settings;
    axoverlay_init_axoverlay_settings(&settings);
    settings.render_callback = render_cb;
    settings.adjustment_callback = adjust_cb;
    settings.select_callback = NULL;
    settings.backend = AXOVERLAY_CAIRO_IMAGE_BACKEND;
    axoverlay_init(&settings, &err);
    if (err) goto fail;

    gint cw = axoverlay_get_max_resolution_width(1, &err);
    if (err) goto fail;
    gint ch = axoverlay_get_max_resolution_height(1, &err);
    if (err) goto fail;

    struct axoverlay_overlay_data data;
    axoverlay_init_overlay_data(&data);
    data.postype = AXOVERLAY_CUSTOM_NORMALIZED;
    data.anchor_point = AXOVERLAY_ANCHOR_CENTER;
    data.x = 0.0;
    data.y = 0.0;
    data.scale_to_stream = FALSE;
    data.width = cw;
    data.height = ch;
    data.colorspace = AXOVERLAY_COLORSPACE_ARGB32;
    overlay_id = axoverlay_create_overlay(&data, NULL, &err);
    if (err) goto fail;
    ready = TRUE;
    syslog(LOG_INFO, "overlay: ready (%dx%d)", cw, ch);
    return 0;
fail:
    syslog(LOG_WARNING, "overlay unavailable: %s", err ? err->message : "?");
    if (err) g_error_free(err);
    return -1;
}

void overlay_cleanup(void)
{
    if (!ready) return;
    GError *err = NULL;
    axoverlay_destroy_overlay(overlay_id, &err);
    if (err) g_error_free(err);
    axoverlay_cleanup();
    ready = FALSE;
}

void overlay_publish(const overlay_box *b, int n)
{
    if (!ready) return;
    if (n > OVERLAY_MAX_BOXES) n = OVERLAY_MAX_BOXES;
    g_mutex_lock(&lock);
    int changed = visibly_different(shown, nshown, b, n);
    memcpy(boxes, b, sizeof(overlay_box) * (size_t)n);
    nboxes = n;
    gboolean schedule = changed && !redraw_pending;
    if (schedule) redraw_pending = TRUE;
    g_mutex_unlock(&lock);
    if (schedule) g_idle_add(redraw_idle, NULL);   /* axoverlay is driven from the main loop */
}
