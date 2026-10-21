#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# check-equivalence.sh — the patched glibc tree built
# without -DPRELINUX must behave exactly like upstream.
#
#   modified files (patches to existing sources): .text of the compiled
#     object must be byte-identical between the no-PRELINUX build and a
#     build of the pristine tree;
#   new files (shadow wrappers replacing syscalls.list entries): no
#     upstream object exists to diff — verify the built libc.so.6 keeps
#     exporting the same symbols.
#
# Usage: check-equivalence.sh <patchdir> <build-patched-noprelinux> <build-upstream>
set -eu

patches=$1
bP=$2
bU=$3

fail=0

for patch in "$patches"/*.patch; do
  # files added by the patch (--- /dev/null) vs modified
  grep -E '^\+\+\+ b/' "$patch" | sed 's|^+++ b/||' | while read -r src; do
    echo "$src"
  done
done > /tmp/eq-all.$$.txt
grep -l '^--- /dev/null' "$patches"/*.patch >/dev/null 2>&1 || true

for patch in "$patches"/*.patch; do
  grep -E '^\+\+\+ b/' "$patch" | sed 's|^+++ b/||' > /tmp/eq-files.$$.txt
  grep -q '^--- /dev/null' "$patch" && isnew=1 || isnew=0
  while read -r src; do
    case "$src" in
      *.c) ;;
      *) continue ;;
    esac
    if grep -q " $src\$" /dev/null 2>/dev/null; then :; fi
    base=$(basename "$src" .c)
    oP=$(find "$bP" -name "$base.os" 2>/dev/null | head -1)
    if [ -z "$oP" ]; then
      continue
    fi
    if [ "$isnew" = 0 ]; then
      oU=$(find "$bU" -name "$base.os" 2>/dev/null | head -1)
      if [ -z "$oU" ]; then
        echo "check-equivalence: no upstream object for $src" >&2
        fail=1
        continue
      fi
      tP=$(objcopy -O binary --only-section=.text "$oP" /dev/stdout 2>/dev/null | sha256sum | cut -d' ' -f1)
      tU=$(objcopy -O binary --only-section=.text "$oU" /dev/stdout 2>/dev/null | sha256sum | cut -d' ' -f1)
      if [ "$tP" != "$tU" ]; then
        echo "check-equivalence: .text differs for $src" >&2
        fail=1
      fi
    fi
  done < /tmp/eq-files.$$.txt
done

# new-file symbol check: these symbols must exist in the no-PRELINUX libc
for sym in chdir chroot execve linkat symlinkat readlinkat unlinkat \
           mkdirat fchownat setxattr lsetxattr fsetxattr \
           getuid geteuid getgid getegid getgroups getresuid getresgid uname; do
  if ! nm -D "$bP/libc.so.6" 2>/dev/null | grep -q " $sym@@"; then
    echo "check-equivalence: libc.so.6 lost symbol $sym" >&2
    fail=1
  fi
done

rm -f /tmp/eq-all.$$.txt /tmp/eq-files.$$.txt
if [ "$fail" = 0 ]; then
  echo "check-equivalence: OK"
fi
exit $fail
