#include "detect.h"
#include "infer.h"
#include <stdlib.h>
#include <syslog.h>

#define NMS_IOU 0.3f

static const int CHANNELS[YUNET_ROLES] = { 1, 1, 4, 10 };

struct aurix_detector {
    aurix_model *model;
    yunet_meta meta;
    const int8_t *outputs[YUNET_OUTPUTS];
};

aurix_detector *detector_open(const char *model_path, const char *meta_path, const char *device)
{
    aurix_detector *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (yunet_meta_load(meta_path, &d->meta)) {
        syslog(LOG_ERR, "detector: cannot read meta %s", meta_path);
        goto fail;
    }
    d->model = model_load(model_path, device);
    if (!d->model) goto fail;

    int h, w, c;
    if (model_input_dims(d->model, 0, &h, &w, &c) || w != d->meta.in_w || h != d->meta.in_h || c != 3 ||
        model_input_type(d->model, 0) != AURIX_DT_UINT8) {
        syslog(LOG_ERR, "detector: model input does not match meta (%dx%d uint8 RGB expected)",
               d->meta.in_w, d->meta.in_h);
        goto fail;
    }
    if (model_num_outputs(d->model) != YUNET_OUTPUTS) {
        syslog(LOG_ERR, "detector: expected %d outputs, got %zu", YUNET_OUTPUTS, model_num_outputs(d->model));
        goto fail;
    }
    /* Check every output is int8 and big enough for its grid, then cache pointers. */
    for (int l = 0; l < YUNET_LEVELS; l++) {
        int st = 8 << l;
        size_t cells = (size_t)(d->meta.in_w / st) * (d->meta.in_h / st);
        for (int r = 0; r < YUNET_ROLES; r++) {
            int idx = d->meta.t[l][r].index;
            size_t bytes = 0;
            const void *p = model_output(d->model, (size_t)idx, &bytes);
            if (!p || model_output_type(d->model, (size_t)idx) != AURIX_DT_INT8 ||
                bytes < cells * CHANNELS[r]) {
                syslog(LOG_ERR, "detector: output %d has wrong type/size", idx);
                goto fail;
            }
            d->outputs[idx] = p;
        }
    }
    syslog(LOG_INFO, "detector: YuNet %dx%d ready", d->meta.in_w, d->meta.in_h);
    return d;
fail:
    detector_close(d);
    return NULL;
}

void detector_close(aurix_detector *d)
{
    if (!d) return;
    model_free(d->model);
    free(d);
}

int detect_faces(aurix_detector *d, const aurix_image *frame, aurix_face *out, int max, float threshold)
{
    const int w = d->meta.in_w, h = d->meta.in_h;
    size_t bytes = 0;
    uint8_t *in = model_input(d->model, 0, &bytes);
    if (!in || bytes < (size_t)w * h * 3) return -1;

    /* Full-frame stretch to the model size (1080p -> 640x352 is ~3x). With the 40 px
     * inter-eye gate at 1080p, gated faces are ~30 px in model space - well inside
     * YuNet's range. Tiling comes later for higher capture resolutions. */
    aurix_image dst = { in, w, h, w * 3, 3 };
    resize_bilinear(frame, &dst);

    if (model_run(d->model)) return -1;
    int n = yunet_decode(&d->meta, d->outputs, out, max, threshold, NMS_IOU);

    const float sx = (float)frame->w / w, sy = (float)frame->h / h;
    for (int i = 0; i < n; i++) {
        out[i].x0 *= sx; out[i].x1 *= sx;
        out[i].y0 *= sy; out[i].y1 *= sy;
        for (int k = 0; k < 5; k++) { out[i].lm.x[k] *= sx; out[i].lm.y[k] *= sy; }
    }
    return n;
}
