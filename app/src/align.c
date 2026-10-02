#include "align.h"
#include <math.h>

const float AURIX_TEMPLATE_112[5][2] = {
    { 38.2946f, 51.6963f }, { 73.5318f, 51.5014f }, { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f }, { 70.7299f, 92.2041f },
};

int align_similarity(const aurix_landmarks *lm, int out_size, float m[6])
{
    const float s = out_size / 112.0f;
    float pu[5], pv[5], pcu = 0, pcv = 0, qcx = 0, qcy = 0;
    for (int i = 0; i < 5; i++) {
        pu[i] = AURIX_TEMPLATE_112[i][0] * s;
        pv[i] = AURIX_TEMPLATE_112[i][1] * s;
        pcu += pu[i]; pcv += pv[i]; qcx += lm->x[i]; qcy += lm->y[i];
    }
    pcu /= 5; pcv /= 5; qcx /= 5; qcy /= 5;

    float num_a = 0, num_b = 0, den = 0;
    for (int i = 0; i < 5; i++) {
        float u = pu[i] - pcu, v = pv[i] - pcv;
        float x = lm->x[i] - qcx, y = lm->y[i] - qcy;
        num_a += u * x + v * y;
        num_b += u * y - v * x;
        den += u * u + v * v;
    }
    if (den < 1e-6f) return -1;
    float a = num_a / den, b = num_b / den;
    m[0] = a;  m[1] = -b; m[2] = qcx - (a * pcu - b * pcv);
    m[3] = b;  m[4] = a;  m[5] = qcy - (b * pcu + a * pcv);
    return 0;
}

int align_face(const aurix_image *frame, const aurix_landmarks *lm, aurix_image *out)
{
    float m[6];
    if (out->w != out->h || align_similarity(lm, out->w, m)) return -1;
    warp_affine_bilinear(frame, out, m);
    return 0;
}

float landmarks_eye_distance(const aurix_landmarks *lm)
{
    return hypotf(lm->x[1] - lm->x[0], lm->y[1] - lm->y[0]);
}
