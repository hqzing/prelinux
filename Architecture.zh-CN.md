# PreLinux 架构设计文档

> 本文档是实施的唯一权威依据，代码实现与本文档冲突时以本文档为准；发现文档本身有缺陷时先修改文档再实施。

## 第一章 项目概述

### 1.1 一句话定义

PreLinux 是一个宿主无关的 Linux 用户态虚拟化项目，通过一个打补丁的 glibc 实现 FHS 路径重定向，使用户无需 root、无需 ptrace、无需重编译软件包，即可在鸿蒙 PC 和 Linux 系统上直接运行主流发行版（Debian、Ubuntu 等）的官方二进制软件包。

名字来自 Prefix + Linux：整套 Linux 用户态安装在一个前缀目录（实例根）下，实例内进程看到的 `/` 由这个前缀提供。

### 1.2 定位

- 目标宿主：鸿蒙 PC 与运行 Linux 的设备，架构 aarch64、x86_64。
- 长期定位是“无 root 环境的发行版宿主”，覆盖 Android、ChromeOS、HPC 集群、受限开发机等一切“内核不归用户管”的场景。
- 与容器/模拟器的区别见 1.3。它不提供发行版，只提供运行发行版官方二进制的宿主环境。

### 1.3 与现有方案的差异

| 方案 | 机制 | 缺陷 | PreLinux 的对应优势 |
|---|---|---|---|
| Termux | 重编译整个仓库 | 生态规模受限于维护者人力 | 直接消费 Debian/Ubuntu 官方仓库 |
| proot / proot-distro | ptrace 拦截 syscall | 性能损耗大，依赖 ptrace 权限 | libc 层字符串映射，开销接近零，不需要 ptrace |
| Homebrew | 自建前缀，二进制或源码 | 生态是自建的，与发行版仓库无关 | 直接使用发行版官方仓库 |
| Bedrock Linux | hijack init | 侵入性极强 | 纯用户态，零侵入 |
| 容器（podman/docker） | namespace + cgroups | 需要 root 或内核配置 | 无 root 可用 |

### 1.4 命名约定

- 项目名：PreLinux
- 命名约定：命令一律为 `prelinux <子命令>` 形态（如 `prelinux import`、`prelinux relocate`）
- 环境变量前缀：`PRELINUX_`（全大写）
- 代码符号前缀：`prelinux_` / `__prelinux_`（C 符号，全小写下划线）
- 文档与宣传中写作 PreLinux
- 发布产物命名：`prelinux-image-<distro><ver>-<arch>.tar.gz`

## 第二章 设计原则

以下原则优先级从高到低，冲突时排名靠前的胜出。

1. **宿主无关性**。核心层（glibc 补丁、底座 SDK）不允许出现任何鸿蒙特定的路径、API、行为假设。所有宿主特定的探测逻辑只允许存在于安装脚本中。
2. **映射规则极简**。路径翻译规则必须能用一页文档说清楚。任何“聪明”的特殊处理都是未来的 bug 温床。
3. **零侵入宿主**。安装不需要 root，不写入安装目录以外的任何路径，卸载等于删除目录。
4. **上游二进制优先**。优先直接运行发行版官方二进制；只有包管理器本体和无法回避的组件才自行编译。
5. **补丁集最小化**。glibc 补丁集的目标规模是修改不超过 10 个文件，全部用 `#ifdef PRELINUX` 包裹，保证可以低成本 rebase 到新版 glibc。
6. **诚实声明边界**。方案固有的能力边界（见第八章）写进用户文档，不掩盖、不粉饰。

## 第三章 术语

- **宿主（host）**：运行 PreLinux 的真实系统。
- **宿主根**：真实文件系统的 `/`。
- **rootfs 实例（instance）**：一个 PreLinux 环境，位于宿主用户目录下的某个前缀目录，如 `$HOME/prelinux/debian`。
- **实例前缀（instance root）**：某个实例的绝对路径。glibc 编译期嵌入一个固定容量（255 字节）的占位字符串 `/.prelinux-root-placeholder-AAAA...`，`prelinux import` 解包后由 relocate 原地二进制改写为实例真实路径（等长或更短，尾部补 NUL；实例路径长度不得超过占位容量）。ld.so 运行时只读常量字符串，无任何探测逻辑。实例路径的**实际**上限由 PT_INTERP 决定：内核只接受 ≤254 字节的解释器路径（BINPRM_BUF_SIZE），因此 aarch64 实例根 ≤222 字节、x86_64 ≤223 字节（见 5.6 与附录 A.18）。
- **内部 FHS**：实例前缀内的标准目录树（bin、sbin、lib、lib64、usr、etc、var、opt、tmp、home、root）。
- **直通路径**：不做映射、直接访问宿主的路径（/proc、/sys、/dev、宿主用户目录等）。
- **镜像（image）**：一个实例的**全自包含**可分发形态——单文件 tarball（发行版官方 rootfs + prelinux-glibc + prelinux 工具链 + 该发行版的包管理器），发布物即镜像。一个发行版对应一个文件，`prelinux import` 一个文件即得实例，无任何"源"要配。参照 docker 的 image/container 关系。
- **底座（base）**：glibc + 底座 SDK + rootfs 骨架的合称（内部概念，不出现在用户可见产物名中）。
- **适配层（adapter）**：某个包管理器的适配包（patch + 构建脚本 + 仓库配置），按包管理器组织、内含按发行版钉死的版本表（开发者概念）。
- **rootfs 来源**：镜像内发行版内容的官方出处（CI 阶段 D 下载并钉 sha256，写进镜像 manifest 与 NOTICE；用户不接触）。

## 第四章 总体架构

```
┌──────────────────────────────────────────────┐
│  适配层 adapters                               │
│  adapter-dpkg(apt) │ adapter-rpm(dnf) │ ...   │
├──────────────────────────────────────────────┤
│  底座 SDK prelinux-base                        │
│  env.sh │ relocate │ rootfs 约定           │
├──────────────────────────────────────────────┤
│  内核层 prelinux-glibc                         │
│  glibc 补丁集 │ ld.so │ 路径翻译运行时          │
├──────────────────────────────────────────────┤
│  宿主 Linux 内核（鸿蒙 PC / 任意 Linux）        │
└──────────────────────────────────────────────┘
```

层间依赖方向只允许向下。glibc 补丁层不感知包管理器的存在；底座 SDK 不感知具体宿主的差异。

## 第五章 路径映射规范（glibc 补丁层）

### 5.1 映射规则

路径翻译函数 `__prelinux_translate_path` 是全部映射逻辑的唯一实现，规则如下（与代码中的编号一致）。

