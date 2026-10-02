#!/usr/bin/env python3
"""Convert YuNet (libfacedetection.train, BSD-3 code; WIDER FACE-trained weights =
INTERNAL BENCHMARK ONLY) into an ARTPEC-friendly TFLite model + sidecar meta.

Steps: download pinned ONNX (sha256-checked) -> fix input size -> swap BGR->RGB in the
first conv -> cut Transpose/Reshape tails (outputs = raw NHWC head maps, sigmoid kept)
-> onnx2tf + TF converter: full INT8, PER-TENSOR, uint8 input, int8 outputs ->
map outputs to roles -> validate INT8 vs float -> write detect.tflite + detect.meta.

    pip install -r tools/requirements-models.txt
    python3 tools/convert_yunet.py --out app/models [--calib-dir frames/] [--size 640x352]

--calib-dir: folder of real camera JPEG/PNG frames (recommended). Without it a synthetic
calibration set is built from scikit-image sample photos, which is fine for latency
benchmarking but not for accuracy work.
"""
import argparse, glob, hashlib, os, shutil, subprocess, sys, tempfile, urllib.request
import numpy as np

ONNX_URL = ("https://raw.githubusercontent.com/ShiqiYu/libfacedetection.train/"
            "master/onnx/yunet_n_dynamic.onnx")
ONNX_SHA256 = "104ee26c71d5c270ce79a6b3dced91c91013260d29a6bf18f9296c5b8c4c2b12"
STRIDES = (8, 16, 32)


def fetch(dst):
    urllib.request.urlretrieve(ONNX_URL, dst)
    h = hashlib.sha256(open(dst, "rb").read()).hexdigest()
    if h != ONNX_SHA256:
        sys.exit(f"sha256 mismatch for YuNet ONNX: {h}")


def surgery(src, dst, W, H):
    import onnx
    from onnx import helper, numpy_helper, shape_inference
    m = onnx.load(src); g = m.graph
    d = g.input[0].type.tensor_type.shape.dim
    d[0].dim_value, d[2].dim_value, d[3].dim_value = 1, H, W
    first = next(n for n in g.node if g.input[0].name in n.input)
    init = {t.name: t for t in g.initializer}
    w = numpy_helper.to_array(init[first.input[1]])
    init[first.input[1]].CopyFrom(numpy_helper.from_array(w[:, ::-1].copy(), first.input[1]))
    prod = {o: n for n in g.node for o in n.output}
    outs = []
    for o in list(g.output):
        n = prod[o.name]
        while n.op_type != "Conv":
            n = prod[n.input[0]]
        name = o.name + "_map"
        op = "Sigmoid" if o.name.startswith(("cls", "obj")) else "Identity"
        g.node.append(helper.make_node(op, [n.output[0]], [name], name=op + "_" + name))
        outs.append(name)
    m = shape_inference.infer_shapes(m)
    tmp = dst + ".tmp.onnx"; onnx.save(m, tmp)
    onnx.utils.extract_model(tmp, dst, [g.input[0].name], outs); os.remove(tmp)
    m = onnx.load(dst)
    for o in m.graph.output:
        o.type.tensor_type.shape.dim[0].dim_value = 1
    onnx.checker.check_model(m); onnx.save(m, dst)


def calib_set(W, H, calib_dir, n=120):
    from PIL import Image
    if calib_dir:
        files = sorted(glob.glob(os.path.join(calib_dir, "*.[jJpP][pPnN]*[gG]")))[:n]
        if not files:
            sys.exit(f"no images in {calib_dir}")
        return np.stack([np.asarray(Image.open(f).convert("RGB").resize((W, H), Image.BILINEAR))
                         for f in files]).astype(np.float32)
    from skimage import data
    rng = np.random.default_rng(0)
    face = data.astronaut()
    bgs = [data.coffee(), data.chelsea(), data.rocket(), data.immunohistochemistry(),
           np.stack([data.camera()] * 3, -1), data.astronaut()]
    out = []
    for i in range(n):
        bg = Image.fromarray(bgs[i % len(bgs)]).resize((W, H), Image.BILINEAR)
        for _ in range(rng.integers(1, 4)):
            s = int(rng.integers(40, min(300, H)))
            f = Image.fromarray(np.clip(face * rng.uniform(0.5, 1.3), 0, 255).astype(np.uint8)).resize((s, s))
            bg.paste(f, (int(rng.integers(0, max(1, W - s))), int(rng.integers(0, max(1, H - s)))))
        out.append(np.asarray(bg))
    return np.stack(out).astype(np.float32)


def decode(maps, thr=0.6, nms=0.3):
    dets = []
    for st in STRIDES:
        cls, obj, bb, kp = (maps[(r, st)] for r in ("cls", "obj", "bbox", "kps"))
        sc = np.sqrt(np.clip(cls[..., 0], 0, 1) * np.clip(obj[..., 0], 0, 1))
        for r, c in zip(*np.nonzero(sc >= thr)):
            cx, cy = (c + bb[r, c, 0]) * st, (r + bb[r, c, 1]) * st
            w, h = np.exp(bb[r, c, 2]) * st, np.exp(bb[r, c, 3]) * st
            k = np.array([((kp[r, c, 2 * j] + c) * st, (kp[r, c, 2 * j + 1] + r) * st) for j in range(5)])
            dets.append((float(sc[r, c]), cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, k))
    dets.sort(key=lambda d: -d[0]); keep = []
    for d in dets:
        if all(iou(d, e) <= nms for e in keep):
            keep.append(d)
    return keep


