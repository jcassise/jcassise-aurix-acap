#!/usr/bin/env python3
"""Build an AURIX gallery.bin from enrolment embeddings.

Input: a .npz with `ids` (N strings) and `emb` (N x D float32), produced by running the
SAME embedding model used on the camera over enrolment photos.

    python3 tools/make_gallery.py enrol.npz gallery.bin

Galleries are biometric data: keep them out of git and handle under the consent/GDPR/BIPA
model agreed with legal.
"""
import struct
import sys

import numpy as np


def main(src: str, dst: str) -> None:
    data = np.load(src, allow_pickle=False)
    ids, emb = list(data["ids"]), np.asarray(data["emb"], dtype=np.float32)
    if emb.ndim != 2 or len(ids) != emb.shape[0]:
        sys.exit("ids/emb shape mismatch")
    n, d = emb.shape
    norms = np.linalg.norm(emb, axis=1, keepdims=True)
    if np.any(norms == 0):
        sys.exit("zero embedding found")
    q = np.clip(np.rint(emb / norms * 127.0), -127, 127).astype(np.int8)

    with open(dst, "wb") as f:
        f.write(b"AURG")
        f.write(struct.pack("<III", 1, d, n))
        for i, ident in enumerate(ids):
            raw = str(ident).encode("utf-8")
            if len(raw) > 63:
                sys.exit(f"id too long (max 63 bytes): {ident}")
            f.write(raw.ljust(64, b"\0"))
            f.write(q[i].tobytes())
    print(f"wrote {n} identities, dim {d}, {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
