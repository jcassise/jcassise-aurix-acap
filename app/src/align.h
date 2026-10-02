/* AURIX - 5-point similarity alignment to the standard 112x112 ArcFace template. */
#ifndef AURIX_ALIGN_H
#define AURIX_ALIGN_H
#include "image.h"

/* Order: left eye, right eye, nose tip, left mouth corner, right mouth corner (image coords). */
typedef struct { float x[5], y[5]; } aurix_landmarks;

/* Least-squares similarity (scale+rotation+translation, no reflection) mapping
 * template coords (scaled to out_size) -> frame coords. Result in warp_affine_bilinear form. */
int align_similarity(const aurix_landmarks *lm, int out_size, float m[6]);

/* Warp the face into out (out->w == out->h == out_size, typically 112). */
int align_face(const aurix_image *frame, const aurix_landmarks *lm, aurix_image *out);

float landmarks_eye_distance(const aurix_landmarks *lm);

/* Template points for a 112x112 crop. */
extern const float AURIX_TEMPLATE_112[5][2];

#endif
