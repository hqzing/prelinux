<div align="center">
<p><a href="README.md">English</a> | 简体中文</p>
</div>

# PreLinux

将 Linux 发行版安装到你指定的前缀目录下。可在宿主系统里直接使用发行版自带的包管理器和软件包。

支持在鸿蒙 PC 和 Linux 设备上使用。当前可安装的发行版：Debian、Ubuntu、Rocky Linux。

PreLinux 命名来源于 Prefix + Linux：整套 Linux 用户态装在一个前缀目录里，程序看到的 `/` 由这个前缀提供。

## 快速开始

通过安装脚本安装：

```sh
curl -fsSLO https://github.com/hqzing/prelinux/releases/latest/download/install.sh

# 默认安装 debian:12 到 ~/prelinux，脚本从 GitHub 拉取镜像
sh install.sh

# 也可以选择 ubuntu:24.04 / rocky:10
# sh install.sh ubuntu:24.04

# 或者装到你指定的前缀下
# PRELINUX_HOME=/opt/prelinux sh install.sh
```

配置环境变量后即可使用：

```sh
source ~/prelinux/debian12/env.sh
apt update
apt install -y tree
tree -L 1 /
```

所有内容都在 `~/prelinux` 下（或你指定的前缀），每个发行版一个目录，这个目录就是一个实例。卸载时删除这个前缀即可。

换国内源、多实例、排障、已知限制等详见[使用指南](Guide.zh-CN.md)。

## 原理

通过一份打过补丁的 glibc，将 FHS 路径重定向到实例目录。不需要 root，不需要 ptrace，也不需要重新编译。

实例内的进程访问的 FHS 目录重定向到实例内部：
- `/bin` -> `~/prelinux/<发行版>/bin`
- `/lib` -> `~/prelinux/<发行版>/lib`
- ...

非 FHS 目录直通宿主系统：
- `/storage` -> `/storage`
- `/system` -> `/system`
- ...

设计与实现细节见[架构设计文档](Architecture.zh-CN.md)。

## 许可证

- 本项目：**GPL-3.0-or-later**，见 [LICENSE](LICENSE)
- 仓库内集成了 [ohos-selfsign](https://github.com/hqzing/ohos-selfsign)：**0BSD**
- 命令行工具静态链接了 musl：**MIT**
- 镜像内的发行版组件：各自的上游许可证，见镜像里的 `NOTICE` 和 `/usr/share/doc/*/copyright`
