#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# build-adapter.sh — local wrapper: build a package-manager adapter inside
# the target distro's official container image.
# All build logic lives in adapters/build-in-container.sh, which CI
# stage B shares; this wrapper only seeds the distfile cache and runs
# the container.
#
# Usage: build-adapter.sh <adapter> <image> <distroid> <outdir>
# Example: build-adapter.sh dpkg debian:12 debian12 out/
set -eu

adapter=$1
image=$2
distroid=$3
outdir=$(mkdir -p "$4" && cd "$4" && pwd)

repo=$(cd "$(dirname "$0")/.." && pwd)

# Resolve the pinned source (via the adapter's version table) and seed
# the host-side distfile cache so repeated builds skip the download.
export distroid
# shellcheck disable=SC1090
. "$repo/adapters/$adapter/build.sh"
mkdir -p "$repo/.cache/distfiles"
distfile="$repo/.cache/distfiles/$(basename "$source")"
if [ ! -f "$distfile" ]; then
  curl -fsSL --retry 3 -o "$distfile" "$source"
fi
echo "$sha256  $distfile" | sha256sum -c -

docker run --rm ${PLATFORM:+--platform "$PLATFORM"} \
  -e distroid="$distroid" \
  -e OUTDIR=/out \
  -e DISTFILE=/distfile \
  -v "$repo:/prelinux:ro" \
  -v "$distfile:/distfile:ro" \
  -v "$outdir:/out" \
  "$image" sh /prelinux/adapters/build-in-container.sh "$adapter"
