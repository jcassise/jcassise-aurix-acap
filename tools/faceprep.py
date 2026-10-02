"""AURIX tools - Python mirror of the camera pipeline: YuNet detect -> 5-pt similarity align (== align.c) -> 112x112 RGB."""
import numpy as np, onnxruntime as ort
from PIL import Image
T112 = np.array([[38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366], [41.5493, 92.3655], [70.7299, 92.2041]], np.float32)
_det = None

def detect(img, thr=0.6, onnx_path=None):
    """onnx_path: original YuNet ONNX (BGR input), e.g. fetched by convert_yunet.fetch()."""
    global _det
    if _det is None: _det = ort.InferenceSession(onnx_path)
    H, W = img.shape[:2]; s = 640 / max(H, W)
    w, h = int(round(W * s / 32)) * 32 or 32, int(round(H * s / 32)) * 32 or 32
    r = np.asarray(Image.fromarray(img).resize((w, h), Image.BILINEAR))
    outs = dict(zip([o.name for o in _det.get_outputs()], _det.run(None, {"input": r[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32)})))
    from convert_yunet import decode
    maps = {}
    for st in (8, 16, 32):
        for role in ("cls", "obj", "bbox", "kps"):
            a = outs[f"{role}_{st}"][0]; maps[(role, st)] = a.reshape(h // st, w // st, -1)
    dets = decode(maps, thr)
    sx, sy = W / w, H / h
    return [(d[0], np.array([d[1] * sx, d[2] * sy, d[3] * sx, d[4] * sy]), d[5] * [sx, sy]) for d in dets]

def similarity(lm):
    P = T112.astype(np.float64); Q = lm.astype(np.float64)
    pc, qc = P.mean(0), Q.mean(0); p, q = P - pc, Q - qc
    den = (p ** 2).sum(); a = (p[:, 0] * q[:, 0] + p[:, 1] * q[:, 1]).sum() / den; b = (p[:, 0] * q[:, 1] - p[:, 1] * q[:, 0]).sum() / den
    return np.array([a, -b, qc[0] - (a * pc[0] - b * pc[1]), b, a, qc[1] - (b * pc[0] + a * pc[1])])

def warp(img, m, size=112):
    v, u = np.mgrid[0:size, 0:size].astype(np.float64)
    x = m[0] * u + m[1] * v + m[2]; y = m[3] * u + m[4] * v + m[5]
    H, W = img.shape[:2]; out = np.zeros((size, size, 3), np.float64)
    inside = (x > -1) & (y > -1) & (x < W) & (y < H)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx, fy = x - x0, y - y0
    x1, y1 = np.clip(x0 + 1, 0, W - 1), np.clip(y0 + 1, 0, H - 1); x0, y0 = np.clip(x0, 0, W - 1), np.clip(y0, 0, H - 1)
    f = img.astype(np.float64)
    a = f[y0, x0] + fx[..., None] * (f[y0, x1] - f[y0, x0]); b = f[y1, x0] + fx[..., None] * (f[y1, x1] - f[y1, x0])
    out = np.rint(a + fy[..., None] * (b - a)); out[~inside] = 0
    return np.clip(out, 0, 255).astype(np.uint8)

def aligned_faces(img, thr=0.6, onnx_path=None):
    return [(sc, box, warp(img, similarity(lm))) for sc, box, lm in detect(img, thr, onnx_path)]