0. 输入命中编译期占位符前缀 → 改写为真实实例根 + 剩余部分（glibc 构建期 prefix 派生字符串走这条）。
1. 输入恰为 `/` → 映射为实例根本身。缺少这条时，`open("/")` 后接 dirfd 相对 `*at()` 逐层下探的写法（rpm 的 fsm 落盘机）会锚定在宿主真实根上——dirfd 相对路径不产生绝对路径参数，拦截点无从改写；把 `/` 本身锚定后，这类下探全程留在实例内。
2. 输入为相对路径 → 不处理。
3. 输入已在实例根之下 → 原样放行（幂等，防止二次映射）。
4. 输入命中直通前缀表 → 不处理。
5. 输入匹配映射前缀表 → 改写为 `实例根 + path`（实例根为编译期占位符改写而来的常量，见第三章术语）。
6. 其他绝对路径（不在任何表内）→ 不处理，并视为需要人工评估的信号，记录在兼容性矩阵中。

### 5.2 映射前缀表（默认）

```
/bin /sbin /lib /lib64 /usr /etc /var /opt /srv /home /root /boot /run /tmp
```

### 5.3 直通前缀表（默认）

```
/proc /sys /dev /selinux /storage /mnt /media /data /system /vendor
```

直通表的具体条目随宿主在安装脚本中调整，但 glibc 代码中表是编译期数组，允许构建时覆盖。

### 5.4 实现约束

翻译函数必须满足：

- async-signal-safe（可在信号处理器上下文调用）。
- 不分配堆内存，使用调用方提供的栈缓冲区（上限 PATH_MAX）。
- 无锁、无全局可变状态。
- 前缀匹配用静态有序表 + 前缀比较，复杂度 O(表长)，禁止正则、禁止字符串解析库。
- 表匹配必须按路径段边界匹配：`/usr` 命中 `/usr/bin/x`，但不命中 `/usrshare/x`。

### 5.5 拦截点清单

在 glibc 内部统一入口打补丁，而不是为每个公开 API 单独包 wrapper。首批拦截点：

- 打开类：`__open64_nocancel`、`openat64` 内部入口、`fopen` 底层入口
- 元数据类：`__xstat`、`__lxstat`、`__xstat64`、`statx`、`access` / `faccessat`
- 目录类：`mkdir`、`rmdir`、`opendir` 底层入口、`chdir`、`chroot`（chroot 仅翻译后放行，不伪造权限）
- 链接类：`link`、`symlink`、`readlink`、`unlink`、`rename`
- 属性类：`chmod`、`chown`、`utimensat`、`mknod`、`truncate`
- 执行类：`execve`、`execvpe` 的路径解析（见 5.6）
- 动态加载：`dlopen` 的文件解析（经 open 入口自动覆盖，需验证）
- 其他：`realpath`、`canonicalize_file_name`、`tempnam` 系列

验收标准：strace 一个调用了上述任意 API 的测试程序，宿主侧观察到的路径必须全部落在实例前缀之下。

### 5.6 exec 路径的特殊性

内核执行 execve 时不经过 glibc 映射。因此：

- glibc 的 execve 封装必须先用翻译函数改写路径再发起 syscall。
- `execvp` / `execvpe` 的 PATH 搜索在翻译层之后进行（PATH 中的条目本身是实例内路径，翻译后命中）。
- 被执行的 ELF 的 PT_INTERP 由内核解析，必须是宿主可见的真实绝对路径。这正是“glibc 放在实例前缀下的真实路径”的原因：`<实例根>/glibc/lib/ld-linux-<arch>.so.x` 对内核是有效路径。
- **PT_INTERP 长度硬上限**：内核把解释器路径读进 BINPRM_BUF_SIZE（256 字节）缓冲区，路径 ≥255 字节时 execve 直接失败（鸿蒙 PC 返回 ENOMEM）。因此 relocate 必须保证写入的解释器路径 ≤254 字节，并在**缩短**路径时同步把 PT_INTERP 段的 `p_filesz/p_memsz` 收缩到实际字符串长度——用 254 字节占位符 relocate 过一次的包管理器（dpkg 等）会留下 292 字节的段，仅把字符串原地改短、段仍超长时内核照样拒绝执行（附录 A.18）。据此实例根上限：aarch64 ≤222、x86_64 ≤223 字节，import 在创建前按同一常量拒绝过深的路径。
- ld.so 不得包含任何运行时自定位逻辑（读 /proc/self/exe、解析自身路径等）。ld.so 是全实例所有进程的入口，稳定性优先于一切灵活性；实例根一律通过编译期占位符在安装时写入。该约束已评估，不得重新引入自定位方案。

### 5.7 ld.so 适配

- ld.so 的默认库搜索路径（trusted dirs）设为 `/lib:/usr/lib:/lib64:/usr/lib64`，经翻译层映射为实例内对应路径。Debian 系只用 /lib:/usr/lib 即可（multiarch 目录由 ld.so.conf.d 列出），但 RHEL 系依赖内建 trusted dirs 提供 /lib64:/usr/lib64（其 ld.so.conf 不列出它们），缺了会在首次 ldconfig 重建缓存后全实例缺库。
- /etc/ld.so.cache 的路径经翻译层自动落到实例内，但 ld.so 早期初始化早于部分 glibc 设施，必须验证翻译函数在 ld.so 上下文可用（翻译函数无依赖，满足要求）。
- LD_LIBRARY_PATH 语义不变，照常生效。
- ld.so 自身内部的一切文件操作走翻译函数。

### 5.8 符号链接与 readlink 策略

第一版不做“反向翻译”（不把实例内路径还原为 FHS 路径展示给程序）。已知后果：

- 程序通过 /proc/self/exe 或 readlink 拿到的是宿主真实路径。绝大多数程序只是把它再传给 open，翻译层幂等放行，行为正确。
- 少数程序将路径用于日志展示或路径比较，可能出现困惑输出。记入兼容性矩阵，不处理。

### 5.9 运行时查询接口

glibc 附加一个 `libprelinux.so`，导出：

```c
const char *prelinux_root(void);
/* 返回实例根（编译期占位符在安装时被改写为真实路径的常量字符串） */

int prelinux_translate(const char *path, char *buf, size_t buflen);
/* 按第 5.1 节规则翻译路径，返回 0 表示未改动，1 表示已翻译，-1 表示缓冲区不足 */
```

底座 SDK 的工具必须调用这两个函数获得前缀与翻译结果，禁止各自复制映射表。

### 5.10 补丁集清单与维护规则

基线版本：**glibc 2.39**（与 Debian 13 / Ubuntu 24.04 同期；覆盖 debian:12(2.36)、ubuntu:24.04 (2.39)、rocky:10 (2.39) 生态，见 9.4 基线策略）。

应用方式（鸿蒙内核兼容层先行，PreLinux 翻译层在后；单一构建覆盖两种内核，不做鸿蒙/Linux 构建变体）：

```
tar -xf glibc-2.39.tar.gz && cd glibc-2.39
while read -r p; do patch -p1 < glibc/patches/$p; done < glibc/series
```

