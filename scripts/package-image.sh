#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# package-image.sh — assemble the single user-facing image (design
# 11.1 stage D): distro rootfs + glibc payload + package manager, fully
# self-contained ("qemu/docker-style": one file per distro).
#
# The base and adapter inputs are already placeholder-relocated tarballs
# (from package-base.sh / build-adapter.sh); the rootfs comes from the
# distro's official source via scripts/fetch-rootfs.sh (provenance +
# sha256 recorded in the manifest).  The CLI ships pre-signed (design
# 6.5) since it is never rewritten at install time.
#
# Usage: package-image.sh <rootfs.tar.gz> <base-tarball> <adapter-tarball|''> <distroid> <outdir>
set -eu

rootfs=$1
base=$2
adapter=$3
distroid=$4
outdir=$5

# absolutize: the packing subshell cds into the staging dir
rootfs=$(cd "$(dirname "$rootfs")" && pwd)/$(basename "$rootfs")
base=$(cd "$(dirname "$base")" && pwd)/$(basename "$base")
[ -z "$adapter" ] || adapter=$(cd "$(dirname "$adapter")" && pwd)/$(basename "$adapter")
outdir=$(mkdir -p "$outdir" && cd "$outdir" && pwd)

arch=${ARCH:-$(uname -m)}   # ARCH may override for cross packaging
case "$arch" in
  x86_64|aarch64) ;;
  *) echo "unsupported arch $arch" >&2; exit 1 ;;
esac
# cross packaging must not silently mix arches: the base tarball name is
# arch-stamped by package-base.sh, refuse a mismatch.
case "$base" in
  *-$arch.tar.gz) ;;
  *) echo "package-image: base tarball '$base' does not match arch '$arch'" >&2; exit 1 ;;
esac

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# Overlay order: rootfs (distro content) <- base <- adapter.  GNU tar flags
# mirror import's (runner runs rootless; the rootfs ships read-only
# dirs on rpm-family distros).
if tar --version 2>/dev/null | grep -q GNU; then
  set -- --overwrite --no-same-permissions --delay-directory-restore
else
  set --
fi
tar -x "$@" -C "$stage" -f "$rootfs"
chmod -R u+rwX "$stage"
tar -x "$@" -C "$stage" -f "$base"
[ -z "$adapter" ] || tar -x "$@" -C "$stage" -f "$adapter"

rootfs_sum=$(sha256sum "$rootfs" | cut -d' ' -f1)
mkdir -p "$stage/prelinux"
cat > "$stage/prelinux/image.toml" <<EOF
# PreLinux image manifest
arch = "$arch"
glibc = "2.39"
built = "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
rootfs-source = "$(cat "$rootfs.source" 2>/dev/null || echo local-file)"
rootfs-sha256 = "$rootfs_sum"
EOF

cat > "$stage/prelinux/NOTICE" <<EOF
PreLinux image for $distroid ($arch)
====================================

This image bundles an official $distroid rootfs snapshot.  The distro
content is redistributed under its upstream licenses (see each package's
copyright notices inside the instance, e.g. /usr/share/doc/*/copyright
on dpkg-family distros; corresponding source packages are available from
the distro's official source archives).  The rootfs snapshot used is
recorded with its sha256 in image.toml.

PreLinux's own components (glibc patchset, CLI, adapters) are
GPL-3.0-or-later; the vendored ohos-selfsign stays 0BSD (attribution
kept in its header).

The glibc HarmonyOS-kernel compatibility patches are part of PreLinux's own
patch set (glibc/patches, GPL-3.0-or-later) and do nothing on a Linux
kernel.  PreLinux's patches to upstream packages are published with the
release.


The prelinux CLI statically links musl (https://musl.libc.org):
Copyright 2005-2020 Rich Felker <dalias@aerifal.cx> and other
contributors; MIT license (see /usr/share/doc/musl-tools/copyright at
build time for the full text).
EOF

# The same notice must be readable from inside the instance: the prelinux
# directory is installer bookkeeping, not part of the FHS prefix table, so
# a copy goes to the usual documentation location.
mkdir -p "$stage/usr/share/doc/prelinux"
cp "$stage/prelinux/NOTICE" "$stage/usr/share/doc/prelinux/NOTICE"

# verify the CLI is signed (relocate signs aarch64 ELFs).  aarch64-only:
# the signature matters on HarmonyOS PC (aarch64) and the relocate gate
# leaves x86_64 binaries unsigned, so checking those would fail spuriously.
# The checker is the vendored selfsign tool compiled on the spot, not the
# shipped CLI, so the CLI does not carry a verifier of its own.
if [ "$arch" = aarch64 ]; then
  here=$(cd "$(dirname "$0")" && pwd)
  repo=$(dirname "$here")
  if command -v cc >/dev/null 2>&1; then
    tmpdir=$(mktemp -d)
    if cc -O2 -o "$tmpdir/selfsign" "$repo/prelinux/src/selfsign.c"
    then
      "$tmpdir/selfsign" --check "$stage/prelinux/bin/prelinux"
    else
      echo "package-image: cannot build the signature checker" >&2
      rm -rf "$tmpdir"
      exit 1
    fi
    rm -rf "$tmpdir"
  else
    echo "package-image: no cc, skipping the CLI signature check" >&2
  fi
fi

name="prelinux-image-$distroid-$arch"
# Images must be hard-link free.  HarmonyOS PC's home filesystem (hmdfs)
# refuses link(2) with EPERM, so a tarball carrying hard-link entries
# cannot be unpacked there — the import dies mid-extraction (discovered
# on real hardware, 2026-10).  --hard-dereference stores every hard link
# as an independent copy of the same bytes: the instance is byte-for-byte
# equivalent content-wise, only the link-count optimisation is lost.
# GNU tar only (the CI runners and packaging hosts); toybox tar has no
# such flag and is not used for packaging.
tarflags=""
if tar --version 2>/dev/null | grep -q GNU; then
  tarflags="--hard-dereference"
else
  echo "package-image: warning: non-GNU tar, image may contain hard links" >&2
fi
# One published format: tar.gz.  gzip is the only compressor we can assume
# on a stock HarmonyOS PC (no zstd in the system image), and a single
# artifact keeps the release, the installer and the docs on one path.
# zstd images are ~35% smaller; import still accepts them for anyone who
# compresses locally, but we do not publish them.
( cd "$stage" && tar $tarflags -cf - . | gzip -9 > "$outdir/$name.tar.gz" )
sha256sum "$outdir/$name.tar.gz" | sed 's| .*/|  |'
