# Models (not committed)

CI generates `detect.tflite` + `detect.meta` with `tools/convert_yunet.py` and bundles them
into each `.eap`. For local builds, run the same script:

    pip install -r tools/requirements-models.txt
    python3 tools/convert_yunet.py --out app/models [--calib-dir camera_frames/]

Model files are git-ignored on purpose: current research weights (YuNet trained on
WIDER FACE; embedder TBD) are **internal benchmarking only** and must not ship. Production
weights will be retrained on commercially licensed or synthetic data with per-tensor QAT.

- `detect.tflite`: YuNet-n, 1x352x640x3 uint8 RGB in, 12 int8 NHWC head maps out,
  full INT8 per-tensor (no per-axis tensors). ~110 KB, ~74k params.
- `detect.meta`: output order + scale/zero-point per output; read by `src/yunet.c`.
- `embed.tflite` (not yet): 1x112x112x3 uint8 RGB in; one embedding out (float32 or
  int8/uint8 - set `embed_zero_point` in `src/config.c`).
