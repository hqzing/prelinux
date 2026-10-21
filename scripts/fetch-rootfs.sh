#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# fetch-rootfs.sh — acquire a distro rootfs tarball from its official
# source, checksum-verified (appendix A: rootfs
# provenance).  No docker involved: ubuntu/debian ship flat tarballs,
# rocky ships an OCI layout tar flattened by scripts/rootfs-oci.sh.
#
# Usage: fetch-rootfs.sh <distroid> <arch> <outdir>
# Prints the rootfs tarball path on stdout (last line).
set -eu

distroid=$1
arch=$2
outdir=$(mkdir -p "$3" && cd "$3" && pwd)
repo=$(cd "$(dirname "$0")/.." && pwd)

out="$outdir/$distroid-$arch.rootfs.tar.gz"
if [ -f "$out" ]; then
  echo "$out"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { curl -fsSL --retry 3 -o "$1" "$2"; }

case "$distroid-$arch" in
  ubuntu2404-aarch64)
    fetch "$work/u" "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-arm64.tar.gz"
    echo "a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2  $work/u" | sha256sum -c -
    mv "$work/u" "$out"
    echo "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-arm64.tar.gz" > "$out.source"
    ;;
  ubuntu2404-x86_64)
    fetch "$work/u" "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-amd64.tar.gz"
    echo "e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86  $work/u" | sha256sum -c -
    mv "$work/u" "$out"
    echo "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-amd64.tar.gz" > "$out.source"
    ;;
  debian12-aarch64)
    fetch "$work/d" "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/cf1f4a45447842b45e9952e0f018ae734a7341c7/bookworm/slim/oci/blobs/rootfs.tar.gz"
    echo "c75f989a229d12b2d2613a5997de9ff3546f664c22da9248720033a2410220f6  $work/d" | sha256sum -c -
    mv "$work/d" "$out"
    echo "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/cf1f4a45447842b45e9952e0f018ae734a7341c7/bookworm/slim/oci/blobs/rootfs.tar.gz" > "$out.source"
    ;;
  debian12-x86_64)
    fetch "$work/d" "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/a3751996111ce2c1c904050c84440184535e23b1/bookworm/slim/oci/blobs/rootfs.tar.gz"
    echo "7fd1ebe418b5079d4aa36f416af9b143d127bc75b0155d71cb5fd559b5e04d39  $work/d" | sha256sum -c -
    mv "$work/d" "$out"
    echo "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/a3751996111ce2c1c904050c84440184535e23b1/bookworm/slim/oci/blobs/rootfs.tar.gz" > "$out.source"
    ;;
  rocky10-aarch64|rocky10-x86_64)
    [ "$arch" = aarch64 ] && rarch=aarch64 || rarch=x86_64
    oci="$work/oci.tar.xz"
    fetch "$oci" "https://dl.rockylinux.org/pub/rocky/10/images/$rarch/Rocky-10-Container-Base.latest.$rarch.tar.xz"
    # checksum from the official CHECKSUM file alongside
    fetch "$work/ck" "https://dl.rockylinux.org/pub/rocky/10/images/$rarch/CHECKSUM"
    sum=$(grep "Container-Base.latest.$rarch.tar.xz" "$work/ck" | grep -oE '[0-9a-f]{64}' | head -1)
    [ -n "$sum" ] || { echo "no official checksum for rocky10-$arch" >&2; exit 1; }
    echo "$sum  $oci" | sha256sum -c -
    "$repo/scripts/rootfs-oci.sh" "$oci" "$out"
    echo "https://dl.rockylinux.org/pub/rocky/10/images/$rarch/Rocky-10-Container-Base.latest.$rarch.tar.xz (sha256:$sum)" > "$out.source"
    ;;
  *)
    echo "fetch-rootfs: no official source for $distroid-$arch" >&2
    exit 1
    ;;
esac

echo "$out"