鸿蒙内核兼容补丁是本项目自己的实现（0001–0006），依据是在鸿蒙 PC 上逐项探测到的内核行为；补丁与路径翻译补丁同处 `glibc/patches/`，一次构建覆盖两种内核。

补丁清单按组列在 `glibc/README.md`：0001–0006 是鸿蒙内核兼容，0007–0021 是路径翻译与特权软化，0022–0027 是包管理器兼容；每个补丁的作用见文件名和文件头部的说明。

**同名文件遮蔽**：以同名 .c 文件遮蔽 syscalls.list 生成物时，必须同时把该条目从 syscalls.list 删除（0013-attributes、0014-exec、0017-syscalls-list、0018-fake-ids、0019-syscall-dedup 均含此类删除）。只加文件而不删条目会形成同目标双配方，哪份胜出取决于 make 对重复目标规则的解析顺序，构建结果不可预期。检查方法：对每个遮蔽名，`grep -l '^<name>\t' sysdeps/**/syscalls.list` 必须为空。

**规模说明（对原则 5 的偏离）**：原则 5 的目标是"修改不超过 10 个文件"。实际为 45+ 文件、约 800 行新增：Linux 上每个路径类 syscall 包装都是独立小文件；chdir/chroot/execve 等 12 个包装由 syscalls.list 自动生成，无法打补丁，只能以同名 .c 文件遮蔽；内核兼容组（0001–0006）不属于路径翻译，单独成组。所有补丁都是 2–5 行 hunk，rebase 成本与文件数不成比例。

**等价性验证**：`scripts/build-glibc.sh --no-prelinux` 以同一棵树不加 `-DPRELINUX` 构建；CI 用同一棵树分别构建两种变体，确认非 PRELINUX 构建仍能完成（内核兼容补丁对 Linux 内核是空操作）。

## 第六章 底座 SDK 规范

### 6.1 组成

prelinux-base 仓库提供三样产物：

1. `env.sh` 环境变量脚本
2. `prelinux relocate` 重定位工具（含签名职责，见 6.5）
3. rootfs 骨架与打包脚本

prelinux CLI 自身的实例根不依赖安装期改写：按 `--root` > `PRELINUX_ROOT` > 自身可执行路径探测（`/proc/self/exe` 形如 `<root>/prelinux/bin/prelinux`）> 编译期占位符 的顺序解析。因此 CLI 二进制是安装期零改写的静态内容，可以在发布前预签名（见 6.5）——它是鸿蒙 PC 上第一个能直接运行的二进制，自举不依赖宿主已有工具。glibc 与 libprelinux.so 不受此限：它们是库，安装期由 relocate 改写并随即重签。

### 6.2 环境脚本 env.sh

每个实例提供 `$INSTANCE_ROOT/env.sh`，source 之后：

- `PATH` 前置 `$INSTANCE_ROOT/bin:$INSTANCE_ROOT/usr/bin:$INSTANCE_ROOT/sbin:$INSTANCE_ROOT/usr/sbin`
- 设置 `PRELINUX_ROOT`（仅作信息性用途，供脚本与用户查询；映射层不读取它，实例根已写入 glibc 常量。严禁让映射层依赖此变量——环境变量会被宿主进程继承泄漏）
- 不设置 LD_LIBRARY_PATH（ld.so 已内置实例搜索路径；仅在排障时建议用户手动设置）
- 定义 `prelinux-shell` 友好提示（PS1 前缀标识当前实例）

### 6.3 重定位工具 relocate

命令：`prelinux relocate [--root <dir>] <path> [<path>...]`

用户不再需要直接调用它：实例导入后不移动，实例内也不编译，它现在是 import 与包管理器落盘 hook 的内部工具。所有功能以 `prelinux <子命令>` 形式提供（git/busybox 风格的单一二进制多子命令），不在 PATH 中散布 `prelinux-*` 独立命令。参数为文件时逐个处理，参数为目录时递归处理整棵树（单文件与递归共用同一套逻辑，不存在两个工具）。

行为：

- ELF 可执行文件或共享库：将 PT_INTERP 改写为 `<实例根>/glibc/lib/ld-linux-<arch>.so.x`；将 RPATH/RUNPATH 中的 FHS 绝对路径改写为实例内路径。实例根由 `--root` 显式指定，缺省从目标文件路径向上探测 glibc 目录。实现是净室 ELF 引擎（附录 A.2），自己处理 PT_INTERP 延长、程序头表搬迁等边界情况。若新路径比旧槽短，PT_INTERP 段的 `p_filesz/p_memsz` 一并收缩到字符串实际长度（内核的 256 字节解释器缓冲区，见 5.6）；若解释器路径将超过 254 字节，relocate 报错并建议改用更浅的实例根，绝不写出无法执行的二进制。
- 文本文件且首行为 shebang 且 shebang 为绝对路径（如 `#!/bin/sh`、`#!/usr/bin/perl`）：改写为实例内对应路径。`#!/usr/bin/env xxx` 形式不改（PATH 已正确）。
- 绝对符号链接：内核自行解析链接目标，翻译层看不到目标字符串，绝对目标会逃出实例（RHEL 系的 /etc/pki/tls/certs/ca-bundle.crt 连锁就是例子）。relocate 把命中映射前缀表的绝对目标改写为 `实例根 + target`；相对链接、直通前缀（/proc 等）、任意宿主路径不动。与 PT_INTERP 改写一致，前提同为“实例导入后不移动”。
- 其他文件：跳过，返回成功。
- 所有路径计算调用 `prelinux_translate`，不自行实现映射规则。

**签名**（6.5 详述）：relocate 对其触碰的每个 ELF 无条件调用签名函数（进程内，vendored selfsign 实现）；在不需要签名的平台上签名段是惰性字节，保持行为统一。对不可写目标（鸿蒙已验签文件、Linux 的 ETXTBSY），回退为"读全量 → 删原文件 → 同路径重建 → 改写 → 签名"。

典型用途：

- 包管理器落盘 hook 对单文件调用。
- 适配层无法精细 hook 时的安装后整树兜底。
- 用户手工拷贝二进制进实例后的修复。

### 6.4 rootfs 骨架约定

实例目录的标准布局：

```
$INSTANCE_ROOT/
├── glibc/          # 打过补丁的 glibc 与 ld.so，不参与内部 FHS 映射语义
├── prelinux/       # CLI 与镜像元数据：bin/prelinux、image.toml、NOTICE
├── bin sbin lib lib64 usr etc var opt tmp home root run srv
├── env.sh
└── .prelinux/
    ├── instance.toml    # 实例元数据：发行版、版本、架构、适配层、glibc、创建时间、根路径
    └── compat.log       # 兼容性矩阵本地记录
```

`instance.toml` 里的发行版名与版本按树内的 `/etc/os-release`（usr-merge 系统上是 `/usr/lib/os-release`）匹配发行版表，命中就记表里的发行版名与版本（例如 `rocky` / `10`，而不是树里报的点版本 `10.2`），未命中记为 `custom` 并保留原始 `VERSION_ID`；适配层由 `usr/bin/dnf`、`usr/bin/dpkg` 是否存在判定。

