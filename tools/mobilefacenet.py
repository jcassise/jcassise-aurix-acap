"""MobileFaceNet (foamliu/MobileFaceNet, Apache-2.0 code) rebuilt from its PyTorch state_dict as
a Keras NHWC model - no torch needed.

- BatchNorm folded into each conv; ImageNet normalisation folded into conv1, with a mean-colour
  offset in front so zero padding matches PyTorch's (padding of the normalised image).
- Optional "safe" cross-layer equalisation (DFQ, Nagel et al. 2019) on junctions that are ReLU or
  linear in the original net (ReLU6 untouched), which helps per-tensor INT8 for the DLPU.
- `reference()` re-implements the original PyTorch forward in numpy for verification.
"""
import copy
import numpy as np

EPS = 1e-5
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)
SETTING = [[2, 64, 5, 2], [4, 128, 1, 2], [2, 128, 6, 1], [4, 128, 1, 2], [2, 128, 2, 1]]


def fold(sd, conv, bn):
    W = sd[conv + ".weight"].astype(np.float64)
    b = sd[conv + ".bias"].astype(np.float64) if conv + ".bias" in sd else np.zeros(W.shape[0])
    if bn:
        g, beta, mu, var = (sd[f"{bn}.{k}"].astype(np.float64) for k in ("weight", "bias", "running_mean", "running_var"))
        s = g / np.sqrt(var + EPS)
        W = W * s.reshape(-1, 1, 1, 1)
        b = (b - mu) * s + beta
    return W, b


