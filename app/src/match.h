/* AURIX - int8 embedding gallery and cosine matching (NEON when available). */
#ifndef AURIX_MATCH_H
#define AURIX_MATCH_H
#include <stdint.h>

#define AURIX_ID_LEN 64

typedef enum { AURIX_CAT_ALLOW = 0, AURIX_CAT_THREAT = 1 } aurix_category;

/* gallery.bin (little-endian):
 *   char magic[4] = "AURG"; u32 version (1 or 2); u32 dim; u32 count;
 *   v1: count x { char id[64]; int8 emb[dim] }                 (category = allow)
 *   v2: count x { char id[64]; u8 category; int8 emb[dim] }
 *
 * Gallery parameter string (app setting "Gallery", set in the camera UI or via VAPIX):
 *   entries separated by ';', each  name,category,kind,base64(int8 emb)
 *   category: allow | threat     kind: dlpu | cpu (must match this camera's embedder)
 * Produced by tools/enroll.py --param. */
typedef struct {
    uint32_t dim, count, cap;
    char (*ids)[AURIX_ID_LEN];
    uint8_t *category;
    int8_t *emb;        /* count * dim */
    float *inv_norm;    /* per record */
} aurix_gallery;

int  gallery_load(const char *path, aurix_gallery *g);
void gallery_free(aurix_gallery *g);
void gallery_init(aurix_gallery *g, uint32_t dim);
int  gallery_add(aurix_gallery *g, const char *id, aurix_category cat, const int8_t *emb);

/* Appends entries from a parameter string. Entries whose kind != `kind` or whose size != g->dim
 * are skipped. Returns entries added, or -1 on a malformed string (nothing partially added). */
int  gallery_parse_param(aurix_gallery *g, const char *s, const char *kind);

/* L2-normalise then scale to [-127,127]. */
void embedding_quantize(const float *in, uint32_t dim, int8_t *out);

int32_t dot_s8(const int8_t *a, const int8_t *b, uint32_t dim);

/* Best cosine match. Returns index or -1 (empty gallery / zero query). */
int gallery_best(const aurix_gallery *g, const int8_t *q, float *score);

#endif
