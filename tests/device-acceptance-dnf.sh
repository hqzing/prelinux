#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# PreLinux on-device acceptance test for the rpm family (Rocky Linux 10).
# Run INSIDE an instance:
#   inst/bin/bash /root/device-acceptance-dnf.sh
set -u
: "${PRELINUX_ROOT:?source <instance>/env.sh first}"

pass=0; fail=0
ok()   { printf 'PASS  %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf 'FAIL  %s\n' "$*"; fail=$((fail+1)); }
check(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

export LC_ALL=C

echo "=== 1. environment"
grep -q '^NAME="Rocky Linux"' /etc/os-release && ok "instance /etc/os-release" || bad "instance /etc/os-release"
[ "$(uname -s)" = Linux ] && ok "uname -s = Linux" || bad "uname -s"
[ "$(id -u)" = 0 ] && ok "fake root (credential theater)" || bad "id -u"

echo "=== 2. rpm/dnf basics"
check "rpm --version" "rpm --version"
check "dnf --version" "dnf --version"
check "rpmdb readable (rpm -qa)" "rpm -qa"
dnf -q makecache >/dev/null 2>&1 && ok "dnf makecache" || bad "dnf makecache"

echo "=== 3. install packages"
dnf -q makecache >/dev/null 2>&1 && ok "dnf makecache" || bad "dnf makecache"
dnf install -y -q tree curl tzdata >/dev/null 2>&1 \
  && ok "install tree + curl + tzdata (GPG on)" || bad "install packages"
tree --version >/dev/null 2>&1 && ok "tree runs" || bad "tree runs"
curl --version >/dev/null 2>&1 && ok "curl runs" || bad "curl runs"
curl --version | grep -q libcurl && ok "curl links libcurl" || bad "curl links libcurl"
[ -L /etc/localtime ] && ok "/etc/localtime symlink" || bad "/etc/localtime symlink"

echo "=== 4. overwrite"
dnf reinstall -y -q tree curl tzdata >/dev/null 2>&1 \
  && ok "dnf reinstall tree + tzdata (overwrite + scriptlets)" || bad "reinstall"
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
