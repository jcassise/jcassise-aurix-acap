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
app/src/embed.*        MobileFaceNet on larod → int8 L2-normalised 128-d embedding
app/src/embed_meta.*   embedder sidecar (input size, output zero point)
app/src/match.*        gallery.bin loader, NEON int8 dot product, cosine top-1
app/src/main.c         GLib main loop (overlay, settings) + pipeline worker thread, stats
app/src/overlay.*      live-view boxes: green allow, red threat, blue unknown, grey too small
app/src/pharos*.{c,h}  AURIX–Pharos protocol v1 client: HTTPS + pinning, /hello, status loop,
                       config read-back, once-only commands, §8 errors/backoff
app/src/metrics.*      live performance metrics (per-stage time, faces, matches, history)
app/src/tracker.*      face tracking: one track per person in view, name locked then kept (one event per visit)
app/src/events.*       which tracks are reported (role, reporting.*) and their Event JSON
app/src/event_queue.*  offline queue: upsert by eventId, priority eviction (strangers first, Threat last)
app/src/access.*       virtual-access decisions: policies, zones, schedules, excluded dates, time zone
app/src/sync.*         identity sync with Pharos: poll, delta/full (staged, atomic), deletions, photos
app/src/person_store.* people + on-camera templates, persisted in localdata/pharos/people.json
app/src/enroll.*       photo -> template on the camera (same detector/aligner/embedder as live)
app/src/jpeg.*         JPEG decoding (stb_image)
app/src/capacity.*     people-capacity estimate (memory, storage, matching speed)
app/src/sysinfo.*      CPU, memory, temperature, storage from /proc and /sys
app/src/web.*          dashboard over the camera's web server (FastCGI, aurix.cgi)
app/web/dashboard.html on-camera dashboard page (self-contained, no internet needed)
contract/              vendored AURIX–Pharos contract (OpenAPI, schemas, examples) - owner: Pharos
tests/pharos/          protocol scenarios: real client vs a contract-validating stand-in server
app/models/            put detect.tflite / embed.tflite here (not committed)
tests/                 host tests for align / image / match
tools/make_gallery.py  enrolment embeddings (.npz) → gallery.bin
tools/convert_yunet.py YuNet ONNX → per-tensor INT8 TFLite + detect.meta (run by CI)
tools/convert_mobilefacenet.py  MobileFaceNet → embed_dlpu (per-tensor + CLE) and
                       embed_cpu (per-channel) INT8 TFLite + meta (run by CI; no torch needed)
tools/enroll.py        photos per person → gallery.bin, same pipeline + model as the camera
```

## Build

CI (GitHub Actions) builds both `.eap` packages on every push to `main` and uploads them
as artifacts. It also runs the host tests. Locally (needs Docker Hub access):

```sh
docker build --build-arg SDK_IMAGE=axisecp/acap-native-sdk:12.11.0-aarch64-ubuntu24.04 --build-arg ARCH=aarch64 -t aurix .
id=$(docker create aurix); docker cp $id:/opt/app ./build; docker rm $id
make -C tests run
```

## App settings (camera web UI → Apps → aurix → Settings, or VAPIX param.cgi)

| Setting | Default | Meaning |
|---|---|---|
| Gallery | empty | identities: `name,allow\|threat,dlpu\|cpu,base64` joined by `;` (from `tools/enroll.py --param`) |
| MatchThreshold | 45 | cosine × 100 needed for a match |
| Overlay | yes | draw boxes in live view |

Changes apply immediately (no restart). `localdata/gallery.bin` is still read and merged if present.
Use `dlpu` entries on ARTPEC-8 and `cpu` entries on ARTPEC-7; others are ignored.

## Performance dashboard

Camera web UI → **Apps → aurix → Open** (or `https://<camera>/local/aurix/aurix.cgi`;
admin login). Refreshes every 2 s:

- **Frame budget**: where each frame's milliseconds go (prepare, detect, align, identify,
  match) against the target frame rate; time spent waiting for the next camera frame is
  shown as spare.
- Processor, load, memory (system and AURIX), temperature, app storage.
- Faces seen / identified / matched per minute and since start; threshold and minimum face.
- People on the camera (allowed / threat, source), Pharos connection and config revision.
- 10-minute charts (frame rate, processor, memory) and the last 20 matches.
- **Capacity**: how many people the camera can hold, and what limits it - free memory
  (~1 KB per person, 96 MB reserve), free app storage (~8 KB per person once Pharos sync
  caches face crops, 32 MB reserve) and matching speed (5 ms per face budget, measured on the
  chip by a start-up benchmark logged as `matching benchmark: … ns per gallery entry`).

