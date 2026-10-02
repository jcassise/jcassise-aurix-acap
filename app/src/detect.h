/* AURIX - face detection stage. */
#ifndef AURIX_DETECT_H
#define AURIX_DETECT_H
#include "align.h"
#include "infer.h"

typedef struct {
    float x0, y0, x1, y1;   /* box in frame coords */
    float score;
    aurix_landmarks lm;     /* 5 points in frame coords */
} aurix_face;

/* Resize frame into the model input, run, decode. Returns face count (<= max) or -1. */
int detect_faces(aurix_model *m, const aurix_image *frame, aurix_face *out, int max, float threshold);

#endif
