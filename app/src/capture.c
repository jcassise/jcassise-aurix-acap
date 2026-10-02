#include "capture.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <vdo-buffer.h>
#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

struct aurix_capture {
    VdoStream *stream;
    unsigned w, h, pitch;
    gboolean native_rgb;   /* TRUE: VDO gives RGB; FALSE: NV12 converted on CPU */
    uint8_t *rgb;          /* conversion / packing buffer */
};

static VdoStream *open_stream(unsigned w, unsigned h, double fps, gboolean rgb, GError **err)
{
    VdoMap *s = vdo_map_new();
    vdo_map_set_uint32(s, "format", rgb ? VDO_FORMAT_RGB : VDO_FORMAT_YUV);
    if (!rgb) vdo_map_set_string(s, "subformat", "NV12");
    vdo_map_set_uint32(s, "width", w);
    vdo_map_set_uint32(s, "height", h);
    vdo_map_set_double(s, "framerate", fps);
    VdoStream *st = vdo_stream_new(s, NULL, err);
    g_object_unref(s);
    return st;
}

aurix_capture *capture_open(unsigned width, unsigned height, double fps)
{
    aurix_capture *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    GError *err = NULL;

    /* RGB straight from VDO where the chip supports it (ARTPEC-8/9); NV12 fallback (ARTPEC-7). */
    c->native_rgb = TRUE;
    c->stream = open_stream(width, height, fps, TRUE, &err);
    if (!c->stream) {
        syslog(LOG_INFO, "VDO RGB unavailable (%s), falling back to NV12", err ? err->message : "?");
        g_clear_error(&err);
        c->native_rgb = FALSE;
        c->stream = open_stream(width, height, fps, FALSE, &err);
    }
    if (!c->stream) goto fail;

    VdoMap *info = vdo_stream_get_info(c->stream, &err);
    if (!info) goto fail;
    c->w = vdo_map_get_uint32(info, "width", width);
    c->h = vdo_map_get_uint32(info, "height", height);
    c->pitch = vdo_map_get_uint32(info, "pitch", c->native_rgb ? c->w * 3 : c->w);
    g_object_unref(info);

    c->rgb = malloc((size_t)c->w * c->h * 3);
    if (!c->rgb) goto fail;
    if (!vdo_stream_start(c->stream, &err)) goto fail;

    syslog(LOG_INFO, "VDO %ux%u pitch %u %s @ %.1f fps", c->w, c->h, c->pitch,
           c->native_rgb ? "RGB" : "NV12", fps);
    return c;
fail:
    syslog(LOG_ERR, "VDO open failed: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    capture_close(c);
    return NULL;
}

int capture_next(aurix_capture *c, aurix_image *out)
{
    GError *err = NULL;
    VdoBuffer *buf = vdo_stream_get_buffer(c->stream, &err);
    if (!buf) {
        syslog(LOG_WARNING, "VDO get_buffer: %s", err ? err->message : "unknown");
        g_clear_error(&err);
        return -1;
    }
    const uint8_t *data = vdo_buffer_get_data(buf);
    if (c->native_rgb) {
        for (unsigned j = 0; j < c->h; j++)
            memcpy(c->rgb + (size_t)j * c->w * 3, data + (size_t)j * c->pitch, (size_t)c->w * 3);
    } else {
        /* Assumes UV plane follows Y at pitch*height; verify on real buffers. */
        nv12_to_rgb(data, data + (size_t)c->pitch * c->h, (int)c->w, (int)c->h, (int)c->pitch, c->rgb);
    }
    vdo_stream_buffer_unref(c->stream, &buf, NULL);

    out->data = c->rgb;
    out->w = (int)c->w;
    out->h = (int)c->h;
    out->stride = (int)c->w * 3;
    out->ch = 3;
    return 0;
}

void capture_close(aurix_capture *c)
{
    if (!c) return;
    if (c->stream) {
        vdo_stream_stop(c->stream);
        g_object_unref(c->stream);
    }
    free(c->rgb);
    free(c);
}
