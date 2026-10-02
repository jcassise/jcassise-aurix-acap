#include "embed.h"
#include "config.h"
#include "embed_meta.h"
#include "infer.h"
#include "match.h"
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

struct aurix_embedder {
    aurix_model *model;
    embed_meta meta;
    size_t in_pitch;
};

aurix_embedder *embedder_open(const char *model_path, const char *meta_path, const char *device)
{
    aurix_embedder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (embed_meta_load(meta_path, &e->meta) || e->meta.dim > AURIX_MAX_DIM) {
        syslog(LOG_ERR, "embedder: cannot read meta %s", meta_path);
        goto fail;
    }
    e->model = model_load(model_path, device);
    if (!e->model) goto fail;

    int h, w, c;
    size_t bytes = 0;
    if (model_input_dims(e->model, 0, &h, &w, &c, &e->in_pitch) || w != e->meta.in_w ||
        h != e->meta.in_h || c != 3 || model_input_type(e->model, 0) != AURIX_DT_UINT8) {
        syslog(LOG_ERR, "embedder: model input does not match meta (%dx%d uint8 RGB)", e->meta.in_w, e->meta.in_h);
        goto fail;
    }
    if (!model_output(e->model, 0, &bytes) || model_output_type(e->model, 0) != AURIX_DT_INT8 ||
        bytes != (size_t)e->meta.dim) {
        syslog(LOG_ERR, "embedder: output is not int8[%d] (%zu bytes)", e->meta.dim, bytes);
        goto fail;
    }
    syslog(LOG_INFO, "embedder: %d-d, %dx%d input, zero point %d", e->meta.dim, w, h, e->meta.zero_point);
    return e;
fail:
    embedder_close(e);
    return NULL;
}

void embedder_close(aurix_embedder *e)
{
    if (!e) return;
    model_free(e->model);
    free(e);
}

int embedder_dim(const aurix_embedder *e) { return e->meta.dim; }

int embed_face(aurix_embedder *e, const aurix_image *face, int8_t *q, uint32_t max_dim)
{
    const int w = e->meta.in_w, h = e->meta.in_h, dim = e->meta.dim;
    if (face->w != w || face->h != h || face->ch != 3 || (uint32_t)dim > max_dim) return -1;
    size_t in_bytes = 0;
    uint8_t *in = model_input(e->model, 0, &in_bytes);
    if (!in || in_bytes < e->in_pitch * (size_t)h) return -1;
    for (int j = 0; j < h; j++)
        memcpy(in + (size_t)j * e->in_pitch, face->data + (size_t)j * face->stride, (size_t)w * 3);

    if (model_run(e->model)) return -1;

    const int8_t *out = model_output(e->model, 0, NULL);
    float tmp[AURIX_MAX_DIM];
    for (int i = 0; i < dim; i++) tmp[i] = (float)(out[i] - e->meta.zero_point);  /* scale cancels */
    embedding_quantize(tmp, (uint32_t)dim, q);
    return dim;
}
