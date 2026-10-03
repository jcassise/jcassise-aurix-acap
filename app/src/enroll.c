#include "enroll.h"
#include "align.h"
#include "config.h"
#include "jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENROLL_MAX_SIDE 1600       /* decode cap: plenty for a face that fills the photo */
#define ENROLL_MIN_EYE_PX 24.0f    /* below this the template is unreliable */
#define DETECT_ASPECT (640.0 / 352.0)

int enroll_photo(aurix_detector *det, aurix_embedder *emb, const unsigned char *jpeg, size_t len,
                 int8_t *out, uint32_t dim, char *why, size_t wl)
{
    if (!det || !emb) { snprintf(why, wl, "recognition models are not loaded"); return -1; }
    if ((uint32_t)embedder_dim(emb) != dim) { snprintf(why, wl, "embedder size mismatch"); return -1; }
    aurix_image photo, canvas;
    if (jpeg_decode_rgb(jpeg, len, ENROLL_MAX_SIDE, &photo, why, wl)) return -1;
    int ox, oy;
    if (image_letterbox(&photo, DETECT_ASPECT, &canvas, &ox, &oy)) { free(photo.data); snprintf(why, wl, "out of memory"); return -1; }
    free(photo.data);

    aurix_face faces[8];
    int n = detect_faces(det, &canvas, faces, 8, 0.6f);
    int best = -1;
    float best_area = 0;
    for (int i = 0; i < n; i++) {
        float a = (faces[i].x1 - faces[i].x0) * (faces[i].y1 - faces[i].y0);
        if (a > best_area) { best_area = a; best = i; }
    }
    int rc = -1;
    if (n < 0) snprintf(why, wl, "face detector failed");
    else if (best < 0) snprintf(why, wl, "no face found in the photo");
    else if (landmarks_eye_distance(&faces[best].lm) < ENROLL_MIN_EYE_PX)
        snprintf(why, wl, "face too small in the photo (use a closer portrait)");
    else {
        uint8_t px[AURIX_FACE_SIZE * AURIX_FACE_SIZE * 3];
        aurix_image face = { px, AURIX_FACE_SIZE, AURIX_FACE_SIZE, AURIX_FACE_SIZE * 3, 3 };
        if (align_face(&canvas, &faces[best].lm, &face)) snprintf(why, wl, "could not align the face");
        else if (embed_face(emb, &face, out, dim) != (int)dim) snprintf(why, wl, "embedder failed");
        else rc = 0;
    }
    free(canvas.data);
    return rc;
}
