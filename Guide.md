<div align="center">
<p>English | <a href="Guide.zh-CN.md">简体中文</a></p>
</div>

# PreLinux guide

## Install

Download the installer and run it:

```sh
curl -fsSLO https://github.com/hqzing/prelinux/releases/latest/download/install.sh

# installs debian:12 into ~/prelinux by default, and fetches the image from GitHub
sh install.sh

# or pick another distribution: ubuntu:24.04 / rocky:10
# sh install.sh ubuntu:24.04

# or install under a prefix of your choice
# PRELINUX_HOME=/opt/prelinux sh install.sh
```

The script does three things:

1. downloads the image into `~/prelinux/cache/`;
2. extracts the `prelinux` command from it into `~/prelinux/bin`;
3. imports the image as an instance at `~/prelinux/<distro>`.

Everything lives under `~/prelinux`, or under the prefix you set. To uninstall, delete that one directory.

## The command line tool

The project ships a command line tool named `prelinux` that creates instances and shows instance information. It lives in `~/prelinux/bin`. Add that directory to `PATH` to use it:

```sh
export PATH="$HOME/prelinux/bin:$PATH"
```

Common subcommands:

| Subcommand | Purpose |
|---|---|
| `prelinux import <distro> [dir]` | create an instance from an image |
| `prelinux root` | the current instance root |
| `prelinux translate <path>` | where a path is mapped to |
| `prelinux info` | instance metadata |

## First use

```sh
source ~/prelinux/debian12/env.sh
apt update
apt install -y tree
tree -L 1 /
```

After `source`, commands such as `apt`, `python3` and `git` in this terminal prefer the instance's copies, and `/etc` and `/usr` show the instance's contents. Close the terminal or start a new shell to return to the host.

## Offline install

On a slow or disconnected network, download the image and import it:

```sh
prelinux import ./prelinux-image-debian12-aarch64.tar.gz ~/prelinux/debian12
```

Images are the `prelinux-image-<distro>-<arch>.tar.gz` files on the [releases page](https://github.com/hqzing/prelinux/releases), one file per distribution.

## Everyday use

Install, upgrade and remove software with the distribution's own package manager (`apt` on Debian/Ubuntu, `dnf` on Rocky):

```sh
apt install -y vim curl
apt upgrade
apt remove vim
```

Switch to a faster mirror (the official sources are slow in mainland China):

```sh
# debian 12
sed -i 's|http://deb.debian.org/debian|http://mirrors.aliyun.com/debian|;
        s|http://deb.debian.org/debian-security|http://mirrors.aliyun.com/debian-security|' \
    /etc/apt/sources.list.d/debian.sources

# ubuntu 24.04
sed -i 's|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|;
        s|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|' \
    /etc/apt/sources.list.d/ubuntu.sources

# rocky 10
for f in /etc/yum.repos.d/*.repo; do
  sed -i -e 's|^mirrorlist=|#mirrorlist=|' \
         -e 's|^#baseurl=http://dl.rockylinux.org/$contentdir/|baseurl=https://mirrors.aliyun.com/rockylinux/|' "$f"
done
dnf makecache
```

Install a second distribution: `sh install.sh ubuntu:24.04` gives you `~/prelinux/ubuntu2404`. Instances are independent. Source the `env.sh` of the one you want to enter.

Inspect things:

```sh
prelinux root                    # the current instance root
prelinux translate /etc/passwd   # where a path is mapped to
prelinux info                    # instance metadata
```

## Troubleshooting

**`apt` prints `Setting TIOCSCTTY ... failed`**: the HongMeng kernel rejects this ioctl. apt continues, so ignore it.

**`dnf` prints `Failed to set locale`**: the image carries no locale data. It does not affect installation.

**`tar: can't link ... Operation not permitted`**: the imported image contains hard links and HarmonyOS's hmdfs does not support them. Use this project's images, or repack a third-party rootfs with `tar --hard-dereference`.

**`prelinux: unknown distro`**: the name is misspelled. The available names are `debian:12`, `ubuntu:24.04`, `rocky:10`.

## Limitations

| Situation | Result |
|---|---|
| Statically linked programs (Go, ...), programs issuing raw syscalls | no path mapping, so they see the host filesystem |
| mount / namespaces / cgroups / setuid | need privileges, so unavailable |
| Graphical programs | unsupported, since there is no display service |
| Compiling or building | unsupported in any language. A dynamic ELF needs its interpreter path rewritten before it can run here, and a static ELF bypasses the path mapping. Use distro packages, and pip can only install wheels |
| Upgrading glibc | unsupported. The instance's glibc is patched, and upgrading it breaks the path mapping, so apt and dnf skip it when upgrading packages |
| File permissions and ownership | what `ls -l` shows cannot be trusted (hmdfs ignores `chmod` and has no extended attributes), while contents, sizes and timestamps are fine |

## Building from source

```sh
make -C prelinux    # needs musl-tools, produces prelinux/prelinux
scripts/build-glibc.sh <src dir> <build dir> <install dir>
scripts/package-base.sh <glibc install dir> prelinux <output dir>
scripts/package-image.sh <rootfs.tar.gz> <base.tar.gz> <adapter.tar.gz> <distro> <output dir>
```

Each script documents its arguments and requirements in its header.
