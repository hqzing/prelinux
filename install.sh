#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# PreLinux installer.  Downloads the image for one distro, installs the
# CLI, and creates an instance, all under ~/prelinux:
#
#   ~/prelinux/bin/prelinux          the command
#   ~/prelinux/cache/                downloaded images
#   ~/prelinux/<distro>/             an instance
#
# It never edits shell startup files; the PATH line is printed at the end.
# To uninstall, delete ~/prelinux.
set -eu

PRELINUX_HOME="${PRELINUX_HOME:-$HOME/prelinux}"
case "$PRELINUX_HOME" in
  /*) ;;
  *) printf 'prelinux-install: error: PRELINUX_HOME must be an absolute path\n' >&2; exit 1 ;;
esac
export PRELINUX_HOME
# Ordered list of release bases for the image; override with
# PRELINUX_RELEASE_URLS="https://mirror-a/... https://mirror-b/...".
PRELINUX_RELEASE_URLS="${PRELINUX_RELEASE_URLS:-https://github.com/hqzing/prelinux/releases/latest/download}"
DISTRO=""

for arg in "$@"; do
  case "$arg" in
    --distro=*) DISTRO="${arg#--distro=}" ;;
    *) DISTRO="$arg" ;;
  esac
done
DISTRO="${DISTRO:-debian:12}"

say() { printf 'prelinux-install: %s\n' "$*"; }
die() { printf 'prelinux-install: error: %s\n' "$*" >&2; exit 1; }

arch=$(uname -m)
case "$arch" in
  aarch64|x86_64) ;;
  *) die "unsupported architecture: $arch" ;;
esac
[ -w "$HOME" ] || die "\$HOME ($HOME) is not writable"
command -v curl >/dev/null 2>&1 || die "curl is required"

distroid=$(echo "$DISTRO" | tr -d ':.')
img="$PRELINUX_HOME/cache/prelinux-image-$distroid-$arch.tar.gz"
mkdir -p "$PRELINUX_HOME/cache" "$PRELINUX_HOME/bin"

if [ ! -f "$img" ]; then
  ok=0
  for base in $PRELINUX_RELEASE_URLS; do
    say "downloading image for $DISTRO ($arch) from $base"
    if curl -fsSL --retry 3 --retry-delay 2 -o "$img" "$base/prelinux-image-$distroid-$arch.tar.gz"; then
      ok=1
      break
    fi
  done
  [ "$ok" = 1 ] || die "image download failed (tried: $PRELINUX_RELEASE_URLS)"
fi

say "installing CLI into $PRELINUX_HOME/bin"
tmpd=$(mktemp -d) || die "cannot create a temporary directory"
trap 'rm -rf "$tmpd"' EXIT
gzip -dc "$img" | tar -xf - -C "$tmpd" ./prelinux/bin/prelinux 2>/dev/null \
  || { gzip -dc "$img" | tar -xOf - ./prelinux/bin/prelinux > "$tmpd/prelinux"; }
if [ -f "$tmpd/prelinux/bin/prelinux" ]; then
  cp "$tmpd/prelinux/bin/prelinux" "$PRELINUX_HOME/bin/prelinux"
else
  cp "$tmpd/prelinux" "$PRELINUX_HOME/bin/prelinux"
fi
chmod 755 "$PRELINUX_HOME/bin/prelinux"
rm -rf "$tmpd"
trap - EXIT

say "importing $DISTRO"
# import reads the singular variable, install.sh the list; give both stages
# the same channel so a self-hosted mirror is not silently bypassed.
if [ -n "${PRELINUX_RELEASE_URLS:-}" ]; then
  PRELINUX_RELEASE_URL="${PRELINUX_RELEASE_URLS%% *}"
  export PRELINUX_RELEASE_URL
fi
"$PRELINUX_HOME/bin/prelinux" import "$DISTRO" "$PRELINUX_HOME/$distroid"

say "done"
cat <<MSG

  Add the CLI to your PATH (in ~/.profile, ~/.bashrc or ~/.zshrc):
    export PATH="$PRELINUX_HOME/bin:\$PATH"

  Then enter the instance:
    source $PRELINUX_HOME/$distroid/env.sh

  Uninstall: rm -rf "$PRELINUX_HOME"
MSG
