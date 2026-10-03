#!/usr/bin/env python3
"""Builds the release Pharos reads: the packages under clean names, latest.json (what to install),
aurix-compat.json (what it runs on) and SHA256SUMS - all written to --out, ready to attach to a GitHub release.

  python3 tools/release_meta.py --eaps DIR --repo owner/name --tag v0.8.1 --out DIR

Fails (exit 1) if the tag does not match the version in the manifests, the two manifests disagree, or a package
for an architecture listed in release/compat.json is missing.
"""
import argparse, datetime, glob, hashlib, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def manifest_version(arch):
    m = json.load(open(os.path.join(ROOT, "app", f"manifest.json.{arch}")))
    return m["acapPackageConf"]["setup"]["version"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eaps", required=True, help="directory searched (recursively) for *.eap")
    ap.add_argument("--repo", required=True, help="owner/name on GitHub")
    ap.add_argument("--tag", required=True, help="release tag, e.g. v0.8.1")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    compat = json.load(open(os.path.join(ROOT, "release", "compat.json")))
    archs = [p["arch"] for p in compat["packages"]]
    versions = {arch: manifest_version(arch) for arch in archs}
    if len(set(versions.values())) != 1:
        sys.exit(f"manifests disagree on the version: {versions}")
    version = next(iter(versions.values()))
    if a.tag.lstrip("v") != version:
        sys.exit(f"tag {a.tag} does not match the manifest version {version}")

    eaps = glob.glob(os.path.join(a.eaps, "**", "*.eap"), recursive=True)
    os.makedirs(a.out, exist_ok=True)
    base = f"https://github.com/{a.repo}/releases/download/{a.tag}"
    packages, sums = [], []
    for p in compat["packages"]:
        found = [e for e in eaps if re.search(rf"_{p['arch']}\.eap$", os.path.basename(e))]
        if len(found) != 1:
            sys.exit(f"expected one .eap for {p['arch']}, found {len(found)}: {found}")
        data = open(found[0], "rb").read()
        name = f"aurix-{version}-{p['arch']}.eap"               # clean, URL-safe asset name
        open(os.path.join(a.out, name), "wb").write(data)
        sha = hashlib.sha256(data).hexdigest()
        sums.append(f"{sha}  {name}")
        packages.append({"arch": p["arch"], "file": name, "url": f"{base}/{name}", "sha256": sha, "size": len(data),
                         "minAxisOs": p["minAxisOs"], "maxAxisOs": p["maxAxisOs"], "chips": p["chips"]})

    released = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    latest = {"app": "aurix", "schema": 1, "version": version, "tag": a.tag, "released": released, "signed": False,
              "packages": packages, "compat": f"{base}/aurix-compat.json",
              "notes": "Unsigned development build: installs on AXIS OS 11 and 12, not on AXIS OS 13."}
    compat_out = dict(compat, version=version)
    json.dump(latest, open(os.path.join(a.out, "latest.json"), "w"), indent=2)
    json.dump(compat_out, open(os.path.join(a.out, "aurix-compat.json"), "w"), indent=2)
    open(os.path.join(a.out, "SHA256SUMS"), "w").write("\n".join(sums) + "\n")
    print(f"AURIX {version}: {', '.join(p['file'] for p in packages)}")


if __name__ == "__main__":
    main()
