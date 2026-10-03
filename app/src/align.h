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

/* Face outline from the landmarks: centre between eyes and mouth (nudged toward the forehead),
 * height from the eye-mouth distance, width from the eye distance. Frame pixels. */
void landmarks_face_ellipse(const aurix_landmarks *lm, float *cx, float *cy, float *rx, float *ry);

/* Rough head pose from five landmarks (corrected for roll): yaw > 0 = turned toward image right,
 * pitch > 0 = looking down. 0/0 for a frontal face; good to ~10 degrees, enough to gate identification. */
void landmarks_pose(const aurix_landmarks *lm, float *yaw_deg, float *pitch_deg);

#endif