`glibc` 位于实例前缀之内但独立于内部 FHS，原因有二：PT_INTERP 需要内核可见的真实绝对路径；glibc 自身的文件不应当被当作发行版内容。

### 6.5 代码签名（鸿蒙 PC）

鸿蒙 PC 强制二进制验签：未签名的可执行文件/共享库不能运行；且已通过验签的文件变为不可写（要改只能删掉重建）。

设计：

- 签名能力以 vendored 函数库的形式编进 CLI 与底座（源自 ohos-selfsign 的 0BSD C 实现）。CLI 不提供签名子命令；构建期用现场编译的 selfsign 工具校验发布物。签名产物以通过鸿蒙的校验为准，不追求与官方 binary-sign-tool 逐字节一致；官方工具签不动的文件，这里通常也能签。
- relocate 改写任何 aarch64 ELF 后重签（其他架构的签名是惰性字节，不签，见 A.10）；覆盖静态二进制与共享库。
- CI 在发布前给 CLI 本体预签（它是安装期零改写的静态二进制，见 6.1）；鸿蒙上的引导链因此为：下载镜像 → 解出已签名的 CLI → import 时 relocate 对全树现场改写 + 签名 → 冒烟及之后一切操作天然带签。
- 签名能力位于 CLI/底座（宿主集成面），glibc 补丁层零感知（原则 1）。

## 第七章 适配层规范

### 7.1 核心范式：自编译包管理器 + 落盘物理改写

包管理器本体在目标发行版的官方容器里原生编译（不交叉编译，也不用打补丁的 glibc），并在其解包落盘点内置改写 hook。理由：

- shebang 由内核直接解析，不经过 glibc，任何运行时拦截都无法可靠覆盖脚本；只能在文件落盘时物理改写。
- 落盘改写是一次性、离线、可静态验证的操作，比运行时拦截更容易调试和审计。
- 自编译包管理器还带来额外的灵活性：内置仓库镜像管理、自定义安装策略、与底座的深度集成。

**为什么否决“执行时接管（exec mediation）”方案**：posix_spawn 会在子进程内直接发起 execve syscall，绕过 glibc 封装；ld.so 代劳加载会改变 /proc/self/exe 与 auxv 语义；shebang 无运行时拦截点。该方案已被评估并否决，不得重新引入。

**特权调用软化（保留）**：chown、fchown、lchown、chmod 系列、setxattr 中涉及所有权的操作，在打补丁的 glibc 中先真实执行，若返回 EPERM 且进程 euid 非 0，则静默返回成功。实例内文件本就属于同一用户，所有权元数据无意义。此机制使 dpkg/rpm 以普通用户身份运行时不报错，同时取代了传统 fakeroot 工具的职能。

**伪造 uid/gid（补丁 0012）**：仅有软化不够。dpkg 在拆包前先自查 `getuid()==0`，非 root 时直接以 `requested operation requires superuser privilege` 退出；apt 还会在 DropPrivileges 降权到 `_apt` 之后回读校验（`APT::Sandbox::Verify::IDs` 默认开启）。因此实例进程（仅实例进程，翻译层只存在于 prelinux-glibc）内置一份每进程的 id 状态：getuid/geteuid/getgid/getegid/getgroups/getresuid/getresgid 返回伪造值（初始全 0，即 root），setuid/setgid/seteuid/setegid/setgroups/setresuid/setresgid 只更新该状态并返回成功，不触碰内核。一致性由同一份状态保证，特权软化里的 euid 判定直接读内核值，不受影响；内核侧权限语义不变。

### 7.2 适配层的职责清单

新增一个包管理器适配需要且仅需要：

1. 在对应发行版容器中原生编译包管理器本体及其依赖，版本与该发行版自带的一致（按发行版钉死在适配层版本表中；debian12=dpkg 1.21.23（bookworm-updates 已取代 1.21.22）、ubuntu2404=dpkg 1.22.6ubuntu6.x），打包前 relocate 改写（见 10.1）。
2. 在包管理器的落盘点接入 relocate hook；无法精细 hook 时，在安装事务结束后调用 relocate 兜底。
3. 提供仓库配置模板（指向发行版官方源或镜像站）。
4. 按第 6.4 节布局打包发布。

适配层不得修改 glibc 补丁层与底座 SDK 的任何文件；发现底座能力不足时，提需求给底座，不在适配层打补丁绕过。

### 7.3 镜像导入与首次修复

实例通过导入我们发布的全自包含镜像创建（发行版官方 rootfs + glibc + 工具链 + 包管理器，单文件，见第三章）：

```
prelinux import debian:12                      # 从我们的 release 下载镜像并导入
prelinux import ./prelinux-image-…tar.gz      # 本地镜像文件兜底
```

导入流程：解包镜像 → 用打补丁的 glibc 与 ld.so 覆盖实例内对应文件（要求它 glibc 版本 ≥ 该发行版构建时使用的 glibc 版本，见 9.4）→ 对全树执行一次 relocate（物理改写所有 ELF 的 PT_INTERP 与脚本 shebang + 绝对符号链接锚定
+ 签名（aarch64 才签））→ 生成 env.sh 与实例元数据 → 冒烟测试。

导入后实例内文件不再与上游逐字节一致，这是预期行为；之后包管理器安装的包由落盘 hook 自动改写，无需再次 relocate。

### 7.4 adapter-dpkg（首个适配）

- hook 点：dpkg 源码 `src/main/archives.c` 的解包写文件路径，写完一个文件后判断其类型（ELF magic 或 shebang），命中则调用 relocate 等价逻辑（第一版以子进程方式调 `prelinux relocate`，简单可靠，性能可接受）。
- **无硬链接文件系统的回退**（附录 A.19）：鸿蒙 PC 的用户主目录（hmdfs）对 `link(2)` 返回 EPERM。dpkg 在四处把硬链接当空间优化用，其中三处失败即事务中止：包载荷的硬链接条目（`archive.c` tarobject）、数据库备份 `status-old`（`atomic-file.c`）、覆盖已有文件前的 `.dpkg-tmp` 备份（`archives.c`）、dpkg 数据库版本升级（`db-ctrl-upgrade.c`）。适配层补丁 0002/0003 在 `link(2)` 返回 EPERM/EOPNOTSUPP/ENOSYS/EXDEV 时改为**复制相同内容**：字节一致，只有链接计数不同，没有任何包依赖链接计数；真实错误照旧中止。conffile 的备份链接本就只 warning，不处理。
- maintainer scripts（postinst 等）：由实例内 `/bin/sh` 执行，执行环境继承 env.sh。脚本调用的 `update-alternatives`、`ldconfig` 等全部来自实例内部，天然闭环。
- 对调用 `systemctl` 的 postinst：底座在实例 bin 中放置 no-op 的 `systemctl` 占位脚本，返回 0，避免安装事务失败。
- apt 无需源码修改，其状态目录 /etc/apt、/var/lib/apt、/var/cache/apt 经映射层自动落入实例。

