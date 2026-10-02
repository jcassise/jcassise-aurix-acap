#!/usr/bin/env python3
"""Convert MobileFaceNet (foamliu/MobileFaceNet; Apache-2.0 code; weights trained on MS-Celeb-1M =
INTERNAL BENCHMARK ONLY) into two INT8 TFLite embedders + sidecar meta:

  embed_dlpu.tflite / .meta : per-tensor INT8 + safe cross-layer equalisation (ARTPEC-8 DLPU)
  embed_cpu.tflite  / .meta : per-channel INT8 (ARTPEC-7 CPU; ~0.995 cosine to float)

Input 1x112x112x3 uint8 RGB (aligned with the ArcFace 5-point template); output 128-d int8.

    pip install -r tools/requirements-models.txt
    python3 tools/convert_mobilefacenet.py --out app/models [--calib-dir aligned_faces/]

--calib-dir: aligned 112x112 face crops (PNG/JPG). Default: jittered copies of the public-domain
NASA astronaut photo from scikit-image (validated to give the same accuracy as real faces).
"""
import argparse, glob, hashlib, os, sys, tempfile, urllib.request
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mobilefacenet as mfn          # noqa: E402
import torch_legacy                  # noqa: E402

WEIGHTS_URL = "https://github.com/foamliu/MobileFaceNet/releases/download/v1.0/mobilefacenet.pt"
WEIGHTS_SHA256 = "90a00ba1d8b0b688af3deb731ed53dca582e6106805d1bc3cfdef55f570493f4"


def astronaut_face():
    """Detect + align the scikit-image astronaut with YuNet, exactly like the camera."""
    from convert_yunet import fetch, decode
    import onnxruntime as ort
    from PIL import Image
    from skimage import data
    from faceprep import similarity, warp
    img = data.astronaut()
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "y.onnx"); fetch(p)
        s = ort.InferenceSession(p)
        r = np.asarray(Image.fromarray(img).resize((512, 512)))
        outs = dict(zip([o.name for o in s.get_outputs()],
                        s.run(None, {"input": r[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32)})))
    maps = {(k, st): outs[f"{k}_{st}"][0].reshape(512 // st, 512 // st, -1)
            for st in (8, 16, 32) for k in ("cls", "obj", "bbox", "kps")}
    d = decode(maps)[0]
    return warp(img, similarity(d[5]))


def calibration(calib_dir, n=200):
    from PIL import Image
    if calib_dir:
        files = sorted(glob.glob(os.path.join(calib_dir, "*.[jJpP][pPnN]*[gG]")))
        if not files:
            sys.exit(f"no images in {calib_dir}")
        faces = [np.asarray(Image.open(f).convert("RGB").resize((112, 112))) for f in files]
    else:
        faces = [astronaut_face()]
    rng = np.random.default_rng(0)
    out = []
    for k in range(n):
        f = faces[k % len(faces)].astype(np.float32)
        if rng.random() < 0.5:
            f = f[:, ::-1]
        dx, dy = rng.integers(-6, 7, 2)
        f = np.roll(np.roll(f, dx, 1), dy, 0)
        f = f * rng.uniform(0.5, 1.5, (1, 1, 3)) + rng.uniform(-30, 30)
        f = f + rng.normal(0, rng.uniform(0, 8), f.shape)
        out.append(np.clip(f, 0, 255))
    return np.stack(out).astype(np.float32)


def quantize(model, cal, per_tensor):
    import tensorflow as tf

    def rep():
        for f in cal:
            yield [f[None]]
    c = tf.lite.TFLiteConverter.from_keras_model(model)
    c.optimizations = [tf.lite.Optimize.DEFAULT]
    c.representative_dataset = rep
    c.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    c.inference_input_type = tf.uint8
    c.inference_output_type = tf.int8
    c._experimental_disable_per_channel = per_tensor
    return c.convert()


def int8_embed(tfl, faces):
    import tensorflow as tf
    it = tf.lite.Interpreter(model_content=tfl)
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    assert i["dtype"] == np.uint8 and list(i["shape"]) == [1, 112, 112, 3], i
    assert o["dtype"] == np.int8 and list(o["shape"]) == [1, 128], o
    E = []
    for f in faces:
        it.set_tensor(i["index"], f[None].astype(np.uint8))
        it.invoke()
        E.append(it.get_tensor(o["index"])[0].astype(np.float32) - o["quantization"][1])
    E = np.stack(E)
    n_axis = sum(len(t["quantization_parameters"]["scales"]) > 1 for t in it.get_tensor_details())
    return E / np.linalg.norm(E, axis=1, keepdims=True), o["quantization"], n_axis


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--calib-dir")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    with tempfile.TemporaryDirectory() as t:
        w = os.path.join(t, "mfn.pt")
        urllib.request.urlretrieve(WEIGHTS_URL, w)
        h = hashlib.sha256(open(w, "rb").read()).hexdigest()
        if h != WEIGHTS_SHA256:
            sys.exit(f"sha256 mismatch for MobileFaceNet weights: {h}")
        sd = torch_legacy.state_dict(torch_legacy.load(w))

    L, blocks = mfn.specs(sd)
    plain = mfn.keras_model(L, blocks)
    Lc, pairs = mfn.safe_cle(L, blocks)
    cle = mfn.keras_model(Lc, blocks)

    # 1) rebuild is exact vs the original PyTorch forward
    probe = np.random.default_rng(1).integers(0, 256, (112, 112, 3)).astype(np.uint8)
    ref = mfn.reference(sd, probe)
    for name, m in (("plain", plain), ("cle", cle)):
        k = m(probe[None].astype(np.float32)).numpy()[0]
        rel = np.abs(k - ref).max() / np.abs(ref).max()
        print(f"rebuild check ({name}): max rel diff {rel:.2e}")
        if rel > 1e-4:
            sys.exit("Keras rebuild does not match PyTorch reference")

    cal = calibration(a.calib_dir)
    held = cal[::7].astype(np.uint8)                  # held-out style check vs float
    Ef = plain(held.astype(np.float32)).numpy()
    Ef /= np.linalg.norm(Ef, axis=1, keepdims=True)

    for tag, model, per_tensor, floor in (("dlpu", cle, True, 0.93), ("cpu", plain, False, 0.98)):
        tfl = quantize(model, cal, per_tensor)
        Eq, (scale, zp), n_axis = int8_embed(tfl, held)
        cos = (Ef * Eq).sum(1)
        print(f"embed_{tag}: {'per-tensor' if per_tensor else 'per-channel'}, per-axis tensors {n_axis}, "
              f"float-vs-INT8 cosine min {cos.min():.4f} mean {cos.mean():.4f}, {len(tfl) // 1024} KB")
        if per_tensor and n_axis:
            sys.exit("per-axis quantisation found in DLPU model")
        if cos.mean() < floor:
            sys.exit(f"embed_{tag} INT8 accuracy below {floor}")
        open(os.path.join(a.out, f"embed_{tag}.tflite"), "wb").write(tfl)
        with open(os.path.join(a.out, f"embed_{tag}.meta"), "w") as f:
            f.write("# AURIX embedder meta v1 - generated by tools/convert_mobilefacenet.py; do not edit\n")
            f.write(f"model mobilefacenet {'per-tensor-int8 safe-cle' if per_tensor else 'per-channel-int8'}\n")
            f.write("input 112 112 rgb uint8\n")
            f.write(f"output 128 int8 {scale:.9g} {zp}\n")
    print(f"wrote embed_dlpu.* and embed_cpu.* to {a.out}")


if __name__ == "__main__":
    main()
