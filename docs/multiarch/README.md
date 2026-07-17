# 多架构实现说明

## 范围与状态

本分支基于官方 iSH 历史继续开发，保留原有 i386 guest，并增加独立的 AArch64 Linux guest 执行路径。当前目标是逐步形成可嵌入 iOS 与 watchOS 应用的可移植核心，而不是替代完整的 iSH 应用层。现阶段生成的静态库和 XCFramework 是构建、ABI 与最终链接验证产物，并非已经发布的公共 SDK。

该实现仍处于实验阶段。真实 Alpine AArch64 环境已经验证 shell、基础文件操作、进程创建与等待、信号投递以及本机 TCP 连接，但尚未覆盖完整的 AArch64 指令集与 Linux 系统调用集合。

## 架构边界

| 层次 | 当前支持 | 约束 |
| --- | --- | --- |
| guest 指令集 | i386、AArch64 | 两套 CPU 状态与执行路径相互隔离 |
| guest Linux ABI | i386 32 位、AArch64 64 位 | 系统调用号、结构体与寄存器约定按 guest 架构编码 |
| host 平台 | macOS 测试、iOS、watchOS | host 指针宽度不能泄漏进 guest ABI |
| Apple 切片 | iOS device `arm64`；watchOS device `arm64_32`、`arm64`；watchOS Simulator `arm64`、`x86_64` | `arm64_32` 是 watchOS host ABI，不是 32 位 AArch64 guest；device `arm64` 的 minOS 为 26.0，其余 watchOS 切片为 10.0 |

guest 地址、host 指针和 Linux wire 数据分别使用明确宽度的类型。AArch64 guest 使用稀疏 48 位地址空间，内存访问通过页表和用户内存复制边界完成；文件、任务和信号服务继续复用官方内核对象，但不直接暴露架构特定的数据布局。

## 模块分工

- `guest/memory/`：guest 地址空间、稀疏页表和用户内存访问契约。
- `guest/aarch64/`：AArch64 CPU 状态、指令解码、执行语义、ELF64 装载与 Linux ABI 编码。
- `guest/linux/`：与 guest 架构无关的内存、文件和系统调用服务边界。
- `kernel/aarch64*.c`：将 AArch64 进程生命周期接入官方任务、文件系统、信号与调度设施。
- `tools/apple-core-gate.sh`：构建一个 iOS device 切片和四个 watchOS 切片，检查严格 core、完整静态库消费者、宿主 ABI 与 Apple 二进制元数据，并生成 watchOS device/Simulator XCFramework。
- `tools/apple-watch-package.sh`：供 Xcode 打包 Scheme 调用 Apple 门禁，并把当前 Xcode 平台的通用静态库放入构建产品目录。
- `tests/aarch64/`：指令、ABI、运行时、并发和真实发行版冒烟测试。

## 构建与测试

先初始化子模块，再建立常规 Meson 构建目录：

