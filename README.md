# AURIX ACAP – on-camera face matching (benchmark build)

First test ACAP for AXIS cameras. Pipeline:

```
VDO frames → larod (detect) → CPU 5-pt alignment → larod (embed) → NEON cosine match → syslog
```

**Internal benchmarking only.** Current research weights are non-commercial and are never
committed or shipped (`*.tflite` is git-ignored). Production weights will be retrained on
commercially licensed or synthetic data with per-tensor INT8 QAT.

## Targets

| Camera | Chip | Arch | AXIS OS | larod device |
|---|---|---|---|---|
| P3248-LV | ARTPEC-7 | armv7hf | 11.11 LTS | `cpu-tflite` (2 fps trigger rate) |
| P3267-LV | ARTPEC-8 | aarch64 | 12.11 LTS | `axis-a8-dlpu-tflite` (10 fps) |

## Layout

```
app/manifest.json.*    per-arch manifests: armv7hf schema 1.7.0 (SDK 1.15),
                       aarch64 schema 2.2.0 + video group + DLPU (SDK 12.11)
app/Makefile           built inside the ACAP Native SDK container
app/src/capture.*      VDO capture; RGB native, NV12→RGB fallback for ARTPEC-7
app/src/infer.*        larod v3 wrapper: mmapped tensors, pitch-aware, DLPU power retries
app/src/tensor.*       pitched-tensor repack (pure C)
app/src/detect.*       YuNet on larod: resize, run, scale back to frame coords
app/src/yunet.*        YuNet output decode + NMS (pure C) driven by detect.meta
app/src/align.*        5-pt similarity alignment to 112×112 ArcFace template
app/src/embed.*        embedder; float32 or int8/uint8 outputs → int8 L2-normalised
app/src/match.*        gallery.bin loader, NEON int8 dot product, cosine top-1
app/src/main.c         loop + per-stage latency stats every 100 frames
app/models/            put detect.tflite / embed.tflite here (not committed)
tests/                 host tests for align / image / match
tools/make_gallery.py  enrolment embeddings (.npz) → gallery.bin
tools/convert_yunet.py YuNet ONNX → per-tensor INT8 TFLite + detect.meta (run by CI)
```

## Build

CI (GitHub Actions) builds both `.eap` packages on every push to `main` and uploads them
as artifacts. It also runs the host tests. Locally (needs Docker Hub access):

```sh
docker build --build-arg SDK_IMAGE=axisecp/acap-native-sdk:12.11.0-aarch64-ubuntu24.04 --build-arg ARCH=aarch64 -t aurix .
id=$(docker create aurix); docker cp $id:/opt/app ./build; docker rm $id
make -C tests run
```

## Install and watch

Upload the `.eap` from the camera web UI (Apps) or AXIS Device Manager, then check
the system log for `aurix:` lines (`stats ...` = per-stage latency, `MATCH ...` = hits).
The app idles rather than exits if models or VDO are unavailable.

## Status – what works and what doesn't yet

**Detector (YuNet-n) is in.** CI converts it to full INT8 per-tensor TFLite (640×352 uint8
RGB in, ~110 KB) and bundles it into both `.eap`s. Validated in conversion: INT8 finds the
same faces as float (IoU > 0.5), landmark drift ~3% of inter-eye distance. The C decoder
(`src/yunet.c`) matches the Python reference to < 0.01 px on x86, aarch64 and armv7hf.

Tested on host (incl. NEON under QEMU): alignment, resize, NV12→RGB, YuNet decode, int8
matching, gallery format. Compiled against the real SDK in CI
but not yet run on a camera: `capture.c`, `infer.c`, `detect.c`, `main.c`.

Without an embedder the app runs a **detection benchmark**: the log `stats` line shows
faces detected / passing the 40 px eye gate and per-stage latency.

Next:
1. **Run on both cameras**, read latency, confirm models land in `models/` inside the package.
2. **Recalibrate** the detector on real camera frames (`--calib-dir`); current calibration
   is synthetic.
3. **Embedder** conversion (benchmark only), then calibrate `match_threshold` (placeholder 0.45).

## AXIS OS 13 readiness checklist

Already compliant: larod v3 with `.tflite` only, no deprecated VDO frame fields (geometry
from `vdo_stream_get_info`), no custom users/groups or dbus, no SD-card install.

Done for the aarch64 build: manifest schema 2.2.0 with `compatibleOsVersions`
(max 13), the `video` group and the `deepLearningProcessor` resource, following Axis'
official examples (acap-native-sdk-examples 12.11.0).

Still to do before moving P3267 to 13:
- Real `vendorId` (placeholder `1234567890`, as in Axis' examples) - required for signing.
- Signing via ACAP Signing Service (non-TIP) or ACAP Service Portal (TIP).
- Replace syslog matches with Axis events / Device Data Hub API (Message Broker is gone).

## Not yet started

PAD/liveness (hook in `main.c`), tracking + best-frame selection, tiled/high-res detection
for watchlist at distance, runtime config via axparameter, on-prem management app.