### 7.5 多包管理器隔离

一个实例只使用一个包管理器。不同包管理器的数据库（/var/lib/dpkg、/var/lib/rpm）各自记账，混用会互相覆盖文件而不自知。想同时使用多个发行版生态的做法是创建多个实例，而非在一个实例内混装。

## 第八章 能力边界

以下内容必须出现在用户文档的显眼位置：

1. **静态链接二进制**不经过 glibc，映射对其无效。行为是访问宿主真实 FHS，可能报错或读写宿主文件（权限范围内）。
2. **直接发起 syscall 的程序**（典型为 Go 程序、部分 musl 静态工具）绕过映射层，同上。
3. 需要**特权能力**的功能不可用：setuid、真正的 chroot、mount、cgroups、namespace、加载内核模块。
4. 依赖**宿主内核缺失特性**的包会失败，逐个记录在兼容性矩阵。
5. 第一版**不支持 GUI 应用**（无显示服务器对接）。
6. shebang 拦截依赖 relocate 落盘改写；用户手工创建的脚本需自行使用实例内路径或 env 形式 shebang。
7. **实例内不支持编译构建**，任何语言都不行：动态 ELF 需要 relocate 才能在本实例运行，静态 ELF 则不经过路径映射。软件来源是发行版预编译包。
8. **不能升级 glibc**：apt 用 dpkg selection、dnf 用 `exclude=glibc*` 让包管理器跳过它，强行替换会让实例失去路径映射。
9. **实例不能移动或改名**：实例根写死进 glibc 与解释器路径，需要新位置就重新 import。

## 第九章 发行版与实例管理

### 9.1 实例即目录

实例是完全自包含的目录，删除即卸载。实例根在 import 时写死进 glibc 与每个 ELF 的解释器路径，因此**实例不能移动或改名**；需要另一个位置就在那里重新 import 一个实例。

### 9.2 多实例策略

第一版不提供实例编排功能（不做 init/list/shell 等实例管理命令）。多实例通过目录隔离自然获得：每个实例目录携带一份 glibc 拷贝，import 时各自写入各自的实例根，想开第二个实例就在另一个目录再跑一次 import。跨实例调用天然正确——PATH 中哪个实例的二进制被执行，内核加载的就是哪个实例的 ld.so，读到的就是哪个实例的根，互不串扰。代价是每个实例多占一份 glibc 的磁盘（数十 MB），可接受。

### 9.3 换源与换发行版

- 同一发行版换镜像源：编辑实例内 apt 配置即可，无任何特殊处理。
- 换发行版：创建新实例（换一个目录重新 import），禁止在现有实例内换源混装。
- 第一版命令形态只保留导入与入口：

```
prelinux import debian:12 [dir]   # 创建实例（默认 $HOME/prelinux/debian12）
source <dir>/env.sh               # 进入实例环境
```

### 9.4 glibc 版本策略

实例内二进制对 glibc 版本的要求是“glibc ≥ 编译该二进制时的版本”。因此：

- prelinux-glibc 的基线版本跟踪 Debian stable 的 glibc，落后不超过一个小版本。
- 支持矩阵随 glibc rebase 更新，文档中列出每个 glibc 基线能覆盖的发行版版本范围。
- 这是项目最主要的长期维护工作，patch 集最小化（原则 5）为此服务。

## 第十章 仓库结构与 CI

```
prelinux/
├── glibc/      # glibc 补丁集（patch + series + 同名文件遮蔽说明），钉住基线版本
├── prelinux/   # CLI 与底座 SDK 源码（import/relocate/info/root/translate）
├── adapters/   # 包管理器适配（dpkg/rpm），按包管理器分目录、按发行版钉版本
├── scripts/    # 本地/CI 共用构建与打包脚本（含 rootfs 获取与 OCI 铺层）
├── tests/      # sync/relocate/strace 测试套件
├── install.sh  # 引导安装脚本
└── README.md / Guide.md / Architecture.zh-CN.md / LICENSE
```

CI 要求：

- 阶段 A（runner 直跑）：glibc 双架构构建、同步测试、等价性检查，以及 relocate 对一组样本二进制（ELF、shebang 脚本、env shebang、非可执行文件）的单元测试。
- 阶段 B（容器任务）：每个发行版的适配器在其官方容器镜像内构建，`make check` 在容器内跑；adapter-dpkg 的端到端测试导入 debian/ubuntu 官方 rootfs 后 `apt install hello`，并核对新装包的 PT_INTERP 与 shebang 已被物理改写。
- 阶段 C（runner 直跑）：e2e——导入合并镜像 → 装包运行 → 重装校验 → strace 套件。
- 阶段 D（tag 触发）：rootfs 获取 + 合并镜像 + SHA256SUMS，发布到 GitHub Releases。
- 发布物命名：`prelinux-image-<distro><ver>-<arch>.tar.gz`（单镜像）。阶段 A/B 的内部件（base/adapter）不发布，只在流水线工件间传递。

### 10.1 适配层批量构建工作流

用一个四阶段的 GitHub Actions 流水线覆盖全部发行版：

```
阶段 A  build-glibc
        矩阵：x86_64 / aarch64
        产出：prelinux-base-<arch>.tar.gz
            │
阶段 B  build-adapter
        矩阵：debian:12 / ubuntu:24.04 / rockylinux:10
        每个 job 使用对应发行版的官方 container image（Actions 的 container: 字段）
        用发行版自带 glibc 原生编译该发行版的包管理器（configure && make && make check）
        打包前对所有产物执行 relocate（改写解释器与 RPATH，指向实例内路径）
        产出：prelinux-adapter-<distro>-<arch>.tar.gz
            │
阶段 C  e2e
        矩阵：发行版 × 架构
        导入合并镜像（阶段 D 同一产线）→ 装包（apt 腿 hello/tree/curl/tzdata，dnf 腿 tree/curl/tzdata）
        → 重装 → strace 验收
            │
阶段 D  release
        获取发行版官方 rootfs（fetch-rootfs.sh：ubuntu-base /
        debuerreotype 扁平 tar 钉死 sha256；rocky 官方 OCI tar 铺层）
        → 与阶段 A+B 产物合并为全自包含单镜像
        prelinux-image-<distro><ver>-<arch>.tar.gz
        → 写 manifest（rootfs 来源+sha256）与 NOTICE（许可证+源码指针）
        → 生成 SHA256SUMS（import 下载校验用）
        → 用 vendored signer 给 CLI 本体预签
        → 打 tag 发布到 GitHub Releases
```

约束：

