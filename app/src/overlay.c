#include "overlay.h"
#include <axoverlay.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <string.h>
#include <syslog.h>

static GMutex lock;
static overlay_box boxes[OVERLAY_MAX_BOXES];
static int nboxes;
static gboolean redraw_pending;
static gint overlay_id = -1;
static gboolean ready;

static void color_for(overlay_state s, double *r, double *g, double *b)
{
    switch (s) {
    case OV_ALLOW:  *r = 0.10; *g = 0.85; *b = 0.20; break;   /* green */
    case OV_THREAT: *r = 0.95; *g = 0.10; *b = 0.10; break;   /* red */
    case OV_CONCERN:*r = 1.00; *g = 0.65; *b = 0.00; break;   /* amber */
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

    /* clear */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    overlay_box local[OVERLAY_MAX_BOXES];
    int n;
    g_mutex_lock(&lock);
    n = nboxes;
    memcpy(local, boxes, sizeof(overlay_box) * (size_t)n);
    redraw_pending = FALSE;
    g_mutex_unlock(&lock);

    const double line = h / 270.0 > 2.0 ? h / 270.0 : 2.0;     /* ~4 px at 1080p */
    const double font = h / 40.0 > 12.0 ? h / 40.0 : 12.0;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font);

    for (int i = 0; i < n; i++) {
        const overlay_box *bx = &local[i];
        double x = bx->x0 * w, y = bx->y0 * h, bw = (bx->x1 - bx->x0) * w, bh = (bx->y1 - bx->y0) * h;
        double r, g, b;
        color_for(bx->state, &r, &g, &b);
        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_set_line_width(cr, bx->state == OV_PENDING ? line * 0.5 : line);
        cairo_rectangle(cr, x, y, bw, bh);
        cairo_stroke(cr);

        if (bx->label[0]) {
            cairo_text_extents_t te;
            cairo_text_extents(cr, bx->label, &te);
            double ty = y - line > font * 1.3 ? y - line : y + bh + font * 1.3;
            cairo_set_source_rgba(cr, r * 0.6, g * 0.6, b * 0.6, 0.75);
            cairo_rectangle(cr, x, ty - font * 1.1, te.x_advance + font * 0.6, font * 1.35);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_move_to(cr, x + font * 0.3, ty - font * 0.1);
            cairo_show_text(cr, bx->label);
        }
    }
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

static gboolean redraw_idle(gpointer unused)
{
    (void)unused;
    GError *err = NULL;
    axoverlay_redraw(&err);
    if (err) {
        syslog(LOG_WARNING, "overlay redraw: %s", err->message);
        g_error_free(err);
    }
    return G_SOURCE_REMOVE;
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
    int changed = n != nboxes || memcmp(boxes, b, sizeof(overlay_box) * (size_t)n);
    memcpy(boxes, b, sizeof(overlay_box) * (size_t)n);
    nboxes = n;
    gboolean schedule = changed && !redraw_pending;
    if (schedule) redraw_pending = TRUE;
    g_mutex_unlock(&lock);
    if (schedule) g_idle_add(redraw_idle, NULL);   /* axoverlay is driven from the main loop */
}
