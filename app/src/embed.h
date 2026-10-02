/* AURIX - face embedding stage. */
#ifndef AURIX_EMBED_H
#define AURIX_EMBED_H
#include <stdint.h>
#include "image.h"
#include "infer.h"

/* face: aligned RGB crop matching the model input (normally 112x112).
 * Writes the L2-normalised int8 embedding to q. Returns its dimension, or -1. */
int embed_face(aurix_model *m, const aurix_image *face, int8_t *q, uint32_t max_dim, int zero_point);

#endif
