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
app/manifest.json      ACAP manifest (appName "aurix", respawn)
app/Makefile           built inside the ACAP Native SDK container
app/src/capture.*      VDO capture; RGB native, NV12→RGB fallback for ARTPEC-7
app/src/infer.*        larod v3 wrapper (.tflite only, mmapped tensors, reusable job)
app/src/detect.*       detector pre-processing + (TODO) output decoding
app/src/align.*        5-pt similarity alignment to 112×112 ArcFace template
app/src/embed.*        embedder; float32 or int8/uint8 outputs → int8 L2-normalised
app/src/match.*        gallery.bin loader, NEON int8 dot product, cosine top-1
app/src/main.c         loop + per-stage latency stats every 100 frames
app/models/            put detect.tflite / embed.tflite here (not committed)
tests/                 host tests for align / image / match
tools/make_gallery.py  enrolment embeddings (.npz) → gallery.bin
```

## Build

CI (GitHub Actions) builds both `.eap` packages on every push to `main` and uploads them
as artifacts. It also runs the host tests. Locally (needs Docker Hub access):

```sh
docker build --build-arg SDK_IMAGE=axisecp/acap-native-sdk:<tag> -t aurix .
id=$(docker create aurix); docker cp $id:/opt/app ./build; docker rm $id
make -C tests run
```

## Install and watch

Upload the `.eap` from the camera web UI (Apps) or AXIS Device Manager, then check
the system log for `aurix:` lines (`stats ...` = per-stage latency, `MATCH ...` = hits).
The app idles rather than exits if models or VDO are unavailable.

## Status – what works and what doesn't yet

Done and tested on host (x86, plus aarch64/armv7hf NEON under QEMU): alignment, resize,
NV12→RGB, int8 matching, gallery file format. Syntax-checked only (needs real SDK + camera):
`capture.c`, `infer.c`, `main.c`.

Before the first useful run on hardware:

1. **Detector output decoding** (`src/detect.c`, `decode_outputs`) — stubbed, returns 0
   faces. Depends on YuNet vs BlazeFace head. Until then, the build benchmarks capture and
   detector latency only.
2. **Verify SDK image tags** in `.github/workflows/build.yml` against the ACAP Native SDK
   compatibility table (11.11 for armv7hf, 12.11 for aarch64).
3. **Calibrate `match_threshold`** (placeholder 0.45) on real data.

## AXIS OS 13 readiness checklist

Already compliant: larod v3 with `.tflite` only, no deprecated VDO frame fields (geometry
from `vdo_stream_get_info`), no custom users/groups or dbus, no SD-card install.

Still to do before moving P3267 to 13:
- Manifest schema v2 with mandatory `compatibleOsVersions` (current `1.5.0` is chosen
  for 11.11/12.11 compatibility; confirm exact v2 version string and field syntax).
- Signing via ACAP Signing Service (non-TIP) or ACAP Service Portal (TIP).
- Replace syslog matches with Axis events / Device Data Hub API (Message Broker is gone).

## Not yet started

PAD/liveness (hook in `main.c`), tracking + best-frame selection, tiled/high-res detection
for watchlist at distance, runtime config via axparameter, on-prem management app.
