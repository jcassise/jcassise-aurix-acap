/* AURIX - runtime configuration (compile-time defaults for now). */
#ifndef AURIX_CONFIG_H
#define AURIX_CONFIG_H

#define AURIX_APP_DIR   "/usr/local/packages/aurix"
#define AURIX_MAX_FACES 16
#define AURIX_MAX_DIM   1024
#define AURIX_FACE_SIZE 112

typedef struct {
    const char *detect_model;
    const char *detect_meta;    /* output roles + quant params, from tools/convert_yunet.py */
    const char *embed_model;
    const char *pad_model;      /* NULL = liveness disabled (watchlist use case) */
    const char *gallery_path;
    const char *device;         /* larod device name */
    unsigned width, height;     /* capture resolution */
    double fps;
    unsigned max_faces;
    unsigned min_eye_px;        /* skip faces below this inter-eye distance */
    float detect_threshold;
    float match_threshold;      /* cosine; PLACEHOLDER - calibrate on real data */
    int embed_zero_point;       /* output zero point if embedder emits int8/uint8 */
    unsigned stats_every;       /* log latency stats every N frames */
} aurix_config;

void config_defaults(aurix_config *cfg);

#endif
