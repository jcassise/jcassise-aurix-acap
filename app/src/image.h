/* AURIX - minimal image helpers (interleaved 8-bit, CPU). */
#ifndef AURIX_IMAGE_H
#define AURIX_IMAGE_H
#include <stdint.h>

typedef struct {
    uint8_t *data;
    int w, h;
    int stride;   /* bytes per row */
    int ch;       /* channels, 3 = RGB */
} aurix_image;

/* NV12 (Y plane + interleaved UV plane, both with `stride`) -> packed RGB888 (stride w*3). BT.601 limited range. */
void nv12_to_rgb(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, uint8_t *rgb);

/* Bilinear warp. m maps DESTINATION pixel (u,v) to SOURCE (x,y):
 *   x = m[0]*u + m[1]*v + m[2];  y = m[3]*u + m[4]*v + m[5]
 * Pixels sampling outside the source are set to 0. src->ch must equal dst->ch. */
void warp_affine_bilinear(const aurix_image *src, aurix_image *dst, const float m[6]);

/* Bilinear resize of src into dst (pixel-centre aligned). */
void resize_bilinear(const aurix_image *src, aurix_image *dst);

/* Letterbox into a canvas of the given aspect ratio (grey padding) so portraits are not stretched.
 * Returns 0 with a malloc'd canvas and the original's offset inside it. */
int image_letterbox(const aurix_image *src, double aspect, aurix_image *canvas, int *off_x, int *off_y);

#endif
