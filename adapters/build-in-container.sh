#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# adapters/build-in-container.sh — run INSIDE the target distro's
# container image: install makedepends, fetch+verify the pinned source,
# prepare/build/check/package (from adapters/<adapter>/build.sh), then
# relocate the staged tree with the pristine placeholder root and pack
# prelinux-adapter-<distroid>-<arch>.tar.gz into $OUTDIR.
#
# Used both by CI stage B (a `container:` job) and by the local wrapper
# scripts/build-adapter.sh.
#
#   env: distroid  (e.g. debian12 — selects the pinned version)
#        OUTDIR    (where the tarball lands, usually a mounted dir)
#        DISTFILE  (optional pre-downloaded source tarball)
#   arg: adapter   (e.g. dpkg | rpm)
set -eu

adapter=${1:?usage: build-in-container.sh <adapter>}
adapterdir=$(cd "$(dirname "$0")/$adapter" && pwd)
repodir=$(cd "$(dirname "$0")/.." && pwd)
: "${distroid:?set distroid}"
: "${OUTDIR:?set OUTDIR}"

# shellcheck disable=SC1090
. "$adapterdir/build.sh"

# optional adapter hook (e.g. rpm enables CRB on RHEL clones)
if command -v preinstall >/dev/null 2>&1; then
  preinstall
fi

export DEBIAN_FRONTEND=noninteractive
if command -v apt-get >/dev/null; then
  apt-get update -qq
  # shellcheck disable=SC2086
  apt-get install -y -qq --no-install-recommends $makedepends >/dev/null
elif command -v dnf >/dev/null; then
  # shellcheck disable=SC2086
  dnf install -y -q $makedepends >/dev/null
elif command -v yum >/dev/null; then
  # shellcheck disable=SC2086
  yum install -y -q $makedepends >/dev/null
elif command -v microdnf >/dev/null; then
  # shellcheck disable=SC2086
  microdnf install $makedepends >/dev/null
elif command -v apk >/dev/null; then
  # shellcheck disable=SC2086
  apk add $makedepends >/dev/null
else
  echo "no known package manager in container" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

distfile=${DISTFILE:-}
if [ -n "$distfile" ] && [ -f "$distfile" ]; then
  cp "$distfile" "$work/src"
  distfile="$work/src"
else
  curl -fsSL --retry 3 -o "$work/src" "$source"
  distfile="$work/src"
fi
echo "$sha256  $distfile" | sha256sum -c -

# minimal images lack some decompressor CLIs (alma tar execs the missing
# lbzip2); pick the decompressor by the known extension
case $source in
  *.tar.bz2)      bzcat "$distfile" | tar -x -C "$work" ;;
  *.tar.xz)       xzcat "$distfile" | tar -x -C "$work" ;;
  *.tar.gz|*.tgz) zcat  "$distfile" | tar -x -C "$work" ;;
  *)              tar -xf "$distfile" -C "$work" ;;
esac

pkgdir="$work/stage"
mkdir -p "$pkgdir"
srcdir="$work"
export srcdir pkgdir adapterdir
prepare
build
check
package

# Relocate against the placeholder root using a CLI
# built right here (the repo may be mounted read-only).  Purge any
# host-built objects first: a local tree may carry foreign-arch .o files.
ph="$(printf '/.prelinux-root-placeholder-'; printf 'A%.0s' $(seq 226))"
[ ${#ph} -eq 254 ] || { echo "placeholder generation broken (got ${#ph} bytes)" >&2; exit 1; }
cp -r "$repodir/prelinux" "$work/cli"
rm -rf "$work/cli"/src/*.o "$work/cli"/prelinux "$work/cli"/libprelinux.so "$work/cli"/obj-cli
# The in-container CLI is only a build-time relocate tool, so it is built
# with the distro's cc and linked dynamically: no need for glibc-static
# (whose aarch64 build in Rocky's CRB currently depends on a glibc-devel
# that BaseOS has not published).  The SHIPPED CLI comes from CI stage A
# with musl-gcc.
make -s -C "$work/cli" MUSLCC=cc STATIC=
"$work/cli/prelinux" relocate --root "$ph" --quiet "$pkgdir"

mkdir -p "$OUTDIR"
arch=$(uname -m)
name="prelinux-adapter-${distroid}-${arch}"
# Hard-link free payload: hmdfs on HarmonyOS PC refuses link(2)
# so an image carrying hard-link entries cannot be
# unpacked there.  GNU tar only; toybox tar has no such flag.
tarflags=""
if tar --version 2>/dev/null | grep -q GNU; then
  tarflags="--hard-dereference"
fi
( cd "$pkgdir" && tar $tarflags -cf - . | gzip -9 > "$OUTDIR/$name.tar.gz" )
sha256sum "$OUTDIR/$name.tar.gz" | sed 's| .*/|  |'
