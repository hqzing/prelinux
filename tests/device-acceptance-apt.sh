#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# PreLinux on-device acceptance test for the apt family (Debian, Ubuntu).
# Run INSIDE an instance:
#   inst/bin/bash /root/device-acceptance-apt.sh   (with env.sh sourced)
set -u
: "${PRELINUX_ROOT:?source <instance>/env.sh first}"

pass=0; fail=0
ok()   { printf 'PASS  %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf 'FAIL  %s\n' "$*"; fail=$((fail+1)); }
check(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

export DEBIAN_FRONTEND=noninteractive LC_ALL=C

echo "=== 1. environment"
EXPECT_ID=${EXPECT_ID:-debian}
. /etc/os-release
[ "${ID:-}" = "$EXPECT_ID" ] && ok "instance os-release: $PRETTY_NAME" \
  || bad "instance os-release (ID=${ID:-?}, want $EXPECT_ID)"
[ "$(uname -s)" = Linux ] && ok "uname -s = Linux" || bad "uname -s"
[ "$(id -u)" = 0 ] && ok "fake root (credential theater)" || bad "id -u"
grep -q '^root:' /etc/passwd && ok "instance /etc/passwd" || bad "instance /etc/passwd"

echo "=== 2. fchmodat2-free binaries (tar/cp/dpkg-deb)"
rm -rf /root/t && mkdir -p /root/t/sub
echo hi > /root/t/a.txt; cp /root/t/a.txt /root/t/sub/b.txt
( cd /root/t && tar -cf t.tar . ) && ok "tar create" || bad "tar create"
mkdir -p /root/t/out
tar -xf /root/t/t.tar -C /root/t/out 2>/root/t/err \
  && ok "tar extract (fchmodat AT_SYMLINK_NOFOLLOW)" || { bad "tar extract"; cat /root/t/err; }
grep -q hi /root/t/out/a.txt && ok "tar extract content" || bad "tar extract content"
cp -a /root/t /root/t.copy 2>/root/t/err2 && ok "cp -a" || { bad "cp -a"; cat /root/t/err2; }
[ -f /root/t.copy/sub/b.txt ] && ok "cp -a content" || bad "cp -a content"
chmod 000 /root/t/a.txt && chmod 644 /root/t/a.txt && ok "chmod 000 then 644" || bad "chmod mode-0000"
rm -rf /root/t /root/t.copy

echo "=== 3. install packages"
apt-get update -qq 2>/dev/null && ok "apt-get update" || bad "apt-get update"
apt-get install -y -qq tree curl tzdata >/dev/null 2>&1 \
  && ok "install tree + curl + tzdata" || bad "install packages"
tree --version >/dev/null 2>&1 && ok "tree runs" || bad "tree runs"
curl --version >/dev/null 2>&1 && ok "curl runs" || bad "curl runs"
curl --version | grep -q libcurl && ok "curl links libcurl" || bad "curl links libcurl"
[ -L /etc/localtime ] && ok "/etc/localtime symlink" || bad "/etc/localtime symlink"

echo "=== 4. overwrite"
apt-get install -y -qq --reinstall tree tzdata >/dev/null 2>&1 \
  && ok "reinstall tree + tzdata (overwrite signed files)" || bad "reinstall"
tree --version >/dev/null 2>&1 && ok "tree runs after reinstall" || bad "tree after reinstall"
[ -L /etc/localtime ] && ok "/etc/localtime intact after reinstall" || bad "/etc/localtime after reinstall"

echo "=== 5. installed binaries are physically relocated"
# binutils is not in the image, so search the string directly
grep -qa "$PRELINUX_ROOT/glibc/lib/ld-linux" /usr/bin/curl \
  && ok "curl PT_INTERP relocated" || bad "curl PT_INTERP"
grep -qa "$PRELINUX_ROOT/glibc/lib/ld-linux" /usr/bin/tree \
  && ok "tree PT_INTERP relocated" || bad "tree PT_INTERP"

echo
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