1. **编译不依赖打补丁的 glibc**：包管理器在发行版容器内用其自带 glibc 原生编译，编译参数与上游无异，`make check` 单元测试直接在构建容器内运行。仅在打包前对全部产物执行一次 relocate 改写。唯一约束是符号版本下限——编译所用 glibc 版本不得高于实例内 glibc 的基线版本，由阶段 C 的 e2e 测试自动验证。
2. **aarch64 使用原生 ARM runner**（ubuntu-24.04-arm），禁止用 QEMU 用户态模拟做编译。官方发行版镜像大多带多架构 manifest，ARM runner 上同一 YAML 拉到的即是 arm64 镜像。
3. **版本钉死**：矩阵中的发行版镜像必须钉具体版本标签（`debian:12` 而非 `debian:latest`），并记录其构建时 glibc 版本，与 9.3 节基线策略联动。发行版升大版本时在矩阵加行而非改行，老版本继续出包，保障存量实例。
4. 选择每个发行版自己的容器编译的理由：零成本获得该发行版原生的依赖版本（dpkg 的 liblzma/zstd、dnf 的 libsolv、pacman 的 libalpm），不手工拼依赖树。

`prelinux import debian:12` 的实际行为是从发布渠道下载一个自包含镜像（发行版 rootfs + prelinux-glibc + 工具链 + 包管理器），按 SHA256SUMS 校验后解入实例目录；本地镜像导入保留为自定义来源通道（debootstrap / docker export 产物）。不实现 OCI registry 客户端：协议、认证、layer 合并的复杂度与"下载一个 tar 包"的需求不成比例。

### 10.2 分发维度与生命周期

引导包的分发维度为 **发行版大版本 × 架构**（如 debian12-x86_64、ubuntu2404-aarch64），不以包管理器为维度：

- 包管理器二进制无发行版血统，但其服务的 rootfs、源配置、依赖关系均按发行版版本划分；用户心智也是“要一个 Ubuntu 24.04”而非“要一个 dpkg”。
- 发行版小版本（ubuntu 22.04 与 24.04）必须区分为独立产物行：两者 glibc 版本、apt 版本、依赖树均不同，合并会使 9.4 的基线策略不可验证。成本仅为 CI 矩阵加一行。

生命周期策略：

1. 只支持上游仍在支持窗口内的发行版版本（Ubuntu LTS 支持期内、Debian stable + oldstable、AlmaLinux/Rocky 10 等大版本支持期内等）。
2. 上游 EOL 的版本从 CI 矩阵移除，已有 release 产物保留不删（存量实例继续可用），但不再更新。
3. 发行版发布新版本时在矩阵加行，EOL 时删行，均为低频人工 PR。

### 10.3 适配层构建脚本与补丁的维护规范

不引入 APKBUILD 式的包构建描述系统。构建对象只有个位数包管理器，不值得一套构建框架；但借鉴其目录结构思想，每个适配是一个自包含目录：

```
adapters/
├── dpkg/
│   ├── build.sh          # 元数据 + 构建逻辑合一（APKBUILD/PKGBUILD 风格）
│   └── patches/          # quilt 风格补丁，CI 中 patch -p1 依次应用
│       └── 0001-unpack-relocate-hook.patch
├── dnf/
└── pacman/
```

build.sh 遵循 APKBUILD/PKGBUILD 惯例：脚本头部用 shell 变量声明元数据（上游版本、源码地址、sha256、glibc 版本下限、目标发行版镜像），下方是 build() 与 check() 函数，CI 直接 source 执行。文件名就叫 build.sh——它是所有人都认识的入口约定，不发明新名字暗示存在框架约定。元数据不单独设文件——构建对象只有个位数，单文件自描述足够，少一个文件格式就少一套解析和校验代码。

三条规则：

1. CI 矩阵从 adapters/ 目录扫描生成，不在流水线 YAML 中手工维护适配列表；新增适配即新增目录。
2. 补丁一律用 patch 文件，禁止为维护补丁开设 fork 分支。补丁数量少且落点在 dpkg `archives.c` 这类低频变动代码上，`patch -p1` 失败即 CI 红灯，人工介入刷新。
3. 上游版本钉死在 build.sh 头部变量中，升级上游版本是一个显式 PR（改版本号 + 刷新补丁），CI 自动验证补丁可干净应用且 make check 通过。禁止跟踪上游滚动更新。

## 第十一章 安装与使用

### 11.1 安装流程

```
curl -fsSL https://<host>/install.sh | sh
```

默认都装在 `$HOME/prelinux`，`PRELINUX_HOME` 可覆盖这个位置；`install.sh` 与 CLI 都读同一个变量，CLI、镜像缓存与默认实例目录因此始终在同一棵目录树里。

install.sh 依次：

1. 探测宿主：架构、`$HOME` 可写、`curl` 可用。
2. 按参数选择发行版（默认 debian:12）。
3. 从有序 release 渠道列表下载对应镜像（默认 GitHub Releases latest；`PRELINUX_RELEASE_URLS` 环境变量可覆盖为自建 HTTP 服务或其他镜像），缓存镜像的 SHA256SUMS 校验由随后的 `prelinux import` 完成（见 11.1）。
4. 解出 CLI 到 `$PRELINUX_HOME/bin`（与镜像缓存、实例同目录）。install.sh 不改动用户的 shell 配置，PATH 由用户自行添加（安装结束时会打印要加的那一行）。
5. `prelinux import <发行版>` 完成实例导入（镜像已在缓存中时直接复用）。CLI 本体发布前预签名，鸿蒙 PC 开箱即跑，自举不依赖宿主已有签名工具。

**下载策略**：install.sh 用单连接 curl 下载镜像（`--retry 3`）。它只依赖 curl，能自行跟随 GitHub Release 的 302 跳转到 CDN，重试时重新解析；镜像体积 50–110 MB，单连接下载即可接受。

**镜像格式**：只发布 `tar.gz`。gzip 是鸿蒙 PC 系统镜像里唯一能指望的压缩工具，单一格式让发布、安装与文档只有一条路径；zstd 版本约小 35%，但 import 仍接受它（用户自行压缩的本地镜像），只是不再发布。

**镜像缓存的语义**：

- 下载落点是 `$PRELINUX_HOME/cache/prelinux-image-<distro><ver>-<arch>.tar.gz`；install.sh 与 import 共用同一路径，install.sh 下好的镜像 import 直接复用。
- **复用必须经过校验**：import 每次都先取发布物的 SHA256SUMS，缓存文件只有与该清单里的记录一致才会被使用（见 11.1 的约定）。因此“渠道可达但镜像已在本地”不会产生第二次下载，也不会绕过校验。
- 发布渠道完全不可达时，import **不会**拿缓存文件凑数：它报错并指出缓存里的路径，用户若信任该文件，用文档里的本地镜像入口显式导入（`prelinux import ./prelinux-image-… </dir>`）。未经校验的自动导入不在设计内。
- install.sh 下载时使用的渠道（`PRELINUX_RELEASE_URLS` 的第一项）会以 `PRELINUX_RELEASE_URL` 传给 import，使两个阶段用同一渠道；否则自建渠道或私有仓库场景下 import 会退回默认地址而 404。