```sh
git submodule update --init
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

### AArch64 执行后端

Meson 选项 `-Daarch64_backend=auto|c|threaded` 控制 AArch64 guest 的默认执行后端。`auto` 是默认值：AArch64 host（包括 watchOS `arm64_32`）选择 threaded-code，`x86_64` host 选择 C；`c` 可显式固定到正确性 oracle，`threaded` 可显式固定到快速后端，但非 AArch64 host 会在配置阶段拒绝该组合。

两种选择都会编译并保留 C 执行器。threaded-code 只加速已经独立实现并经过差分测试的指令，未提速指令继续回落到 C oracle；因此选择快速后端不会从归档中移除 `aarch64_execute`，`x86_64` Simulator 也始终具有可用的 C 路径。

原生 AArch64 host 还会注册 `aarch64_backend_performance` 微基准。它在同一个二进制中显式运行 C oracle 与 threaded-code，覆盖纯快速分派、混合 C 回落和低解码成本的 NOP 调度三种稳定命中工作负载。请使用 release 构建运行：

```sh
meson setup build-perf --buildtype=release -Daarch64_backend=auto
meson compile -C build-perf aarch64_backend_benchmark
meson test -C build-perf --benchmark --verbose
```

基准会预热 TLB 与 threaded 缓存，自适应到两种后端的单次采样均至少 100 ms，再以 5:5 的交替次序采集十组 C/threaded 配对样本，报告每条 guest 指令的中位耗时、MAD 离散度和配对加速比。任一 MAD 超过 10% 时只会提示本轮不宜作为回归基线，不会把环境噪声判为代码失败。结果只用于同一台、负载稳定机器上的提交间比较，不设置跨机型或共享 CI 的性能阈值，也不代表完整 Linux 软件或 Apple 设备运行时性能。

Apple 门禁需要 Xcode SDK、Meson 与 Ninja。它构建以下五个 Apple 切片：

- iOS device `arm64`，minOS 15.0；
- watchOS device `arm64_32`，minOS 10.0；
- watchOS device `arm64`，minOS 26.0；
- watchOS Simulator `arm64`，minOS 10.0；
- watchOS Simulator `x86_64`，minOS 10.0。

门禁显式使用 `aarch64_backend=auto`，并要求 iOS `arm64`、watchOS `arm64_32`/`arm64` 与 Simulator `arm64` 选择 threaded-code，Simulator `x86_64` 选择 C。它同时核对生成的配置宏、core 与完整归档中的 C/threaded 对象和公开符号，并对五个切片严格编译函数指针 ABI probe；iOS 还以 `arm64e -O2` 检查 threaded 间接调用的指针认证指令。

可以直接运行：

```sh
MESON="$(command -v meson)" \
NINJA="$(command -v ninja)" \
tools/apple-core-gate.sh
```

每个切片都会进行两层构建：严格警告配置的 AArch64 core，以及包含 kernel、fs、platform、指令模拟器与 fakefs 的完整静态库。完整库随后接受两种最终链接检查：普通消费者通过对真实入口 `become_first_process` 的强引用按需解析归档，但不会调用该入口；另一个消费者强制解析三份静态归档的全部成员。门禁还会检查：

- host 指针、函数指针、`long`、`size_t` 和文件偏移等 ABI 宽度；
- AArch64 auto 后端的五切片选择、C oracle 的永久归档保留，以及 threaded 缓存项的 ILP32/LP64 函数指针宽度；
- `arm64_32` gadget 表的 4 字节指针重定位，以及汇编对 C 指针字段的 ILP32/LP64 访问宽度；
- Mach-O 的 device/Simulator 平台和各切片 minOS；
- 静态归档的架构集合、必要成员和禁用符号；
- 三份 XCFramework 是否同时包含 watchOS device 与 Simulator 变体。

默认产物位于：

```text
build-apple-core/universal/watchos/libish_aarch64_core.a
build-apple-core/universal/watchos/libish.a
build-apple-core/universal/watchos/libish_emu.a
build-apple-core/universal/watchos/libfakefs.a
build-apple-core/universal/watchsimulator/libish_aarch64_core.a
build-apple-core/universal/watchsimulator/libish.a
build-apple-core/universal/watchsimulator/libish_emu.a
build-apple-core/universal/watchsimulator/libfakefs.a
build-apple-core/xcframeworks/libish.xcframework
build-apple-core/xcframeworks/libish_emu.xcframework
build-apple-core/xcframeworks/libfakefs.xcframework
```

为了兼容已有调用方，脚本还会在 `build-apple-core/` 根目录保留 `libish_aarch64_core-watchos.a`、`libish-watchos.a`、`libish_emu-watchos.a` 与 `libfakefs-watchos.a` 这四份 device 通用归档副本。

device 通用归档包含 `arm64_32` 与 `arm64`，Simulator 通用归档包含 `arm64` 与 `x86_64`。三份 XCFramework 目前都没有公共头文件（Headers）、模块映射（module map）或稳定的公共 C API；它们只是门禁生成的二进制容器，不能称为公共 SDK，也不能声称可以不经接口设计和集成验证就直接接入 ETOS 或其他应用。

这个门禁会交叉构建并链接最小 Mach-O 消费者，并静态检查后端选择、归档符号与 `arm64e` 指针认证指令，但不会启动这些消费者、watchOS Simulator 或 guest。它不衡量 threaded-code 的运行性能，也不验证应用生命周期、界面、签名、沙箱、entitlement、真机运行或 App Store 交付。

### Xcode Scheme 验收

工程提供两个共享 Scheme：

- `iSHCore-watchOS` 是 aggregate 打包 Scheme。它调用 `tools/apple-watch-package.sh`，生成四个 watchOS 切片、通用静态库和三份 XCFramework；它本身没有 Xcode 可运行产品。
- `iSHWatchLinkSmoke` 是最小 watchOS application 类型的链接夹具。它只有一个 C 入口，依赖 `iSHCore-watchOS`，并最终链接 `libish.a`、`libish_emu.a`、`libfakefs.a` 及系统 SQLite。它没有 SwiftUI、图标或用户界面，不会在构建时运行 guest，也不代表完整或可交付的 Watch App。

先验证打包 Scheme：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHCore-watchOS \
    -configuration Release \
    -sdk watchos \
    CODE_SIGNING_ALLOWED=NO \
    build
```

