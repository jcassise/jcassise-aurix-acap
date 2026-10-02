/* AURIX - face detection stage (YuNet on larod). */
#ifndef AURIX_DETECT_H
#define AURIX_DETECT_H
#include "image.h"
#include "yunet.h"

typedef struct aurix_detector aurix_detector;

/* model: detect.tflite, meta: detect.meta (both from tools/convert_yunet.py). */
aurix_detector *detector_open(const char *model_path, const char *meta_path, const char *device);
void detector_close(aurix_detector *d);

/* Resize frame into the model input, run, decode. Coordinates returned in FRAME pixels.
 * Returns face count (<= max) or -1 on error. */
int detect_faces(aurix_detector *d, const aurix_image *frame, aurix_face *out, int max, float threshold);

#endif
