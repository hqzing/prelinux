#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# rootfs-oci.sh — flatten an OCI-layout image tar into a plain rootfs
# tarball, WITHOUT docker (appendix A: rootfs
# provenance).  Reads index.json -> manifest -> layer blobs, verifies
# each layer's sha256, applies layers in order, handling whiteouts.
#
# Usage: rootfs-oci.sh <oci.tar[.xz|.gz]> <out.tar.gz>
set -eu

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/oci"
tar -xf "$1" -C "$work/oci"

python3 - "$work/oci" "$2" <<'EOF'
import hashlib, json, os, shutil, sys, tarfile

ocidir, out = sys.argv[1], sys.argv[2]

idx = json.load(open(f"{ocidir}/index.json"))
manifests = idx["manifests"]
if len(manifests) != 1:
    sys.exit(f"rootfs-oci: expected exactly 1 manifest, got {len(manifests)}")
m_digest = manifests[0]["digest"].split(":", 1)[1]
manifest = json.load(open(f"{ocidir}/blobs/sha256/{m_digest}"))
layers = [l["digest"].split(":", 1)[1] for l in manifest["layers"]]

stage = os.path.join(ocidir, "rootfs")
os.mkdir(stage)

def verify(path, want):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != want:
        sys.exit(f"rootfs-oci: layer sha256 mismatch: {want}")

# distros ship non-writable entries (RHEL's /etc/gshadow is 0000);
# force owner-writable modes so non-root extraction and later overlay
# operations work (import hardens the tree itself, this is build-side).
def writable_filter(m, path):
    if m.isdir():
        m.mode = m.mode | 0o700
    elif m.isreg():
        m.mode = m.mode | 0o600
    return m

for digest in layers:
    blob = f"{ocidir}/blobs/sha256/{digest}"
    verify(blob, digest)
    with tarfile.open(blob) as tf:
        # whiteouts: .wh.<name> deletes <name>; .wh..wh..opq clears the dir
        members = tf.getmembers()
        for m in members:
            base = os.path.basename(m.name)
            if base.startswith(".wh."):
                target = os.path.join(stage, os.path.dirname(m.name),
                                      base[4:] if base != ".wh..wh..opq" else "")
                if base == ".wh..wh..opq":
                    for e in os.listdir(target):
                        p = os.path.join(target, e)
                        shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
                elif os.path.isdir(target):
                    shutil.rmtree(target)
                elif os.path.exists(target):
                    os.unlink(target)
        tf.extractall(stage, filter=writable_filter,
                          members=[m for m in members
                                   if not os.path.basename(m.name).startswith(".wh.")])

with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as tf:
    tf.add(stage, arcname=".")
print(f"rootfs-oci: flattened {len(layers)} layer(s) -> {out}")
EOF