随后按切片验证最终 Xcode 链接。两个 watchOS device 切片具有不同的 minOS，因此必须分别执行：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchos \
    ARCHS=arm64_32 \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    build

xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchos \
    ARCHS=arm64 \
    WATCHOS_DEPLOYMENT_TARGET=26.0 \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    build
```

Simulator 两个切片同为 minOS 10.0，也逐一执行以保留清晰的架构证据：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchsimulator \
    ARCHS=arm64 \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    build

xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchsimulator \
    ARCHS=x86_64 \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    build
```

这些命令只使用 SDK 进行编译和链接，不要求安装或启动 Simulator runtime。Xcode Scheme 的交叉构建中间产物位于 `ISH_WATCH_ARTIFACT_ROOT`，默认展开为 DerivedData 下的：

```text
Build/Intermediates.noindex/iSH.build/iSHWatchArtifacts/<配置><平台后缀>/
```

其中仍使用与命令行门禁相同的 `universal/watchos/`、`universal/watchsimulator/` 和 `xcframeworks/` 子目录。当前平台供 LinkSmoke 使用的三份通用归档会复制到：

```text
Build/Products/<配置><平台后缀>/iSHWatchLibraries/
```

LinkSmoke 的验证包位于 `Build/Products/<配置><平台后缀>/iSHWatchLinkSmoke.app/`；这个路径下出现 `.app` 只表示 Xcode 完成了 application 类型的最终链接，不表示该包拥有界面、可以启动 guest 或能够作为产品交付。

可以用以下命令查看本机的完整展开路径，而不依赖 DerivedData 的随机目录名：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHCore-watchOS \
    -configuration Release \
    -sdk watchos \
    -showBuildSettings \
    | grep -E 'ISH_WATCH_(ARTIFACT_ROOT|LIBRARY_DIR) ='
```

## Alpine AArch64 冒烟

验收使用 Alpine 3.24.1 AArch64 minirootfs：

- 官方归档：`https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz`
- SHA-256：`f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259`

下载并校验归档后，可在仓库外生成 fakefs：

```sh
build/tools/fakefsify \
    /tmp/alpine-minirootfs-3.24.1-aarch64.tar.gz \
    /tmp/ish-a64-alpine
