/* AURIX - YuNet output decoding (pure C, no larod; host-testable).
 * Model outputs are 12 raw NHWC int8 head maps (cls/obj/bbox/kps x strides 8/16/32)
 * whose order and quantisation are described by detect.meta (tools/convert_yunet.py). */
#ifndef AURIX_YUNET_H
#define AURIX_YUNET_H
#include <stdint.h>
#include "align.h"

typedef struct {
    float x0, y0, x1, y1;   /* box */
    float score;
    aurix_landmarks lm;     /* image-left eye, image-right eye, nose, mouth L, mouth R */
} aurix_face;

enum { YUNET_CLS = 0, YUNET_OBJ, YUNET_BBOX, YUNET_KPS, YUNET_ROLES };
#define YUNET_LEVELS 3      /* strides 8, 16, 32 */
#define YUNET_OUTPUTS (YUNET_ROLES * YUNET_LEVELS)

typedef struct { int index; float scale; int zero_point; } yunet_tensor;

typedef struct {
    int in_w, in_h;
    yunet_tensor t[YUNET_LEVELS][YUNET_ROLES];   /* [level][role] -> model output */
} yunet_meta;

int yunet_meta_load(const char *path, yunet_meta *meta);

/* outputs[i] = model output i (as ordered in the .tflite). Coordinates are in model-input
 * pixels. NMS (IoU > nms_iou suppressed) applied. Returns number of faces (<= max). */
int yunet_decode(const yunet_meta *meta, const int8_t *const *outputs,
                 aurix_face *faces, int max, float score_thr, float nms_iou);

#endif