### 11.2 卸载

删除 `~/prelinux` 即可（命令、镜像缓存与所有实例都在里面）；install.sh 不提供卸载子命令，也不改动 shell 配置。

### 11.3 版本号

发布版本用语义版本（`vMAJOR.MINOR.PATCH`），描述的是**契约**，不是镜像内容：

| 变更 | 版本位 |
|---|---|
| 镜像格式、CLI 参数、实例布局的不兼容变化 | MAJOR |
| 新增发行版、新增子命令、新增能力 | MINOR |
| 修复、重打镜像（发行版 snapshot 更新、同一 glibc 大版本内的 rebase） | PATCH |

镜像不单独记录格式版本：CLI 是从镜像里取出来的，两者永远同源，镜像格式的不兼容变化只通过 MAJOR 表达。首个正式版是 `v1.0.0`。

tag 打 `vX.Y.Z`，CI 在发布前校验 `prelinux --version` 与该 tag 一致；`releases/latest/download/` 因此始终可用，需要复现的用户改用 `releases/download/vX.Y.Z/`。

## 第十二章 给实施者的注意事项

1. 先读第五章再写任何代码。映射规则是全局一致性的根基。
2. glibc 补丁全部用 `#ifdef PRELINUX` 包裹，并附带一个不带该宏时行为与上游完全一致的构建测试，防止补丁污染默认行为。
3. 不要在 glibc 补丁层引入任何堆分配与锁，理由见 5.4。
4. relocate 的 ELF 引擎是净室实现（A.2）。PT_INTERP 改写涉及 PT_LOAD 段扩展，个别二进制会失败，失败时要报错而不是写坏文件。
5. adapter-dpkg 的落盘 hook 第一版以子进程调 relocate 实现，不要为性能过早做内嵌优化。
6. 遇到文档未覆盖的决策，默认选择“对宿主更透明、对上游更诚实”的方案，并把决策回填进本文档。

---

## 附录 A 设计决策记录

本附录记录与正文配套的设计决策：每条给出结论与理由，供后续维护者理解"为什么这样做"。与正文冲突时以正文为准。

