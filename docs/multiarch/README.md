# 多架构实现说明

## 范围与状态

本分支基于官方 iSH 历史继续开发，保留原有 i386 guest，并增加独立的 AArch64 Linux guest 执行路径。当前目标是提供可嵌入 iOS 与 watchOS 应用的可移植核心，而不是替代完整的 iSH 应用层。

该实现仍处于实验阶段。真实 Alpine AArch64 环境已经验证 shell、基础文件操作、进程创建与等待、信号投递以及本机 TCP 连接，但尚未覆盖完整的 AArch64 指令集与 Linux 系统调用集合。

## 架构边界

| 层次 | 当前支持 | 约束 |
| --- | --- | --- |
| guest 指令集 | i386、AArch64 | 两套 CPU 状态与执行路径相互隔离 |
| guest Linux ABI | i386 32 位、AArch64 64 位 | 系统调用号、结构体与寄存器约定按 guest 架构编码 |
| host 平台 | macOS 测试、iOS、watchOS | host 指针宽度不能泄漏进 guest ABI |
| Apple 架构 | iOS `arm64`、watchOS `arm64_32`、watchOS `arm64` | `arm64_32` 是 watchOS host ABI，不是 32 位 AArch64 guest |

guest 地址、host 指针和 Linux wire 数据分别使用明确宽度的类型。AArch64 guest 使用稀疏 48 位地址空间，内存访问通过页表和用户内存复制边界完成；文件、任务和信号服务继续复用官方内核对象，但不直接暴露架构特定的数据布局。

## 模块分工

- `guest/memory/`：guest 地址空间、稀疏页表和用户内存访问契约。
- `guest/aarch64/`：AArch64 CPU 状态、指令解码、执行语义、ELF64 装载与 Linux ABI 编码。
- `guest/linux/`：与 guest 架构无关的内存、文件和系统调用服务边界。
- `kernel/aarch64*.c`：将 AArch64 进程生命周期接入官方任务、文件系统、信号与调度设施。
- `tools/apple-core-gate.sh`：以严格警告配置构建并检查三个 Apple core slice。
- `tests/aarch64/`：指令、ABI、运行时、并发和真实发行版冒烟测试。

## 构建与测试

先初始化子模块，再建立常规 Meson 构建目录：

```sh
git submodule update --init
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Apple core 门禁需要 Xcode SDK。脚本分别构建 iOS `arm64`、watchOS `arm64_32` 和 watchOS `arm64`，验证 Mach-O 平台，再合并并检查 watchOS 静态库架构：

```sh
MESON=meson tools/apple-core-gate.sh
```

这个门禁验证的是可复用 AArch64 core slice，不代表完整 watchOS 应用已经完成接入。

## Alpine AArch64 冒烟

验收使用 Alpine 3.24.1 AArch64 minirootfs：

- 官方归档：`https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz`
- SHA-256：`f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259`

下载并校验归档后，可在仓库外生成 fakefs：

```sh
build/tools/fakefsify \
    /tmp/alpine-minirootfs-3.24.1-aarch64.tar.gz \
    /tmp/ish-a64-alpine
tests/aarch64/alpine-smoke.bash build/ish /tmp/ish-a64-alpine
```

冒烟脚本不会下载或提交 rootfs。它会拒绝与已有 `ish` 进程重叠运行，并为每个 guest 命令设置硬超时；退出时会清理自己启动的服务和残留进程。

在开发使用的 macOS 与 Xcode 环境中，当前验证结果为：

- AArch64 单元、运行时、线程以及地址/未定义行为检查通过。
- iOS `arm64`、watchOS `arm64_32` 和 watchOS `arm64` core 编译与链接通过。
- 常规 Meson 测试 91 项中通过 89 项；`float80` 与 `e2e` 的 `shldl` 差异是当前 host/QEMU 组合下已有的基线差异。
- Alpine AArch64 的动态 `/bin/sh`、文件操作、子进程等待、信号终止和本机 HTTP 获取通过。

## 来源、许可与独立实现边界

本仓库从官方 `ish-app/ish` 提交 `997642f3787cc63e65f7134b7bb0362c74bff8e0` 延续开发，保留其 Git 历史、版权声明以及 `LICENSE.md`、`LICENSE.IOS`。使用或分发时仍须遵守这些文件中的许可条件。

多架构改动依据公开的 Arm 指令集、Linux AArch64 ABI、ELF 与 Apple 平台 ABI 资料独立设计和实现。实现过程中以官方基线为集成边界，不引入其他衍生实现的源代码。提交前的相似性审查会从候选重合中扣除官方基线已有内容；剩余命中仅为通用控制流和错误检查形式，未发现具有实现语义的连续代码重合。

## 当前限制

- AArch64 指令和 Linux 系统调用覆盖以运行现有工作负载为驱动，尚不等同于完整内核兼容层。
- 多线程进程跨架构替换映像尚未实现；不安全的该类 `exec` 会返回 `EBUSY`。
- `DC ZVA` 当前通过 `DCZID_EL0.DZP` 声明不可用，guest libc 会回退到普通清零路径。
- 网络验证目前只覆盖基础 TCP 客户端路径，不代表 UDP、IPv6 或全部 socket 选项均已实现。
- Apple 门禁只验证 core 的 ABI、编译与链接；应用生命周期、界面和沙箱能力由集成方负责。
