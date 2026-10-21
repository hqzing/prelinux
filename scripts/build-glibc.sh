#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# build-glibc.sh — configure & build prelinux-glibc from a patched source tree.
#
# Usage: build-glibc.sh <srcdir> <builddir> <destdir> [--no-prelinux]
#
# The glibc prefix is the 512-byte placeholder (design chapter 3): every
# occurrence is rewritten to the real instance root by `prelinux relocate`
# at install time.  --no-prelinux builds the same tree without -DPRELINUX
# for the upstream-equivalence check.
set -eu

srcdir=$1
builddir=$2
destdir=$3
prelinux=1
[ "${4:-}" = "--no-prelinux" ] && prelinux=0

# absolutize: we cd into builddir below, so relative paths would rot
srcdir=$(cd "$srcdir" && pwd)
builddir=$(mkdir -p "$builddir" && cd "$builddir" && pwd)
destdir=$(mkdir -p "$destdir" && cd "$destdir" && pwd)

placeholder="$(printf '/.prelinux-root-placeholder-'; printf 'A%.0s' $(seq 226))"
[ ${#placeholder} -eq 254 ] || { echo "placeholder generation broken (got ${#placeholder} bytes)" >&2; exit 1; }
prefix="$placeholder/glibc"

cflags="-O2 -fstack-clash-protection"
# Ubuntu's gcc spec-injects -D_FORTIFY_SOURCE=3 (even after glibc's own
# -Wp,-U_FORTIFY_SOURCE), which breaks glibc's own build (syslog.c).
cflags="$cflags -U_FORTIFY_SOURCE"
if [ "$(uname -m)" = "aarch64" ]; then
  cflags="$cflags -mbranch-protection=standard"
fi
if [ "$prelinux" = 1 ]; then
  cflags="$cflags -DPRELINUX"
fi

mkdir -p "$builddir" "$destdir"
cd "$builddir"

if [ ! -f config.status ]; then
  "$srcdir/configure" \
    --prefix="$prefix" \
    --sysconfdir=/etc \
    --localstatedir=/var \
    --libdir="$prefix/lib" \
    --disable-crypt \
    --disable-debug \
    --disable-dependency-tracking \
    --disable-silent-rules \
    --disable-werror \
    --without-gd \
    --without-selinux \
    --enable-bind-now \
    --enable-stack-protector=strong \
    --with-pkgversion="PreLinux glibc 2.39" \
    --with-bugurl="https://github.com/hqzing/prelinux/issues" \
    CFLAGS="$cflags"
fi

# PreLinux: the default library search path must include the FHS dirs
# (the translation layer maps them into the instance) in
# addition to the build's own libdir.  RHEL-family distros rely on the
# built-in trusted dirs /lib64:/usr/lib64 (their ld.so.conf does not
# list them), so keep them alongside the Debian-style /lib:/usr/lib.
# The stamp file is not sensitive to make variables, so force it to
# regenerate.
rm -f "$builddir"/elf/trusted-dirs.st "$builddir"/elf/trusted-dirs.h
make -j"$(nproc)" all user-defined-trusted-dirs=/lib:/usr/lib:/lib64:/usr/lib64
make install DESTDIR="$destdir"
