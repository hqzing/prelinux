#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# test-strace.sh: every absolute path a
# patched-glibc program passes to the kernel must land under the
# instance root or a pass-through prefix.
#
# Usage: test-strace.sh <repo-root> <instance-dir>
# Needs: host cc, strace.
set -eu
repo=$1
inst=$2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -O2 -Wall -o "$work/pathtest" "$repo/tests/pathtest.c"
# use the repo's own (pristine, out-of-instance) binary: literal paths
# win over translation when the file exists on the host
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/pathtest"

# %file covers all path-carrying syscalls, arch-independently
strace -f -qq -o "$work/tr.log" -e trace=%file,execve,execveat \
  "$work/pathtest" >/dev/null

# First quoted string per syscall line = the path argument (also covers
# "AT_FDCWD, \"/path\"" forms).  execve's argv arrays come later on the
# line and are ignored.
grep -oE '^[a-z0-9_]+\([^"]*"[^"]*"' "$work/tr.log" \
  | grep -oE '"[^"]*"' | tr -d '"' | grep '^/' | sort -u > "$work/paths.txt"
# same for lines with a leading pid ("123   openat(...)")
grep -oE '^ *[0-9]+ +[a-z0-9_]+\([^"]*"[^"]*"' "$work/tr.log" \
  | grep -oE '"[^"]*"' | tr -d '"' | grep '^/' | sort -u >> "$work/paths.txt"
sort -u -o "$work/paths.txt" "$work/paths.txt"

fails=0
while read -r p; do
  case "$p" in
    "$inst"/*) ;;                                        # instance: OK
    "$work"/*) ;;                                        # the test binary itself: OK
    /proc/*|/sys/*|/dev/*|/selinux|/selinux/*|/storage/*|/mnt/*|/media/*|/data/*|/system/*|/vendor/*) ;;
    /dev) ;;
    *) echo "STRACE-ESCAPE: $p"
       # show the offending syscall lines so the culprit call is visible
       grep -F "\"$p\"" "$work/tr.log" | head -3 | sed 's/^/STRACE-LINE: /'
       fails=1 ;;
  esac
done < "$work/paths.txt"

if [ "$fails" != 0 ]; then
  echo "test-strace: FAIL — paths escaped the instance" >&2
  exit 1
fi

count=$(wc -l < "$work/paths.txt")
echo "test-strace: OK ($count distinct path arguments, all confined)"
