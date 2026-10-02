#include "detect.h"
#include <syslog.h>

/* Decode raw model outputs into faces, in MODEL INPUT pixel coordinates.
 *
 * TODO: implement once the detector is fixed. This depends entirely on the output head:
 *  - YuNet (OpenCV zoo): 3 strides (8/16/32), per-anchor cls/obj/bbox/kps tensors, then NMS.
 *  - BlazeFace: fixed SSD anchor set (896 for front model), 16 regressors per anchor
 *    (box + 6 keypoints; map to our 5-point order), sigmoid score, weighted NMS.
 * Also handle output dequantisation (int8/uint8 with scale/zero-point) for the DLPU build. */
static int decode_outputs(aurix_model *m, int in_w, int in_h, aurix_face *out, int max, float thr)
{
    (void)m; (void)in_w; (void)in_h; (void)out; (void)max; (void)thr;
    return 0;
}

int detect_faces(aurix_model *m, const aurix_image *frame, aurix_face *out, int max, float threshold)
{
    int h, w, c;
    if (model_input_dims(m, 0, &h, &w, &c) || c != 3) {
        syslog(LOG_ERR, "detector: expected NHWC RGB input");
        return -1;
    }
    aurix_dtype dt = model_input_type(m, 0);
    if (dt != AURIX_DT_UINT8) {
        /* int8 / float inputs need a conversion pass here; uint8 is the common QAT export. */
        syslog(LOG_ERR, "detector: input dtype %d not supported yet", (int)dt);
        return -1;
    }
    size_t bytes = 0;
    uint8_t *in = model_input(m, 0, &bytes);
    if (!in || bytes < (size_t)w * h * 3) return -1;

    /* Watchlist-at-distance will need tiling or a second high-res pass here instead of a
     * single full-frame downscale, to keep small faces above the inter-eye threshold. */
    aurix_image dst = { in, w, h, w * 3, 3 };
    resize_bilinear(frame, &dst);

    if (model_run(m)) return -1;
    int n = decode_outputs(m, w, h, out, max, threshold);

    const float sx = (float)frame->w / w, sy = (float)frame->h / h;
    for (int i = 0; i < n; i++) {
        out[i].x0 *= sx; out[i].x1 *= sx;
        out[i].y0 *= sy; out[i].y1 *= sy;
        for (int k = 0; k < 5; k++) { out[i].lm.x[k] *= sx; out[i].lm.y[k] *= sy; }
    }
    return n;
}
