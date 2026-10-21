# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# PreLinux base SDK packaging — assemble prelinux-base-<arch>.tar.gz
# from an installed glibc DESTDIR and the built base SDK.
#
# Usage: package-base.sh <glibc-destdir> <base-dir> <outdir>
set -eu

destdir=$1
basedir=$2
outdir=$3
arch="$(uname -m)"
[ "$arch" = "x86_64" ] || [ "$arch" = "aarch64" ] || {
  echo "unsupported arch $arch" >&2; exit 1; }

# absolutize: the packing subshell cds into the staging dir
destdir=$(cd "$destdir" && pwd)
basedir=$(cd "$basedir" && pwd)
outdir=$(mkdir -p "$outdir" && cd "$outdir" && pwd)

ph="$(printf '/.prelinux-root-placeholder-'; printf 'A%.0s' $(seq 226))"
[ ${#ph} -eq 254 ] || { echo "placeholder generation broken (got ${#ph} bytes)" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# 1. glibc install tree -> glibc (strip the placeholder prefix);
#    copy, don't move, so the DESTDIR stays reusable for reruns.
cp -a "$destdir/$ph/glibc" "$stage/glibc"
# drop development artifacts users don't need at runtime? keep: cheap.

# 2. base SDK
mkdir -p "$stage/prelinux/bin"
cp "$basedir/prelinux" "$stage/prelinux/bin/prelinux"
cp "$basedir/libprelinux.so" "$stage/glibc/lib/libprelinux.so"

# 3. relocate the base tree itself with the pristine placeholder as the
#    root (glibc programs got the placeholder interp from the build,
#    which is exactly right; this pass additionally normalizes RPATHs).
"$stage/prelinux/bin/prelinux" relocate --root "$ph" --quiet "$stage"

# NOTE: no FHS skeleton dirs (bin, lib, ...) are shipped here: usr-merged
# distros ship them as symlinks and tar would clobber the links with
# empty directories.  `prelinux import` creates the skeleton with
# mkdir -p (a no-op over existing symlinks) after unpacking the rootfs.

mkdir -p "$outdir"
# Hard-link free payload (HarmonyOS PC's hmdfs refuses link(2); see
# scripts/package-image.sh).  GNU tar only.
tarflags=""
if tar --version 2>/dev/null | grep -q GNU; then
  tarflags="--hard-dereference"
fi
# gzip only, matching the published images (see package-image.sh).
( cd "$stage" && tar $tarflags -cf - . | gzip -9 > "$outdir/prelinux-base-$arch.tar.gz" )
sha256sum "$outdir"/prelinux-base-$arch.tar.gz | sed 's| .*/|  |'
