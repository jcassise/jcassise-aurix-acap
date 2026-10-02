/* AURIX - face embedding stage (MobileFaceNet on larod). */
#ifndef AURIX_EMBED_H
#define AURIX_EMBED_H
#include <stdint.h>
#include "image.h"

typedef struct aurix_embedder aurix_embedder;

/* model: embed.tflite, meta: embed.meta (from tools/convert_mobilefacenet.py). */
aurix_embedder *embedder_open(const char *model_path, const char *meta_path, const char *device);
void embedder_close(aurix_embedder *e);
int embedder_dim(const aurix_embedder *e);

/* face: aligned RGB crop matching the model input (112x112). Writes the L2-normalised int8
 * embedding to q (embedder_dim() values). Returns the dimension, or -1. */
int embed_face(aurix_embedder *e, const aurix_image *face, int8_t *q, uint32_t max_dim);

#endif