def iou(a, b):
    ix = max(0, min(a[3], b[3]) - max(a[1], b[1])); iy = max(0, min(a[4], b[4]) - max(a[2], b[2]))
    u = (a[3] - a[1]) * (a[4] - a[2]) + (b[3] - b[1]) * (b[4] - b[2]) - ix * iy
    return ix * iy / u if u > 0 else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", default="640x352", help="WxH, multiples of 32")
    ap.add_argument("--calib-dir")
    a = ap.parse_args()
    W, H = map(int, a.size.lower().split("x"))
    if W % 32 or H % 32:
        sys.exit("size must be multiples of 32")
    os.makedirs(a.out, exist_ok=True)
    import onnxruntime as ort
    import tensorflow as tf

    with tempfile.TemporaryDirectory() as t:
        raw, mod = os.path.join(t, "yunet.onnx"), os.path.join(t, "yunet_rgb.onnx")
        fetch(raw); surgery(raw, mod, W, H)
        calib = calib_set(W, H, a.calib_dir)
        cpath = os.path.join(t, "calib.npy"); np.save(cpath, calib)
        tfdir = os.path.join(t, "tf")
        subprocess.run(["onnx2tf", "-i", mod, "-o", tfdir, "-tb", "tf_converter", "-oiqt",
                        "-qt", "per-tensor", "-iqd", "uint8", "-oqd", "int8", "-n",
                        "-cind", "input", cpath, "[[[[0.,0.,0.]]]]", "[[[[1.,1.,1.]]]]"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tfl = glob.glob(os.path.join(tfdir, "*_full_integer_quant.tflite"))[0]

        q = tf.lite.Interpreter(model_path=tfl); q.allocate_tensors()
        ind = q.get_input_details()[0]
        assert ind["dtype"] == np.uint8 and list(ind["shape"]) == [1, H, W, 3], ind
        per_axis = [x for x in q.get_tensor_details() if len(x["quantization_parameters"]["scales"]) > 1]
        assert not per_axis, "per-axis quantisation found"
        sess = ort.InferenceSession(mod)
        names = [o.name for o in sess.get_outputs()]

        def float_maps(img):
            r = sess.run(None, {"input": img.transpose(2, 0, 1)[None].astype(np.float32)})
            return {n: x[0].transpose(1, 2, 0) for n, x in zip(names, r)}

        def int8_maps(img):
            q.set_tensor(ind["index"], img[None].astype(np.uint8)); q.invoke()
            return [(d, (q.get_tensor(d["index"])[0].astype(np.float32) - d["quantization"][1])
                     * d["quantization"][0]) for d in q.get_output_details()]

        # map tflite outputs -> (role, stride) by correlation with the float model
        probe = calib[0].astype(np.uint8)
        ref = float_maps(probe); roles = []
        for d, deq in int8_maps(probe):
            cands = [n for n in names if ref[n].shape == deq.shape]
            best = max(cands, key=lambda n: np.corrcoef(ref[n].ravel(), deq.ravel())[0, 1])
            roles.append((best.split("_")[0], int(best.split("_")[1]), d["quantization"]))
        if len({(r, s) for r, s, _ in roles}) != 12:
            sys.exit("output role mapping not unique")

        # validate INT8 vs float at detection level
        matched = total = 0; kerr = []
        for img in calib[::10].astype(np.uint8):
            fd = decode({(n.split("_")[0], int(n.split("_")[1])): v for n, v in float_maps(img).items()})
            qd = decode({(r, s): v for (r, s, _), (_, v) in zip(roles, int8_maps(img))})
            total += len(fd)
            for d in fd:
                b = max(qd, key=lambda e: iou(d, e), default=None)
                if b is not None and iou(d, b) > 0.5:
                    matched += 1
                    kerr.append(np.abs(d[5] - b[5]).mean() / max(1e-6, np.hypot(*(d[5][1] - d[5][0]))))
        print(f"validation: {matched}/{total} float faces matched by INT8; "
              f"landmark err/eye-dist mean {np.mean(kerr) if kerr else float('nan'):.3f}")
        if total and matched / total < 0.9:
            sys.exit("INT8 model lost too many faces - check calibration data")

        shutil.copy(tfl, os.path.join(a.out, "detect.tflite"))
        with open(os.path.join(a.out, "detect.meta"), "w") as f:
            f.write("# AURIX detector meta v1 - generated by tools/convert_yunet.py; do not edit\n")
            f.write("model yunet_n per-tensor-int8\n")
            f.write(f"input {W} {H} rgb uint8\n")
            for k, (r, s, (sc, zp)) in enumerate(roles):
                f.write(f"out {k} {r} {s} {sc:.9g} {zp}\n")
    print(f"wrote {a.out}/detect.tflite and detect.meta ({W}x{H})")


if __name__ == "__main__":
    main()
