#include "snapshot.h"
#include "jpeg.h"
#include <math.h>
#include <stdlib.h>

/* Face crop for the event (>= 240 px short side, or native size if the face is smaller; never upscaled). */
unsigned char *snapshot_face(const aurix_image *f, const trk_track *t, size_t *len)
{
    /* centred on the face itself (landmarks), not the detector box, which runs down to the chin */
    float cx = t->has_face_geo ? t->fcx : (t->x0 + t->x1) / 2, cy = t->has_face_geo ? t->fcy : (t->y0 + t->y1) / 2;
    float side = fmaxf(t->x1 - t->x0, t->y1 - t->y0) * 1.5f;
    if (t->has_face_geo && 2.0f * t->fry * 1.25f > side) side = 2.0f * t->fry * 1.25f;
    int x0 = (int)fmaxf(0, cx - side / 2), y0 = (int)fmaxf(0, cy - side / 2);
    int x1 = (int)fminf((float)f->w, cx + side / 2), y1 = (int)fminf((float)f->h, cy + side / 2);
    if (x1 - x0 < 16 || y1 - y0 < 16) return NULL;
    aurix_image crop = { f->data + (size_t)y0 * f->stride + (size_t)x0 * 3, x1 - x0, y1 - y0, f->stride, 3 };
    int cw = crop.w, ch = crop.h;
    if (ch > 480) { cw = cw * 480 / ch; ch = 480; }            /* cap the size, keep detail */
    if (cw == crop.w) return jpeg_encode_rgb(&crop, 85, len);
    unsigned char *px = malloc((size_t)cw * ch * 3);
    if (!px) return NULL;
    aurix_image small = { px, cw, ch, cw * 3, 3 };
    resize_bilinear(&crop, &small);
    unsigned char *j = jpeg_encode_rgb(&small, 85, len);
    free(px);
    return j;
}

unsigned char *snapshot_scene(const aurix_image *f, size_t *len)
{
    /* 1280 wide, the camera's own shape (1280x960 on a 4:3 sensor, 1280x720 on 16:9) */
    int sw = 1280, sh = (int)((long long)f->h * 1280 / f->w) & ~1;
    if (f->w <= sw) return jpeg_encode_rgb(f, 75, len);
    unsigned char *px = malloc((size_t)sw * sh * 3);
    if (!px) return NULL;
    aurix_image small = { px, sw, sh, sw * 3, 3 };
    resize_bilinear(f, &small);
    unsigned char *j = jpeg_encode_rgb(&small, 75, len);
    free(px);
    return j;
}

