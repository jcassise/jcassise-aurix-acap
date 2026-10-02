/* AURIX - embedder sidecar meta (pure C). */
#ifndef AURIX_EMBED_META_H
#define AURIX_EMBED_META_H

typedef struct {
    int in_w, in_h;
    int dim;          /* embedding length */
    float scale;      /* output quantisation (scale cancels in cosine; kept for completeness) */
    int zero_point;
} embed_meta;

int embed_meta_load(const char *path, embed_meta *m);

#endif