tests/aarch64/alpine-smoke.bash build/ish /tmp/ish-a64-alpine
```

冒烟脚本不会下载或提交 rootfs。它会拒绝与已有 `ish` 进程重叠运行，并为每个 guest 命令设置硬超时；退出时会清理自己启动的服务和残留进程。

本次发布候选在开发使用的 macOS 与 Xcode 环境中完成了以下门禁：

- 默认非交叉 `kernel=ish` 配置当前登记的 Meson 测试，在常规、ASan+UBSan 与 TSan 配置中均通过。
- iOS device `arm64`，watchOS device `arm64_32`/`arm64` 与 Simulator `arm64`/`x86_64` 的 core、完整静态库、普通消费者、全归档消费者、ABI 和二进制元数据门禁通过，并成功生成包含 device/Simulator 变体的三份 XCFramework。
- Alpine AArch64 的动态 `/bin/sh`、文件操作、子进程等待、信号终止和本机 HTTP 获取通过；冒烟结束后没有残留 `ish` 进程。

## 来源、许可与独立实现边界

本仓库从官方 `ish-app/ish` 提交 `997642f3787cc63e65f7134b7bb0362c74bff8e0` 延续开发，保留其 Git 历史、版权声明以及 `LICENSE.md`、`LICENSE.IOS`。使用或分发时仍须遵守这些文件中的许可条件。

多架构改动依据公开的 Arm 指令集、Linux AArch64 ABI、ELF 与 Apple 平台 ABI 资料独立设计和实现。实现过程中以官方基线为集成边界，不引入其他衍生实现的源代码。最终发布前必须以官方基线到发布提交为范围，重新执行来源、相似性、受跟踪文件路径与提交信息审计；无法用公开规范解释的显著重合必须重写或明确标注来源。

## 当前限制

- AArch64 指令和 Linux 系统调用覆盖以运行现有工作负载为驱动，尚不等同于完整 ISA 或内核兼容层；当前已实现 32/64 位 `CASP`、`CASPA`、`CASPL` 与 `CASPAL`，但尚未覆盖完整 `FEAT_LSE`，guest 的 `AT_HWCAP` 暂不宣告 `HWCAP_ATOMICS`。
- 未支持的 AArch64 指令会安全投递 `SIGILL`，未知 Linux 系统调用会返回 `ENOSYS`。
- AArch64 `futex` 已支持 `WAIT`、`WAKE`、`REQUEUE`、匿名共享后备的跨进程键、robust list 与 clear-child-tid 退出清理。ELF 主映像和解释器的文件页以活跃 inode 代际与绝对文件字节偏移建键，因此 fork 后以及独立装载到不同虚拟地址的同一文件别名可以协作。fork 后的私有文件页会先共享同一受同步保护的物理后备；普通写、CAS 与有效 STXR 都在首次写前换入匿名副本，共享 futex 写固定也使用同一 COW 入口，再进入当前 `mm` 的匿名键域。跨页 COW 先完整分配再提交，失败不会留下部分后备、来源或映射世代变化；futex 键准备遇到该分配失败时返回 `ENOMEM`。同一宿主文件若经不同 guest mount 打开，当前会保守地产生不同 inode 代际并形成安全假阴性。i386 的非 PRIVATE `WAIT`/`WAKE`/`REQUEUE` 也会按稳定后备身份建键：匿名共享映射可跨 fork 后的不同 `mm` 协作，独立普通文件映射按 inode 代际与实际文件字节偏移匹配；robust list 和 clear-child-tid 的退出唤醒沿用相同共享键。
- i386 与 AArch64 均实现系统调用 449 的基础 `futex_waitv`：支持 1～128 个 U32 等待项、逐项 PRIVATE/共享稳定键、重复地址、与传统 `REQUEUE` 交互、MONOTONIC/REALTIME 绝对 time64 截止时间，以及 wake、信号、超时和分配失败竞争下的统一队列回收。AArch64 可按 `SA_RESTART` 恢复等待；i386 当前在信号中断后返回 `EINTR`，尚无完整的系统调用重启框架。`FUTEX2_NUMA`、`FUTEX2_MPOL`、`FUTEX_LOCK_PI` 等扩展仍未实现，`clone3` 仅接受当前任务模型可安全表达的受限标志集。
- AArch64 `mmap` 当前支持匿名私有映射、严格的 `MAP_SHARED | MAP_ANONYMOUS`，以及普通文件的 `MAP_PRIVATE`。普通文件页按 fault 延迟读取；同一活跃 inode 代际与 4 KiB 文件偏移共享单飞页缓存，不依赖 fd 槽或虚拟地址。关闭或复用原 fd 不影响既有映射，fork 保留共享只读后备，首次普通写、CAS 或有效 STXR 会 COW 到匿名私有页；尾页有效内容之后补零，整页越过当前 EOF 或后备 I/O 失败会投递 `SIGBUS/BUS_ADRERR`。映射只接受显式声明可分页、可定位读取的普通文件提供者；伪文件返回 `ENODEV`，只读 fd 可建立可写私有映射，`mprotect` 仍受初始最大权限与 noexec 约束。futex 与 `futex_waitv` 若命中尚未驻留的文件页，会先释放全局 futex 锁再执行 page-in 并重试。文件 `MAP_SHARED`/`MAP_SHARED_VALIDATE`、脏页回写、`msync`、截断后的驻留页失效，以及文件页的 `madvise` 驱逐仍未实现。匿名共享后备在 fork 后双向可见，而权限、解除映射和固定替换仍归各地址空间独立管理；i386 匿名共享映射的原位 `mremap` 扩展尚无可供多个既存 `mm` 共同引用的单一 shmem 后备，因此当前明确拒绝该操作。`MADV_DONTNEED` 会清零匿名私有映射或已分配 brk 页；匿名共享页保留共同后备内容，但当前模型尚不表达可单独驱逐的 PTE/RSS 驻留状态。
- `pidfd` 类接口尚缺少稳定的任务代际、引用和权限模型；`openat2` 尚未表达 `RESOLVE_*` 路径约束。
- `pselect6`、`ppoll` 与 `epoll_create1/epoll_ctl/epoll_pwait` 已接入当前的文件事件和信号掩码模型，不代表所有 Linux I/O 复用语义均已实现。
- `FPREM` 在单次模拟中完成完整余数，不暴露实现相关的 `C2=1` 中间化简步骤。
- `FXTRACT` 已覆盖数值分类，但通用 x87 异常标志、控制字掩码与未掩码陷阱尚未完整模拟；依赖 `FNSTSW` 精确观察异常状态的程序仍可能存在差异。
- 一般有限正数输入的 `log2` 仅承诺确定性近似，不承诺正确舍入。
- i386→i386、i386→AArch64 与 AArch64→AArch64 的多线程 `exec` 已执行 Linux 风格的 de-thread、非 leader TGID 接管、`files`/`sighand` 私有化和安全点映像换代；AArch64→i386 的反向架构切换仍未实现，当前返回 `ENOEXEC` 且保留旧映像。
- `DC ZVA` 当前通过 `DCZID_EL0.DZP` 声明不可用，guest libc 会回退到普通清零路径。
- 网络验证目前只覆盖基础 TCP 客户端路径，不代表 UDP、IPv6 或全部 socket 选项均已实现。
- Apple 门禁验证五切片的 AArch64 auto 后端选择、C/threaded 归档共存、函数指针 ABI、`arm64e` 指针认证，以及 core、完整静态库的普通与全归档链接闭包、重定位、Mach-O 平台、minOS 和 XCFramework 二进制变体；它不运行 guest、不衡量后端性能，也不验证完整 iOS/watchOS 应用的生命周期、界面、签名、沙箱、entitlement 或真机能力，这些仍由集成方负责。
- Alpine 冒烟是目标工作负载验证，不等价于完整发行版兼容认证。
