<div align="center">
<p><a href="Guide.md">English</a> | 简体中文</p>
</div>

# PreLinux 使用指南

## 安装

下载安装脚本并执行：

```sh
curl -fsSLO https://github.com/hqzing/prelinux/releases/latest/download/install.sh

# 默认安装 debian:12 到 ~/prelinux，脚本从 GitHub 拉取镜像
sh install.sh

# 也可以选择 ubuntu:24.04 / rocky:10
# sh install.sh ubuntu:24.04

# 或者装到你指定的前缀下
# PRELINUX_HOME=/opt/prelinux sh install.sh
```

脚本做三件事：

1. 把镜像下载到 `~/prelinux/cache/`；
2. 从镜像里取出 `prelinux` 命令，装到 `~/prelinux/bin`；
3. 把镜像导入成一个实例，目录在 `~/prelinux/<发行版>`。

所有内容都在 `~/prelinux` 下（或你指定的前缀），卸载时删除这一个目录即可。

## 命令行工具

本项目提供了一个名为 `prelinux` 的命令行工具，用来创建实例、查看实例信息。它位于 `~/prelinux/bin`，如需使用，把该目录加入 `PATH`：

```sh
export PATH="$HOME/prelinux/bin:$PATH"
```

常用子命令：

| 子命令 | 作用 |
|---|---|
| `prelinux import <发行版> [目录]` | 从镜像创建实例 |
| `prelinux root` | 查看当前实例根目录 |
| `prelinux translate <路径>` | 输入实例路径，返回映射后的真实路径 |
| `prelinux info` | 查看实例元数据 |

## 第一次使用

```sh
source ~/prelinux/debian12/env.sh
apt update
apt install -y tree
tree -L 1 /
```

`source` 之后，当前终端里的 `apt`、`python3`、`git` 等命令优先使用实例里的版本，`/etc`、`/usr` 看到的也是实例里的内容。关掉终端或新开一个 shell 就回到宿主。

## 离线安装

对于网络不通畅/无网的环境，也可以下载镜像后导入：

```sh
prelinux import ./prelinux-image-debian12-aarch64.tar.gz ~/prelinux/debian12
```

镜像就是[发布页](https://github.com/hqzing/prelinux/releases)上的 `prelinux-image-<发行版>-<架构>.tar.gz`，一个发行版一个文件。

## 常用操作

用发行版自己的包管理器（Debian/Ubuntu 用 `apt`，Rocky 用 `dnf`）装软件、升级、卸载：

```sh
apt install -y vim curl
apt upgrade
apt remove vim
```

换国内源（官方源在国内很慢）：

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

再装一个发行版：`sh install.sh ubuntu:24.04`，得到 `~/prelinux/ubuntu2404`。实例之间互不影响，`source` 哪个实例的 `env.sh` 就进入哪个实例。

查看信息：

```sh
prelinux root                    # 当前实例根目录
prelinux translate /etc/passwd   # 看路径被映射到哪里
prelinux info                    # 实例元数据
```

## 常见问题

**apt 打印 `Setting TIOCSCTTY ... failed`**：鸿蒙内核不接受这个 ioctl，apt 会继续跑，可以忽略。

**dnf 打印 `Failed to set locale`**：镜像里没有 locale 数据，不影响安装。

**`tar: can't link ... Operation not permitted`**：导入的镜像里带硬链接，而鸿蒙的 hmdfs 不支持硬链接。用本项目提供的镜像，或用 `tar --hard-dereference` 重新打包第三方 rootfs。

**`prelinux: unknown distro`**：发行版名写错，可用的有 `debian:12`、`ubuntu:24.04`、`rocky:10`。

## 限制

| 场景 | 结果 |
|---|---|
| 静态链接程序（Go 等）、直接发 syscall 的程序 | 路径不映射，看到宿主文件系统 |
| mount / namespace / cgroup / setuid | 需要特权，不可用 |
| 图形界面程序 | 不支持，没有显示服务 |
| 编译构建 | 任何语言都不支持。编出来的动态 ELF 要改写解释器路径才能在本实例运行，静态 ELF 则不经过路径映射。请用发行版预编译包，pip 只能装带 wheel 的包 |
| 文件权限与属主 | `ls -l` 显示的值不可信，hmdfs 忽略 `chmod` 也没有扩展属性；文件内容、大小、时间戳正常 |
| 升级 glibc | 不支持。实例里的 glibc 是改过的，升级会破坏路径映射，所以 apt 和 dnf 在升级软件包时会跳过它 |

## 从源码构建

```sh
make -C prelinux    # 需要 musl-tools，产物 prelinux/prelinux
scripts/build-glibc.sh <源码目录> <构建目录> <安装目录>
scripts/package-base.sh <glibc 安装目录> prelinux <输出目录>
scripts/package-image.sh <rootfs.tar.gz> <base.tar.gz> <adapter.tar.gz> <发行版> <输出目录>
```

每个脚本开头都写了参数说明和它依赖的环境。