1. **内核兼容补丁的归属**：鸿蒙内核的 syscall 兼容（SIGSYS、clone3、rseq 等）由本项目自己实现，依据是探测到的内核行为（0001–0006），对 Linux 内核是空操作；补丁与路径翻译补丁同处 `glibc/patches/`，一次构建覆盖两种内核（原则 1）。
2. **relocate 为独立实现**：relocate 只做本项目需要的事（追加 PT_LOAD 页承载超长 PT_INTERP、搬迁程序头表、替换 .dynstr 改写 RPATH），不引入外部依赖，并保持"失败时报错，不写出无法执行的二进制"的约定。
3. **libprelinux.so 与 glibc 补丁共享同一实现**：`prelinux/src/translate.c` 以 `-DPRELINUX` 编入 glibc 供内部拦截点使用，同时独立编译为 `glibc/lib/libprelinux.so` 供底座工具链接；CLI 直接编译同一源文件（它必须能在实例可用之前运行）。单一源文件满足 5.9 节"映射表只有一份"。
4. **CLI 形态**：单一静态链接二进制，busybox 风格子命令（`import`、`relocate`、`info`、`root`、`translate`），可在任何宿主上先于实例运行。
5. **chdir/chroot/execve 的实现方式**：这三个函数在上游由 syscalls.list 自动生成，无法打补丁；从 syscalls.list 移除条目，改为常规 .c 实现并加入翻译。
6. **实例根占位容量 255 字节**：占位符是一个路径分量，受 NAME_MAX 限制，容量为 255 字节（254 可用 + NUL）。实际可达的实例根更短：安装后的 PT_INTERP 指向 `<根>/glibc/lib/ld-linux-*.so.*`，内核只接受 ≤254 字节的解释器路径，因此 aarch64 上限 222、x86_64 上限 223，import 提前拒绝。
7. **前缀派生字符串的 Rule 0**：构建期由 prefix 派生的字符串（libdir、gconv/locale 路径、RUNTIME_LINKER）是"占位符 + 后缀"形式，长度超过任何真实根，无法原位改写。翻译层因此增加第 0 条规则：以占位前缀开头且第 254 字节为 '/' 或 NUL 的路径，运行时替换为真实实例根。内核直接消费的 PT_INTERP 不适用此规则，仍由 relocate 物理改写。
8. **libc6 与仓库的版本差不在导入时修复**：直接升级 libc6 会使实例失效——ld.so 与 libc 必须同构，新的（上游内容）libc 一落盘，之后启动的进程就失去翻译层。早期版本在导入时下载仓库版 libc6 重打包注入，这会把一次安装变成依赖网络的多步操作，已移除。现在只把 libc6、libc6-dev、libc-bin 一并 hold，并在文档里说明：镜像快照落后于仓库时，`libc6-dev` 会因版本依赖装不上，重新构建/下载镜像即可，不要去升级实例里的 libc6。
9. **实例内不编译**：编译要么需要在实例里装编译器与头文件（`libc6-dev` 精确绑定 `libc6`，而非 root 环境里升级 `libc6` 会在事务中途丢掉伪造 root），要么需要为 `gcc`/`ld` 打补丁。两条都不划算，因此实例只运行发行版预编译包；需要自己编译的程序应在宿主或别的机器上构建，再以静态二进制或自建包的形式带入。
10. **签名范围**：relocate 自动签名它改写的每个 aarch64 ELF（鸿蒙 PC 仅此架构，其他架构的签名是惰性数据）；CLI 不提供显式签名入口，只有内部函数与构建期工具；没有开关或环境变量。
11. **CLI 运行时根探测**：`--root` > 环境变量 > `/proc/self/exe`。安装期零改写，CLI 可被预签名，鸿蒙上的自举不依赖宿主已有的签名工具。
12. **`/` 映射为实例根**：否则 `open("/")` 之后的 dirfd 相对 `*at()` 调用没有绝对路径可供拦截，rpm 的 fsm 会把载荷写到宿主真实根。deb 系走绝对路径不受影响；这条规则统一封堵此类逃逸。
13. **绝对符号链接锚定**：符号链接目标由内核解析，翻译层无法介入。relocate 把命中映射前缀的绝对链接目标改写为实例内路径；包管理器新装的链接不在落盘 hook 覆盖范围内，由全树 relocate 兜底。
14. **ELF 追加页落位**：relocate 加长 PT_INTERP 时追加的页（新 interp、dynstr、扩展后的程序头表）必须让 `load_bias + e_phoff` 指向程序头表：qemu-user 按此计算 AT_PHDR，内核也按此语义定位。对 ET_DYN 该规则等价于 `vaddr == 文件偏移`，对 ET_EXEC 必须再加上 load bias（附录 A.24）。
15. **伪造 uid/gid**：实例进程报告一个伪造的 uid/gid 状态（初始 root，降权与恢复只改该状态），apt 的回读校验保持一致；见 7.1 末节，无配置项。
16. **uname 打桩进 glibc**：鸿蒙内核的 uname 报 "HarmonyOS"，Autotools、CMake 与预编译程序按 OS 名识别平台时中断。sysname 非 "Linux" 时改写为 "Linux"，在 Linux 内核上是空操作。
17. **CLI 用 musl 静态链接**：静态 glibc 二进制在鸿蒙 PC 上无法启动（内核会把 glibc 启动期注册的 rseq 等合法 syscall 以 SIGSYS 杀掉），musl 是鸿蒙原生用户态 libc。实例内 SDK 库 libprelinux.so 仍用 glibc 编译，它只服务实例内的 glibc 进程。
18. **PT_INTERP 长度与段长上限**：内核把解释器路径读入 256 字节缓冲区，路径 255 字节起 execve 返回 ENOMEM（254 可用）；段长同样受限，只改字符串而不收缩 `p_filesz` 的二进制仍会因段长超限被拒绝。`prelinux_elf_set_interp` 在字符串放得下时把 `p_filesz/p_memsz` 收缩到 `strlen + 1`；relocate 对段长大于字符串的二进制也触发一次改写；解释器路径将超 254 时报错。
19. **镜像不含硬链接**：hmdfs 对 `link(2)` 返回 EPERM，带硬链接条目的镜像会在导入解包中途失败。打包脚本统一使用 GNU tar 的 `--hard-dereference`，硬链接存为独立副本（内容一致，只损失链接计数）。dpkg 侧见 A.20。
20. **dpkg 的 link 回退**：dpkg 在包载荷硬链接、`status-old`、`.dpkg-tmp`、数据库升级四处使用硬链接，在不支持硬链接的文件系统上全部失败。适配补丁在 EPERM/EOPNOTSUPP/ENOSYS/EXDEV 时改为复制内容，其余 errno 照常中止。
21. **预签名自举链**：镜像内的 CLI 发布前已签名，安装时直接运行；打包时用现场编译的 selfsign 工具校验签名，CLI 本身不再提供验签子命令。改写过或新安装的 ELF 由 relocate 现场重签，签名后的文件在内核层面不可写，改写走"删除重建"。
22. **不直连 fchmodat2**：鸿蒙内核不实现 fchmodat2（aarch64 452），按安全策略以 SIGSYS 杀进程而非返回 ENOSYS。glibc 2.39 在 aarch64 上默认直接调用它，需要逐条处理 INLINE_SYSCALL 调用点：flag 0 用 open + fchmod，`AT_EMPTY_PATH` 用 `fchmod(fd)`，其余走 O_PATH + /proc；负 fd 返回 EBADF。
23. **hmdfs 大小写不敏感**：仅大小写不同的路径是同一个目录项。包内存在这种撞名（内核头文件 `xt_CONNMARK.h` 与 `xt_connmark.h`），dpkg 延迟改名时第二个报 ENOENT。适配补丁在 rename 返回 ENOENT 且目标已存在时按撞名处理：warning 并保留已落盘副本。用户侧表现为撞名路径只留一份。
24. **ET_EXEC 的追加页按 load bias 落位**：内核的 `AT_PHDR` 是相对 load bias 的文件偏移（`(首个 PT_LOAD 的 vaddr − offset) + e_phoff`）。按 `vaddr == offset` 落页对 ET_DYN 成立，对 ET_EXEC 会整体低一个 bias，AT_PHDR 与解释器落入未映射区。追加页因此先定文件偏移、再按 `vaddr = load_bias + offset` 落位；relocate 可原地修复旧布局的实例。
25. **xattr 与权限元数据归一**：hmdfs 没有 xattr 且忽略 chmod（目录恒为 2771，文件在 660/770 之间二值化，setuid 位无效）。`setxattr` 的 EPERM 报为 EOPNOTSUPP，`getxattr` 的 ENOENT 归一为 ENODATA，ACL 感知的工具（`cp -a`、`rsync -a`）因此走"无 ACL → chmod"的正常路径。实例内的权限与属主元数据不可信，见 Guide 的限制表。
26. **rpm 硬链接集延迟复制**：rpm 先打开第一个成员，把其余成员 `linkat` 上去，而 cpio 把内容交给带内容的那个成员，可能在链接之后才写入。链接失败时只登记（源与目标仍是 `;tid` 临时名），等载荷遍历结束、commit 之前统一复制；内容、权限与 relocate 行为与硬链接集一致。
27. **不直连 close_range**：鸿蒙内核对 `close_range(2)`（aarch64 436）同样以 SIGSYS 杀进程。公共符号改用 glibc 自带的用户态实现；glibc 的 `posix_spawn` 在 spawni.c 的 closefrom 动作中直接发该 syscall，改为在 PRELINUX 下走 `__closefrom_fallback`，否则每个 Popen 子进程都会在 exec 前被杀死。
28. **xattr 系列必须翻译**：`listxattr`/`llistxattr` 与 `removexattr`/`lremovexattr` 若不过翻译层，内核看到的是宿主路径；`copy_attr` 一类实现会因宿主路径不存在而报 ENOENT，表现为 `cp -a` 对目录"无法保留权限"。四个包装按 getxattr 的模式补齐并删除 syscalls.list 条目。
29. **dpkg-deb 的权限检查放宽**：dpkg-deb 要求控制目录恰好 0755、维护者脚本恰好 0555，而 hmdfs 强制目录带 setgid 且忽略 chmod，设备上无法满足。保留类型检查，精确模式检查降为 warning（归档中的模式仍会被规范化）。
30. **导入只做覆盖，不追版本**：libc6/libc-bin 的仓库版本对齐已从导入流程移除（见 A.8）。导入只把 glibc 覆盖到发行版文件上，再钉住包管理器：apt 用 dpkg selection 固定 libc6/libc6-dev/libc-bin，dnf 用 `exclude=glibc*`。覆盖目标可能是已签名的文件（鸿蒙内核使签名后的文件不可写），所以遇到 EPERM/EACCES 时删除重建，由随后的 relocate 重新签名。
31. **dpkg 解包结束时刷新 loader cache**：apt 把所有包 `--unpack` 完再起另一个 dpkg 进程 configure，而 `ca-certificates` 的 postinst 要运行刚解包的 `openssl`，`python3.11-minimal` 的 postinst 要运行刚解包的 `python3.11`。适配补丁在解包进程退出时调用一次实例的 `ldconfig`，否则 `libssl.so.3`、`libexpat.so.1` 这类刚解包的库还没进缓存，装 curl 或 python3 都会失败。不能在每个 `.so` 落盘时刷：那时文件还叫 `*.dpkg-new`，改名发生在之后且不再触发写钩子，`ldconfig` 看不到它们。
