/* AURIX - int8 embedding gallery and cosine matching (NEON when available). */
#ifndef AURIX_MATCH_H
#define AURIX_MATCH_H
#include <stdint.h>

#define AURIX_ID_LEN 64

/* gallery.bin (little-endian):
 *   char magic[4] = "AURG"; u32 version = 1; u32 dim; u32 count;
 *   count x { char id[64] (NUL-padded); int8 emb[dim] }
 * Build with tools/make_gallery.py. */
typedef struct {
    uint32_t dim, count;
    char (*ids)[AURIX_ID_LEN];
    int8_t *emb;        /* count * dim */
    float *inv_norm;    /* per record, precomputed at load */
} aurix_gallery;

int  gallery_load(const char *path, aurix_gallery *g);
void gallery_free(aurix_gallery *g);

/* L2-normalise then scale to [-127,127]. */
void embedding_quantize(const float *in, uint32_t dim, int8_t *out);

int32_t dot_s8(const int8_t *a, const int8_t *b, uint32_t dim);

/* Best cosine match. Returns index or -1 (empty gallery / zero query). */
int gallery_best(const aurix_gallery *g, const int8_t *q, float *score);

#endif
