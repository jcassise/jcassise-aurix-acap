#!/usr/bin/env python3
"""Build a gallery.bin from enrolment photos, using the SAME pipeline as the camera:
YuNet detect -> 5-point alignment (== app/src/align.c) -> the camera's own embed.tflite.

    people/
      alice/  1.jpg 2.jpg ...
      bob/    1.jpg ...
    python3 tools/enroll.py people/ --model embed_dlpu.tflite --out gallery.bin [--threat bob] [--param]

--param also prints the text for the app's "Gallery" setting (camera web UI -> Apps -> aurix ->
Settings), so no file has to be copied to the camera.

Use embed_dlpu.tflite for ARTPEC-8 cameras and embed_cpu.tflite for ARTPEC-7 (from the CI
"models" artifact): the two variants are not interchangeable. Photos with zero or several faces
are skipped. Each identity's embeddings are averaged and re-normalised.

Galleries are biometric data: obtain consent, keep them out of git, and handle them under the
agreed GDPR/BIPA model.
"""
import argparse, base64, os, struct, sys, tempfile
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_yunet import fetch            # noqa: E402
from faceprep import aligned_faces         # noqa: E402


def load_embedder(path):
    import tensorflow as tf
    it = tf.lite.Interpreter(model_path=path)
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    zp = o["quantization"][1]

    def run(face):
        it.set_tensor(i["index"], face[None].astype(np.uint8))
        it.invoke()
        e = it.get_tensor(o["index"])[0].astype(np.float32) - zp
        return e / np.linalg.norm(e)
    return run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("people")
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", default="gallery.bin")
    ap.add_argument("--min-eye-px", type=float, default=40)
    ap.add_argument("--threat", action="append", default=[], help="identity to mark as threat (repeatable)")
    ap.add_argument("--kind", choices=["dlpu", "cpu"], help="default: from the model file name")
    ap.add_argument("--param", action="store_true", help="print the Gallery setting string")
    a = ap.parse_args()
    kind = a.kind or ("cpu" if "cpu" in os.path.basename(a.model) else "dlpu")
    from PIL import Image, ImageOps
    embed = load_embedder(a.model)
    with tempfile.TemporaryDirectory() as t:
        onnx = os.path.join(t, "yunet.onnx")
        fetch(onnx)
        ids, embs = [], []
        for person in sorted(os.listdir(a.people)):
            d = os.path.join(a.people, person)
            if not os.path.isdir(d):
                continue
            vecs = []
            for f in sorted(os.listdir(d)):
                try:
                    img = np.asarray(ImageOps.exif_transpose(Image.open(os.path.join(d, f))).convert("RGB"))
                except Exception:
                    continue
                faces = aligned_faces(img, onnx_path=onnx)
                if len(faces) != 1:
                    print(f"  skip {person}/{f}: {len(faces)} faces")
                    continue
                vecs.append(embed(faces[0][2]))
            if not vecs:
                print(f"  {person}: no usable photos")
                continue
            v = np.mean(vecs, 0)
            ids.append(person)
            embs.append(v / np.linalg.norm(v))
            print(f"  {person}: {len(vecs)} photo(s)")
    if not ids:
        sys.exit("nobody enrolled")
    E = np.stack(embs)
    q = np.clip(np.rint(E * 127.0), -127, 127).astype(np.int8)
    cats = [1 if ident in a.threat else 0 for ident in ids]
    with open(a.out, "wb") as f:
        f.write(b"AURG")
        f.write(struct.pack("<III", 2, E.shape[1], len(ids)))
        for ident, cat, row in zip(ids, cats, q):
            f.write(ident.encode("utf-8")[:63].ljust(64, b"\0"))
            f.write(bytes([cat]))
            f.write(row.tobytes())
    print(f"wrote {len(ids)} identities ({E.shape[1]}-d, kind {kind}) to {a.out}")
    if a.param:
        entries = []
        for ident, cat, row in zip(ids, cats, q):
            if any(c in ident for c in ",;"):
                sys.exit(f"name may not contain ',' or ';': {ident}")
            entries.append(f"{ident},{'threat' if cat else 'allow'},{kind},{base64.b64encode(row.tobytes()).decode()}")
        print("\nGallery setting:\n" + ";".join(entries))


if __name__ == "__main__":
    main()