def specs(sd):
    L, blocks = [], []

    def add(conv, bn, k, s, act, dw=False, pad=None):
        W, b = fold(sd, conv, bn)
        L.append(dict(W=W, b=b, k=k, s=s, dw=dw, act=act, pad=(k - 1) // 2 if pad is None else pad))

    add("conv1.0", "conv1.1", 3, 2, "relu6")
    add("dw_conv.depthwise", "dw_conv.bn1", 3, 1, "relu", dw=True)
    add("dw_conv.pointwise", "dw_conv.bn2", 1, 1, "relu")
    i, cin = 0, 64
    for t, c, n, s in SETTING:
        for j in range(n):
            st = s if j == 0 else 1
            p = f"features.{i}.conv"
            start = len(L)
            add(p + ".0.0", p + ".0.1", 1, 1, "relu6")
            add(p + ".1.0", p + ".1.1", 3, st, "relu6", dw=True)
            add(p + ".2", p + ".3", 1, 1, None)
            blocks.append((start, st == 1 and cin == c))
            cin = c
            i += 1
    add("conv2.0", "conv2.1", 1, 1, "relu6")
    add("gdconv.depthwise", "gdconv.bn", 7, 1, None, dw=True, pad=0)
    add("conv3", "bn", 1, 1, None)
    return L, blocks


def _out_range(l):
    return np.abs(l["W"]).reshape(l["W"].shape[0], -1).max(1)


def _in_range(l):
    W = l["W"]
    if l["dw"]:
        return np.abs(W).reshape(W.shape[0], -1).max(1)
    return np.abs(W).transpose(1, 0, 2, 3).reshape(W.shape[1], -1).max(1)


def safe_cle(L, blocks, iters=6):
    L = copy.deepcopy(L)
    project = {st + 2 for st, _ in blocks}
    pairs = [(x, x + 1) for x in range(len(L) - 1) if x not in project and L[x]["act"] != "relu6"]
    for _ in range(iters):
        for x, y in pairs:
            r1, r2 = _out_range(L[x]), _in_range(L[y])
            s = np.where((r1 > 0) & (r2 > 0), np.sqrt(r1 / np.maximum(r2, 1e-12)), 1.0)
            L[x]["W"] = L[x]["W"] / s.reshape(-1, 1, 1, 1)
            L[x]["b"] = L[x]["b"] / s
            L[y]["W"] = L[y]["W"] * (s.reshape(-1, 1, 1, 1) if L[y]["dw"] else s.reshape(1, -1, 1, 1))
    return L, pairs


def keras_model(L, blocks):
    import tensorflow as tf
    x_in = tf.keras.Input((112, 112, 3), batch_size=1, name="input")
    x = tf.keras.layers.Rescaling(1.0, offset=(-MEAN * 255.0).tolist(), name="center")(x_in)
    resid = dict(blocks)
    block_in = None
    for idx, l in enumerate(L):
        W, b = l["W"], l["b"]
        if idx == 0:
            W = W * (1.0 / (255.0 * STD.astype(np.float64))).reshape(1, 3, 1, 1)
        if idx in resid:
            block_in = x
        if l["pad"]:
            x = tf.keras.layers.ZeroPadding2D(l["pad"])(x)
        if l["dw"]:
            K = tf.keras.layers.DepthwiseConv2D(l["k"], l["s"], "valid")
            x = K(x)
            K.set_weights([W.transpose(2, 3, 0, 1).astype(np.float32), b.astype(np.float32)])
        else:
            K = tf.keras.layers.Conv2D(W.shape[0], l["k"], l["s"], "valid")
            x = K(x)
            K.set_weights([W.transpose(2, 3, 1, 0).astype(np.float32), b.astype(np.float32)])
        if l["act"] == "relu6":
            x = tf.keras.layers.ReLU(6.0)(x)
        elif l["act"] == "relu":
            x = tf.keras.layers.ReLU()(x)
        if resid.get(idx - 2):
            x = tf.keras.layers.Add()([block_in, x])
    x = tf.keras.layers.Reshape((128,), name="embedding")(x)
    return tf.keras.Model(x_in, x)


def _conv_ref(x, W, stride, pad, groups):
    x = np.pad(x, ((0, 0), (pad, pad), (pad, pad)))
    C, H, Wd = x.shape
    O, Ig, k, _ = W.shape
    Ho, Wo = (H - k) // stride + 1, (Wd - k) // stride + 1
    out = np.zeros((O, Ho, Wo))
    og = O // groups
    for g in range(groups):
        xs = x[g * Ig:(g + 1) * Ig]
        cols = np.stack([xs[:, a:a + stride * Ho:stride, b:b + stride * Wo:stride]
                         for a in range(k) for b in range(k)], 1).reshape(Ig * k * k, Ho * Wo)
        out[g * og:(g + 1) * og] = (W[g * og:(g + 1) * og].reshape(og, -1) @ cols).reshape(og, Ho, Wo)
    return out


def reference(sd, img):
    """Original PyTorch forward (NCHW, unfolded BN, explicit normalisation) in numpy."""
    x = ((img / 255.0 - MEAN) / STD).transpose(2, 0, 1).astype(np.float64)

    def bn(x, n):
        g, b, m, v = (sd[f"{n}.{k}"] for k in ("weight", "bias", "running_mean", "running_var"))
        return (x - m[:, None, None]) / np.sqrt(v[:, None, None] + EPS) * g[:, None, None] + b[:, None, None]

    def cv(x, n, s, p, groups=1):
        y = _conv_ref(x, sd[n + ".weight"].astype(np.float64), s, p, groups)
        return y + (sd[n + ".bias"][:, None, None] if n + ".bias" in sd else 0)

    r6 = lambda z: np.clip(z, 0, 6)
    r = lambda z: np.maximum(z, 0)
    x = r6(bn(cv(x, "conv1.0", 2, 1), "conv1.1"))
    x = r(bn(cv(x, "dw_conv.depthwise", 1, 1, 64), "dw_conv.bn1"))
    x = r(bn(cv(x, "dw_conv.pointwise", 1, 0), "dw_conv.bn2"))
    i, cin = 0, 64
    for t, c, n, s in SETTING:
        for j in range(n):
            st = s if j == 0 else 1
            p = f"features.{i}.conv"
            y = r6(bn(cv(x, p + ".0.0", 1, 0), p + ".0.1"))
            y = r6(bn(cv(y, p + ".1.0", st, 1, y.shape[0]), p + ".1.1"))
            y = bn(cv(y, p + ".2", 1, 0), p + ".3")
            x = x + y if (st == 1 and cin == c) else y
            cin = c
            i += 1
    x = r6(bn(cv(x, "conv2.0", 1, 0), "conv2.1"))
    x = bn(cv(x, "gdconv.depthwise", 1, 0, 512), "gdconv.bn")
    x = bn(cv(x, "conv3", 1, 0), "bn")
    return x.reshape(-1)
