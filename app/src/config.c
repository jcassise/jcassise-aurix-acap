#include "config.h"
#include <stddef.h>

void config_defaults(aurix_config *c)
{
    c->detect_model = AURIX_APP_DIR "/models/detect.tflite";
    c->detect_meta  = AURIX_APP_DIR "/models/detect.meta";
    c->embed_model  = AURIX_APP_DIR "/models/embed.tflite";
    c->embed_meta   = AURIX_APP_DIR "/models/embed.meta";
    c->pad_model    = NULL;
    c->gallery_path = AURIX_APP_DIR "/localdata/gallery.bin";
#if defined(__aarch64__)
    /* ARTPEC-8 DLPU. ARTPEC-9 uses "a9-dlpu-tflite" - verify on hardware. */
    c->device = "axis-a8-dlpu-tflite";
    c->embed_kind = "dlpu";
    c->fps = 10.0;
    c->max_embed_per_frame = 4;
#else
    /* ARTPEC-7 (P3248): no accelerator, CPU TFLite, low trigger rate. */
    c->device = "cpu-tflite";
    c->embed_kind = "cpu";
    c->fps = 2.0;
    c->max_embed_per_frame = 1;
#endif
    c->width = 1920;
    c->height = 1080;
    c->max_faces = 8;
    c->min_eye_px = 40;
    c->detect_threshold = 0.6f;
    c->match_threshold = 0.45f;
    c->stats_every = 100;
}
