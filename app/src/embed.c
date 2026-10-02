#include "embed.h"
#include "config.h"
#include "match.h"
#include <string.h>
#include <syslog.h>

int embed_face(aurix_model *m, const aurix_image *face, int8_t *q, uint32_t max_dim, int zero_point)
{
    int h, w, c;
    size_t pitch;
    if (model_input_dims(m, 0, &h, &w, &c, &pitch) || h != face->h || w != face->w || c != face->ch)
        return -1;
    if (model_input_type(m, 0) != AURIX_DT_UINT8) {
        /* Normalisation (mean/std) is expected to be folded into the quantised model input. */
        syslog(LOG_ERR, "embedder: only uint8 input supported for now");
        return -1;
    }
    size_t in_bytes = 0;
    uint8_t *in = model_input(m, 0, &in_bytes);
    if (!in || in_bytes < pitch * (size_t)h) return -1;
    for (int j = 0; j < h; j++)
        memcpy(in + (size_t)j * pitch, face->data + (size_t)j * face->stride, (size_t)w * c);

    if (model_run(m)) return -1;

    size_t out_bytes = 0;
    const void *out = model_output(m, 0, &out_bytes);
    aurix_dtype dt = model_output_type(m, 0);
    uint32_t dim;
    float tmp[AURIX_MAX_DIM];

    switch (dt) {
    case AURIX_DT_FLOAT32:
        dim = (uint32_t)(out_bytes / sizeof(float));
        if (dim == 0 || dim > max_dim || dim > AURIX_MAX_DIM) return -1;
        memcpy(tmp, out, dim * sizeof(float));
        break;
    case AURIX_DT_INT8:
    case AURIX_DT_UINT8:
        dim = (uint32_t)out_bytes;
        if (dim == 0 || dim > max_dim || dim > AURIX_MAX_DIM) return -1;
        for (uint32_t i = 0; i < dim; i++) {
            int v = dt == AURIX_DT_INT8 ? ((const int8_t *)out)[i] : ((const uint8_t *)out)[i];
            tmp[i] = (float)(v - zero_point);   /* scale cancels in cosine similarity */
        }
        break;
    default:
        return -1;
    }
    embedding_quantize(tmp, dim, q);
    return (int)dim;
}