Raw JSON: `aurix.cgi?data` - usable for logging benchmarks from a script.

## Connecting to Pharos (protocol v1, step 1)

In Pharos: Hardware → AURIX devices → Add. Then on the camera open **Apps → aurix → Open**
and use the **Pharos connection** panel at the bottom of the AURIX page: Pharos address,
device ID, device token (masked; never shown again) and the server's **public key** or
certificate - choose the file (PEM, DER, `.pub`) or paste it, or paste a fingerprint/pin. Saving validates everything,
shows the certificate's name, fingerprint and expiry to check against Pharos, and connects.
It is stored in `localdata/pharos/commission.json` (owner-only) and takes precedence over
the app settings below.

The older route still works - **Apps → aurix → Settings**:

| Setting | Value |
|---|---|
| PharosUrl | `https://<pharos-host>` (AURIX appends `/aurix/v1`) |
| PharosDeviceId | the device ID Pharos created |
| PharosToken | the one-time device token |
| PharosServerCert | certificate fingerprint (as `openssl x509 -fingerprint -sha256` prints it, or plain hex), `sha256//<base64>` or bare base64 SPKI pin, or the PEM; two separated by `;` during rotation. Empty = CA validation |

**PharosStatus** shows the connection state (Connected, Credentials rejected, TLS pin
mismatch, Revoked, …). While commissioned, Pharos owns the match threshold and minimum
face size; the local MatchThreshold setting is ignored. The last config from Pharos is
kept in `localdata/pharos/` and applied at boot even if Pharos is unreachable.

Implemented: `/hello` (with capacity `limits`), status with config read-back, once-only
commands (`resync`, `reenroll` real), errors/backoff (§8), and **identity sync (§5)**: people
and photos from Pharos become on-camera templates (one per photo, up to 5 per person);
full syncs are staged and only applied when complete; deletions; policies stored; clock
offset kept from Pharos `serverTime`. Watchlist classes: green = allowed, amber = Concern,
red = Threat.

**Events (§7, 0.7.0):** faces are tracked; a name locks after two confident frames and is kept
while the same face stays in view (head turns don't drop it). One event per visit: opened when the
person is identified (or declared a stranger), updated if a clearly better face is seen, closed
(`endedAt`) after `events.trackCloseSec` without the face. Face crop (≥240 px or native) and scene
(1280×720) JPEGs follow each event. Watchlist role: Threat/Concern always, strangers as plain
sightings. Virtual access: every decision is an access event (granted, or denied with reason; a
stranger is `denied/stranger`). Offline: events queue on the camera and are delivered in order.
Overlay: ellipses with a soft glow; magenta = known but not authorised, orange-red = stranger in a
restricted area.

## Install and watch

Upload the `.eap` from the camera web UI (Apps) or AXIS Device Manager, then check
the system log for `aurix:` lines (`stats ...` = per-stage latency, `MATCH ...` = hits).
The app idles rather than exits if models or VDO are unavailable.

## Status – what works and what doesn't yet

Measured on hardware (2026-10-02), detection only:

| Camera | Detect | Capture+convert | Rate |
|---|---|---|---|
| P3267 (ARTPEC-8 DLPU) | 55 ms | 46 ms | 10 fps (cap) |
| P3248 (ARTPEC-7 CPU) | 419 ms | 94 ms | ~2 fps |

**Detector**: YuNet-n, per-tensor INT8 (640×352). **Embedder**: MobileFaceNet 128-d, rebuilt
from PyTorch weights exactly (1e-6), INT8 cosine vs float ≈0.97 (DLPU, per-tensor + safe CLE)
and ≈0.995 (CPU, per-channel). Offline check with the DLPU model: 7/7 unseen photos identified
(lowest genuine 0.555, highest impostor 0.217, unknown bystander 0.204 → rejected at 0.45).

Next:
1. **Larger galleries**: the Gallery setting suits a handful of people; thousands need file sync
   from the management app.
2. **Dual VDO stream**: hardware-scaled small stream for the detector, 1080p NV12 for crops;
   removes most CPU pre-processing (~80 ms/frame on the P3267).
3. Recalibrate on real camera frames; QAT on own licensed data for production.

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
