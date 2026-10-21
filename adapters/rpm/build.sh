# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# adapters/rpm/build.sh — rpm adapter for PreLinux.
#
# APKBUILD/PKGBUILD style.  rpm's unpack path is lib/fsm.c's fsmCommit;
# our patch relocates each freshly installed regular file there.
#
# Per-distro pinned upstreams (match the distro's rpm):
#   rocky10   rpm 4.19.1.1 (rockylinux/rockylinux:10's own version)

case "${distroid:?set by the CI driver}" in
  rocky10)
    pkgver=4.19.1.1
    source="https://ftp.osuosl.org/pub/rpm/releases/rpm-4.19.x/rpm-${pkgver}.tar.bz2"
    sha256=874091b80efe66f9de8e3242ae2337162e2d7131e3aa4ac99ac22155e9c521e5
    ;;
  *)
    echo "rpm adapter: no pinned version for distro '$distroid'" >&2
    exit 1
    ;;
esac

pkgname=rpm
pkgrel=0
glibc_baseline=2.39

# alma/rocky package names (dnf/yum compatible)
makedepends="gcc gcc-c++ make cmake patch gettext bzip2 readline-devel pkgconf-pkg-config lua-devel libarchive-devel sqlite-devel popt-devel file-devel libselinux-devel libacl-devel bzip2-devel xz-devel zlib-devel elfutils-devel libcap-devel openssl-devel curl-devel rpm-sequoia-devel"
checkdepends=""

# RHEL clones (rocky10) keep some -devel packages in the CRB repo,
# which ships disabled; enable it when a config-manager is available.
preinstall() {
  command -v dnf >/dev/null || return 0
  dnf install -y -q dnf-plugins-core >/dev/null 2>&1 || return 0
  dnf config-manager --set-enabled crb >/dev/null 2>&1 || true
}

prepare() {
  cd "${srcdir:?}/rpm-${pkgver}"
  for p in "$adapterdir"/patches/*.patch; do
    patch -p1 < "$p"
  done
}

build() {
  cd "${srcdir:?}/rpm-${pkgver}"
  mkdir -p build && cd build
  cmake .. \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_NLS=OFF \
    -DENABLE_OPENMP=OFF \
    -DENABLE_PYTHON=OFF \
    -DENABLE_PLUGINS=OFF \
    -DENABLE_TESTSUITE=OFF \
    -DWITH_FSVRSE=OFF \
    -DWITH_SELINUX=OFF \
    -DWITH_AUDIT=OFF
  make -j"$(nproc)"
}

check() {
  # rpm's test suite needs a full distro environment; build-level smoke
  # is exercised by CI's e2e (install+run tree in an instance).
  :
}

package() {
  cd "${srcdir:?}/rpm-${pkgver}/build"
  make install DESTDIR="$pkgdir"
  # Do NOT ship an rpmdb: the instance's rootfs already carries its own
  # /var/lib/rpm, and an empty database here would wipe it.
  rm -rf "$pkgdir/var"
}
