<div align="center">
<p>English | <a href="README.zh-CN.md">简体中文</a></p>
</div>

# PreLinux

Install a Linux distribution into a prefix directory of your choice, and use that distribution's own package manager and packages on the host system.

Supported on HarmonyOS PC and Linux hosts. Available distributions: Debian, Ubuntu, Rocky Linux.

The name comes from *Prefix + Linux*: the whole Linux userland lives in one prefix directory, and the `/` a program sees is provided by that prefix.

## Quick start

Install with the installer script:

```sh
curl -fsSLO https://github.com/hqzing/prelinux/releases/latest/download/install.sh

# installs debian:12 into ~/prelinux by default, and fetches the image from GitHub
sh install.sh

# or pick another distribution: ubuntu:24.04 / rocky:10
# sh install.sh ubuntu:24.04

# or install under a prefix of your choice
# PRELINUX_HOME=/opt/prelinux sh install.sh
```

Then set up the environment and use it:

```sh
source ~/prelinux/debian12/env.sh
apt update
apt install -y tree
tree -L 1 /
```

Everything lives under `~/prelinux` (or the prefix you set), one directory per distribution, and that directory is the instance. To uninstall, delete that prefix.

Using a local mirror, multiple instances, troubleshooting and the list of limitations are covered in the [guide](Guide.md).

## How it works

A patched glibc redirects FHS paths into the instance directory. No root, no ptrace, no rebuilding of packages.

Inside an instance, FHS paths are redirected into it:

- `/bin` -> `~/prelinux/<distro>/bin`
- `/lib` -> `~/prelinux/<distro>/lib`
- ...

Non-FHS paths pass through to the host:

- `/storage` -> `/storage`
- `/system` -> `/system`
- ...

Design and implementation notes are in the [architecture notes](Architecture.zh-CN.md) (Chinese only for now).

## License

- This project: **GPL-3.0-or-later**, see [LICENSE](LICENSE)
- [ohos-selfsign](https://github.com/hqzing/ohos-selfsign) is bundled: **0BSD**
- The command line tool statically links musl: **MIT**
- Distribution components inside the images keep their upstream licenses. See `NOTICE` in the image and `/usr/share/doc/*/copyright`
