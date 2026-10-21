#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# test-sync.sh — the SHARED CORE of the translation layer must be
# byte-identical between the glibc patch and the base SDK (
# the mapping rules have a single implementation).
#
# Usage: test-sync.sh <repo-root>
set -eu
repo=$1

extract() {
  sed -n '/BEGIN SHARED CORE/,/END SHARED CORE/p' "$1"
}

# The glibc side lives inside the 0001 patch as include/prelinux-translate.h
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Extract the patch's copy of the header (lines added to
# include/prelinux-translate.h).
awk '/^--- \/dev\/null$/{f=0} /^\+\+\+ b\/include\/prelinux-translate.h/{f=1; next} f && /^\+/ {print substr($0,2)} f && /^---/{f=0}' \
  "$repo/glibc/patches/0007-translate-core.patch" > "$tmp/glibc-side.h"
if [ ! -s "$tmp/glibc-side.h" ]; then
  echo "test-sync: could not extract glibc-side header from patch" >&2
  exit 1
fi

extract "$repo/prelinux/src/prelinux-translate.h" > "$tmp/base-core.txt"
extract "$tmp/glibc-side.h" > "$tmp/glibc-core.txt"
if ! diff "$tmp/base-core.txt" "$tmp/glibc-core.txt" >/dev/null; then
  echo "test-sync: SHARED CORE drifted between prelinux/src and glibc patch:" >&2
  diff -u "$tmp/base-core.txt" "$tmp/glibc-core.txt" | head -40
  exit 1
fi
echo "test-sync: OK (shared core identical)"
