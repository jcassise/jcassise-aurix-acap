# Models (not committed)

Place `detect.tflite` and `embed.tflite` here before building. They are bundled into the
.eap only if present. `.tflite` files are git-ignored on purpose: the current research
weights (WIDER FACE / WebFace-family training data) are **internal benchmarking only** and
must never be pushed or shipped. Production weights will be retrained on commercially
licensed or synthetic data with per-tensor INT8 QAT for the ARTPEC-8 DLPU.

Expected interfaces (current code):
- `detect.tflite`: input NHWC uint8 RGB. Output decoding is a TODO in `src/detect.c`.
- `embed.tflite`: input 1x112x112x3 uint8 RGB (normalisation folded in); output one
  embedding tensor, float32 or int8/uint8 (set `embed_zero_point` in `src/config.c`).
