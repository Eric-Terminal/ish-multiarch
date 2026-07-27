# 多架构实现说明

## 范围与状态

本分支基于官方 iSH 历史继续开发，保留原有 i386 guest，并增加独立的
AArch64 Linux guest 执行路径。AArch64 已接入普通 iPhone App，同时提供
独立的 SwiftUI Watch App；两端都会从固定 Alpine 种子安装真实文件系统，
启动 PID 1，并通过各自终端交互。静态库和 XCFramework 仍只是构建、ABI
与最终链接验证产物，并非已经发布的公共 SDK。

该实现仍处于实验阶段，但不再只有 core 冒烟。专用 iPhone 与 Watch
Simulator 产品已经运行固定的 shell、文件、进程、信号、线程、DNS、
HTTP/HTTPS、包管理、SQLite、Python、本地 C/pthread、本地 Git 与离线 SSH
客户端软件矩阵。
iOS `arm64`、watchOS `arm64_32`/`arm64` 以及 Watch Simulator
`arm64`/`x86_64` 产品构建也已通过。尚未完成的是实体 iPhone/Watch 的签名
运行、App Store 交付，以及超出该软件矩阵的完整 ISA/系统调用兼容认证。

## 架构边界

| 层次 | 当前支持 | 约束 |
| --- | --- | --- |
| guest 指令集 | i386、AArch64 | 两套 CPU 状态与执行路径相互隔离 |
| guest Linux ABI | i386 32 位、AArch64 64 位 | 系统调用号、结构体与寄存器约定按 guest 架构编码 |
| host 平台 | macOS 测试、iOS、watchOS | host 指针宽度不能泄漏进 guest ABI |
| Apple 切片 | iOS device `arm64`；watchOS device `arm64_32`、`arm64`；watchOS Simulator `arm64`、`x86_64` | `arm64_32` 是 watchOS host ABI，不是 32 位 AArch64 guest；watchOS device `arm64` 的 minOS 为 26.0，其余 watchOS 切片为 10.0 |

guest 地址、host 指针和 Linux wire 数据分别使用明确宽度的类型。AArch64 guest 使用稀疏 48 位地址空间，内存访问通过页表和用户内存复制边界完成；文件、任务和信号服务继续复用官方内核对象，但不直接暴露架构特定的数据布局。

## 模块分工

- `guest/memory/`：guest 地址空间、稀疏页表和用户内存访问契约。
- `guest/aarch64/`：AArch64 CPU 状态、指令解码、执行语义、ELF64 装载与 Linux ABI 编码。
- `guest/linux/`：与 guest 架构无关的内存、文件和系统调用服务边界。
- `kernel/aarch64*.c`：将 AArch64 进程生命周期接入官方任务、文件系统、信号与调度设施。
- `tools/apple-core-gate.sh`：构建一个 iOS device 切片和四个 watchOS 切片，检查严格 core、完整静态库消费者、宿主 ABI 与 Apple 二进制元数据，并生成 watchOS device/Simulator XCFramework。
- `tools/apple-watch-package.sh`：默认为 Xcode 构建四个 Watch 核心切片并复制当前平台的通用静态库；公开 CI 在已经完成完整门禁后使用显式 `prebuilt` 模式复用同一批产物。
- `app/Watch/`：SwiftUI Watch 终端、rootfs 安装、Linux runtime 生命周期与第三方声明入口。
- `tests/aarch64/`：指令、ABI、运行时、并发和真实发行版冒烟测试。

## 构建与测试

多架构构建要求 Meson 1.3.0 或更高版本。配置会把编译器记录的绝对与
相对源码根统一映射为仓库内路径，避免诊断宏、调试信息和发布归档泄露
本机 checkout 位置，也消除不同深度构建目录中由源码路径造成的
Release 对象差异。受控 Apple/Xcode 构建还会清零静态归档成员的
时间与所有者元数据。直接调用 Ninja 复现相同归档时，必须让两次
构建使用相同的 `SOURCE_DATE_EPOCH`，并同时设置
`ZERO_AR_DATE=1`；受控脚本会自动建立这两个环境变量。

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

需要定位真实 Linux 工作负载中的 C 回落热点时，可以在原生 AArch64 主机上额外传入
`-Daarch64_threaded_profile=true`；画像必须通过这个 Meson 选项统一开启，不能由单个
翻译单元自行定义内部宏。该选项默认关闭；开启后，每个 AArch64 进程先在自己的 runner
中记录回落次数，销毁时再合并，命令行产品正常退出时向启动时的标准错误副本写出制表符
分隔的 `AARCH64_THREADED_PROFILE` 报告。终态快照累计当时已经析构的 runner；宿主异常
终止或仍未完成退出清理的 runner 不在完整性承诺内。`opcode` 是
`guest/aarch64/decode.h` 中的枚举值，`representative_word` 只是最先合并到全局结果的
代表机器字，不保证是整棵进程树中按时间最早执行的指令。

画像构建会增加运行时开销，只用于选择待优化的指令族，不能用于比较后端性能。Apple
产品门禁要求该选项保持关闭，并检查最终归档不包含画像对象或符号。

原生 AArch64 host 还会注册 `aarch64_backend_performance` 微基准。它在同一个二进制中显式运行 C oracle 与 threaded-code，覆盖纯快速分派、混合 C 回落和低解码成本的 NOP 调度三种稳定命中工作负载。请使用 release 构建运行：

```sh
meson setup build-perf --buildtype=release -Daarch64_backend=auto
meson compile -C build-perf aarch64_backend_benchmark
meson test -C build-perf --benchmark --verbose
```

基准会预热 TLB 与 threaded 缓存，自适应到两种后端的单次采样均至少 100 ms，再以 5:5 的交替次序采集十组 C/threaded 配对样本，报告每条 guest 指令的中位耗时、MAD 离散度和配对加速比。任一 MAD 超过 10% 时只会提示本轮不宜作为回归基线，不会把环境噪声判为代码失败。结果只用于同一台、负载稳定机器上的提交间比较，不设置跨机型或共享 CI 的性能阈值，也不代表完整 Linux 软件或 Apple 设备运行时性能。

完整 iSH 二进制还提供一个手工双后端差分门禁。它只在原生 arm64 macOS 上运行，从同一个
规范 Alpine 3.24.1 fakefs 种子复制两棵隔离 root，串行建立并运行固定为 C 与 threaded
默认后端的全新 Release 构建：

```sh
tools/apple-aarch64-rootfs.sh \
    /tmp/ish-aarch64-paired-seed \
    /tmp/alpine-minirootfs-3.24.1-aarch64.tar.gz \
    build/tools/fakefsify

MESON="$(command -v meson)" \
NINJA="$(command -v ninja)" \
tests/aarch64/backend-paired.bash /tmp/ish-aarch64-paired-seed
```

输入种子必须是打包器生成且当前没有被运行的四项只读快照；脚本会先冻结一份私有快照，
不会直接 mount 或修改输入种子。
固定离线工作负载只使用 minirootfs 自带的动态 BusyBox，覆盖 pipeline、进程创建、`exec`、
`wait`、预期非零子进程、文件变换与 guest 生成脚本。门禁要求两个后端都以退出码 0 完成、
stderr 为空、stdout 符合受跟踪合同，并逐字节比较工作负载专属 artifact 树。两份 root
首次 mount 后的 `meta.db` inode、WAL 字节、PID、mtime、调度顺序和耗时都可能合法不同，
因此不参与比较；超时只用于阻止孤儿进程，不能当作性能结果。

这个门禁会创建两棵全新构建和外部 rootfs 副本，所以不注册进默认 `meson test`。它始终
最多运行一个自己启动的 iSH，构建限制为 `-j2`，正常或可捕获的异常退出都会只终止自己启动
的受控进程组并删除自己的临时目录，不会操作已有 iSH、Simulator 或应用容器。

Apple 门禁需要 Xcode SDK、Meson 与 Ninja。它构建以下五个 Apple 切片：

- iOS device `arm64`，minOS 15.0；
- watchOS device `arm64_32`，minOS 10.0；
- watchOS device `arm64`，minOS 26.0；
- watchOS Simulator `arm64`，minOS 10.0；
- watchOS Simulator `x86_64`，minOS 10.0。

门禁显式使用 `aarch64_backend=auto` 且关闭 threaded 画像，并要求 iOS `arm64`、watchOS `arm64_32`/`arm64` 与 Simulator `arm64` 选择 threaded-code，Simulator `x86_64` 选择 C。它同时核对生成的配置宏、core 与完整归档中的 C/threaded 对象和公开符号，拒绝画像对象与符号，并对五个切片严格编译函数指针 ABI probe；iOS 还以 `arm64e -O2` 检查 threaded 间接调用的指针认证指令。

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

### AArch64 rootfs 种子与首次安装

`tools/apple-aarch64-rootfs.sh` 在构建机上把固定版本、固定 SHA-256 的官方 Alpine
AArch64 minirootfs 转换为 fakefs 种子。输出目录只有 `meta.db`、`data/`、
`rootfs-manifest.txt` 与 `rootfs-hardlinks.tsv` 四项；归档本身和生成的 rootfs 都不进入
仓库。hardlink 清单用于弥补 App bundle 复制过程中可能丢失的宿主链接关系。

正式打包还会离线读取归档内的 apk installed 数据库，并与
`third_party/alpine/3.24.1-aarch64/packages.tsv` 逐项核对 16 个二进制包的版本、
apk origin、apk 元数据声明的许可证表达式和 aports commit 字段。这个门禁只锁定二进制
归档与受跟踪 apk 包元数据清单的一致性，不验证 aports 对象或上游源码字节，也不等于已经
交付第三方许可证文本或完整对应源码；发行前仍须完成下文的来源与许可门禁。

对应源码门禁与日常 App 构建分离。`binary-reference.tsv` 把包清单关联到固定二进制归档和
apk installed 数据库；`static-link-sources.tsv` 另行记录 BusyBox 静态吸收的 utmps 与
skalibs，不把它们冒充成 minirootfs 内的二进制包。`origins.tsv` 因而锁定 10 个二进制包
origin 和 2 个静态源码 origin，`source-assets.tsv` 锁定 12 份 aports 目录归档及 11 份
上游 distfile 的 URL、大小和 SHA-512。静态源码使用 BusyBox 构建点的 aports 全树快照；
该快照不是对两只依赖 APK 自身 build commit 的声明。

生成与验证还会严格解析每份 `APKBUILD` 的 `sha512sums`：目录内文件就地重算，目录外要求
则必须与上游资产清单双向闭合；静态源码版本还会与快照中 `pkgver/pkgrel` 的规范字面值
一致。以下命令会先检查受跟踪锁，再显式顺序取得资产、生成只有普通文件且元数据固定的
确定性 tar，并按受跟踪 SHA-256 做纯离线复核：

```sh
python3 tools/apple-aarch64-rootfs-sources.py check-locks
python3 tools/apple-aarch64-rootfs-sources.py fetch \
    build/alpine-source-cache
python3 tools/apple-aarch64-rootfs-sources.py bundle \
    build/alpine-source-cache \
    build/alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar
python3 tools/apple-aarch64-rootfs-sources.py verify \
    build/alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar
```

准备发行资产时，应从已经填充并通过锁校验的 cache 生成一个新的本地
暂存目录，不能复用 `build/` 里可能遗留的旧 tar：

```sh
mkdir -p build
python3 tools/apple-aarch64-rootfs-release.py \
    build/alpine-source-cache \
    build/alpine-source-release
```

暂存器不会下载或补齐源码资产。它会独立生成并验证两份候选，确认两次
结果逐字节一致后，以排他 rename 发布输出目录；既有输出、符号链接路径、
缺失缓存、摘要漂移或任一步失败都会拒绝发布。成功目录固定只包含
`alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar` 与
`corresponding-source.sha256` 两个普通文件。本地暂存完成不等于这些资产
已经进入公共 Release，也不替代最终发行审计。

确定性 tar 包含 23 份真实源码载荷和内嵌锁表，不进入 Xcode rootfs phase、seed 或 App
Resources。它的当前 SHA-256 由
`third_party/alpine/3.24.1-aarch64/corresponding-source.sha256` 唯一锁定。

许可证输入与对应源码共用同一份 16 个已安装包、12 个源码 origin 和 23 份源码资产闭包。
`license-inputs.tsv` 把每项包版本及其 apk 许可证表达式映射到固定的 aports 成员、上游成员
或受跟踪权威文本；两只静态源码则只能映射回声明的 BusyBox consumer 及其 ISC 许可。
清单锁定输入大小、SHA-256、声明 section 和 section SHA-256。
`THIRD-PARTY-NOTICES.txt` 的 19 个具名 section 只允许空行出现在 marker 之外。完整
GPLv2 正文来自锁定的 pax-utils COPYING，完整 LGPL 2.1 正文来自受跟踪权威文本；apk-tools
中内容异常的 LICENSE 只保留为来源证据。以下第一条
命令只读受跟踪文件，第二条还会纯离线核对现有源码缓存；两者都不联网、不构建 guest：

```sh
python3 tools/apple-aarch64-rootfs-licenses.py check-locks
python3 tools/apple-aarch64-rootfs-licenses.py validate-sources \
    build/alpine-source-cache
```

该门禁验证包、源码成员、权威文本和声明正文各自的字节身份，并逐字验证权威文本进入目标
section；普通源码成员到 notice 摘录的选择经过人工内容审计，不把机器锁误称为通用许可
解释器。它不会把 APKBUILD maintainer 当成版权所有者，也不替代法律审查。通用 MIT 模板
中的所有者占位符会原样保留，避免在固定源码没有给出所有者声明时自行推断。详细边界见
`third_party/alpine/3.24.1-aarch64/LICENSE-NOTICES.md`。

项目自身许可与当前公开源码仓库入口使用另一份独立的确定性资源：

```sh
python3 tools/apple-project-license-notices.py check-locks
```

该门禁从仓库根目录逐字收入 `LICENSE.md`、`LICENSE.IOS`，并收入两份根许可
说明明确引用的 GNU GPLv2、GPLv3 完整原文；`inputs.tsv` 同时固定当前公开
源码仓库 `https://github.com/Eric-Terminal/ish-multiarch`。生成的
`PROJECT-LICENSES.txt` 已进入普通 `iSH`、`iSH+Linux` 与 `iSHWatch`
三种 App，扩展、测试 target 与 LinkSmoke 不携带它。iPhone 的 About 页面
同时提供当前公开仓库按钮；Watch 的共享“许可证与源码”页面提供同一仓库
链接。

这里固定的是当前项目许可正文和规范仓库入口，不是某个 App 二进制的精确
对应源码证明。发布构建仍须把完整 commit/tag、主仓库与所有 gitlink、源码
资产、摘要和公开 Release 相互绑定，并从公开位置回读验证；当前可变分支
URL 不能替代该证据。

Apple 宿主第三方输入使用独立的 target-aware 锁，不能与 guest seed 的
Alpine 闭包混为一谈：

```sh
python3 tools/apple-host-delivery-inputs.py check-locks
python3 tools/apple-host-notices.py check-locks
```

该只读门禁固定 `deps/libarchive`、`deps/libapps` 与可选 Linux 产品所用
`deps/linux` 的 gitlink、版本来源、实际构建输入路径集合、许可、源码内
声明与 provenance 来源证据，以及 target 路由，并拒绝三个既有 gitlink
未声明进入主工程构建 phase。
普通 `iSH` 携带 `libarchive.a` 与 `hterm_all.js`；
后者的静态 concat 闭包同时包含 hterm、libdot、intl-segmenter 和
wcwidth。Watch 与 FileProvider 没有这两项 vendored 输入，Watch 的显式
外部链接项只按 Apple SDK 边界登记。完整格式与非目标见
`third_party/apple-host/README.md`。

第二条命令会从 libarchive 的 128 个实际 Xcode 编译源出发，递归闭合锁定
gitlink 内的 37 个本地双引号 include，并逐字提取每个文件开头连续的 C
块注释。另有 22 段来源、生成或许可证据由固定行区间与摘要锁定，其中
13 段来自 hterm 交付输入及其锁定生成 provenance（包括 Material 上游
README；`ranges.py` 不进入 53 项实际交付闭包），9 段来自 libarchive；
hterm、libdot、intl-segmenter、wcwidth、`libarchive/COPYING` 和
Material Design Icons 同期快照根 `LICENSE` 的完整许可按原始文件收入；
Material README 只收入完整 License section，三个上游 SVG 只作为锁定
输入与格式化映射证据。Unicode Data Files 完整许可来自官方 unicodetools
固定提交，聚合显示只移除文件开头的 UTF-8 BOM。只有最终收入正文的字节
完全相同才去重，来源路径不会丢失。

收入宿主正文的七个完整许可输入去重为 6 份唯一文本，libarchive 闭包得到
97 份唯一前导文本，22 个锁定片段也各自唯一，因此生成 125 个唯一正文
section；再加 `overview`、`material-provenance`、
`wcwidth-unicode-provenance` 与 `unresolved-provenance`，文件共有
129 对 BEGIN/END 标记。
重建结果必须与受跟踪的 `APPLE-HOST-NOTICES.txt` 逐字一致。

来源片段只在锁定文本或本地历史明确写出 imported、taken、adapted、
derived、generated 等来源/生成关系时纳入；普通标准引用不会被递归扩成无穷
依赖。三个 Material SVG 现已固定 libapps 导入提交，以及作者时间点官方
master tip `3d4a32b327272c458e12586437c3ca0696b28a69` 的三个精确
`production` 路径、README Apache-2.0 适用声明与根 LICENSE；校验器会离线
证明上游原字节只经固定空白格式化得到当前交付字节。导入提交没有记录精确
revision，因此正文不会冒充导入者明确选择了该快照。wcwidth 已保存官方
unicodetools 固定提交中的 Unicode 13.0.0 三份输入
与完整 Data Files 许可，并交叉确认它们与固定 UCD 发布归档成员逐字一致；
历史 `ranges.py` 会从哨兵状态离线重建当前三张表。该证据不冒充 libapps
作者明确选择了上述 Git 提交，也不声称没有 LICENSE 成员的 UCD.zip 自带
许可。当前未决节继续记录 `lib_colors.js` 的 W3C CSS Color 4 改编与 X11
`rgb.txt` 派生缺少固定上游材料，以及 libarchive 的 Unicode 6.0.0
`UnicodeData.txt` 生成表和 Unicode Standard Annex #15 来源仍须闭合。

这份宿主正文进入普通 `iSH` 与 `iSH+Linux`，共享 iPhone 查看器按 bundle
中实际存在的资源显示：普通 iSH 依次显示项目许可、Alpine seed 与宿主正文，
iSH+Linux 显示项目许可与公共宿主正文。Watch 显示项目许可与 Alpine seed，
但不携带宿主正文；FileProvider 和测试产品也不携带宿主正文。该接线仍不是
最终法律结论：BLAKE2 许可分支、W3C/X11 与 libarchive Unicode 来源许可、
LGPL 方案，以及 `iSH+Linux` 的 Linux GPLv2、在线 rootfs 和对应源码交付
均保持未决。
这些缺口继续阻断公共发行；本切片只是确定性记录证据与未知边界，不是法律闭合。

当前仓库已经锁定对应源码制品和 Alpine 声明正文。声明文件现已逐字进入
iPhone 与 Watch App，并由各自的只读查看入口、UI 用例、静态 target 归属门禁
及公开 CI 的 bundle 字节比较覆盖；不携带固定 AArch64 seed 的 iSH+Linux 明确
排除该资源。项目许可正文与当前公开仓库入口已经进入三种 App，公共宿主正文
也已形成确定性生成门禁和产品接线；但精确二进制 revision、公开 Release、
上述外部来源、LGPL 选择和对应源码资产尚未形成可从 release 位置回读的完整
交付链，因此仍不能声称来源与许可交付已经完成。BusyBox `volume_id`
中 21 个输入与 pax-utils `elf.h` 的原始 notice 明确采用
LGPL-2.1-or-later；`volume_id/bcache.c` 只写 LGPL、没有指定版本。发行门禁还
必须解决该版本依据，并证明实际交付物采用了 LGPL 2.1 第 3 节转换，或同时提供
未转换条款要求的可重新链接材料。

Apple 端的 `ish_apple_rootfs_seed_install` 只负责把前述只读 fakefs rootfs seed 首次安装到
调用者提供的 Application Support 父目录。它会验证固定格式的官方 AArch64 manifest、
BusyBox ELF、SQLite schema 与完整性、hardlink 清单和数据树类型，再在未发布的 staging
中重建链接、更新 `meta.db` 的真实 inode，并以排他 rename 发布 final root。安装过程使用
长期 lock、随机 staging 和与该目录 dev/inode 绑定的原子 owner 记录；在文件系统支持目录
fsync 时，
目录项删除与发布分别通过两阶段 fsync 保留掉电恢复顺序。未知 owner、错配目录和符号链接
不会被自动删除。

已经存在且带有效安装 receipt、且 canonical owner 状态可安全收敛的 root 会直接复用。
复用检查只确认 final 顶层目录、`meta.db` 与 `data/` 的所有权和类型，不会重新读取新
seed、重放初始 hardlink 或拒绝 guest 后来创建的 FIFO、WAL/SHM 与用户文件，因此 App
更新不会隐式重置用户 Linux 环境。

该接口成功时通过结果枚举区分“本次安装”和“已经存在”，失败返回正数 POSIX errno。
它本身不 mount fakefs、不创建 PID 1、不配置 TTY，也不执行 guest ELF；普通 iPhone App
与 `iSHWatch` target 已在该安装事务成功后分别完成这些 runtime 生命周期步骤。安全边界
假定 seed 来自签名 App bundle、持久父目录位于应用私有容器，且没有绕过 installer lock
的同 UID 写者。

### Xcode Scheme 验收

工程提供三个相关共享 Scheme：

- `iSHCore-watchOS` 是 aggregate 打包 Scheme。它调用 `tools/apple-watch-package.sh`，生成四个 watchOS 切片、通用静态库和三份 XCFramework；它本身没有 Xcode 可运行产品。
- `iSHWatchLinkSmoke` 是最小 watchOS application 类型的链接夹具。它只有一个 C 入口，依赖 `iSHCore-watchOS`，并最终链接 `libish.a`、`libish_emu.a`、`libfakefs.a` 及系统 SQLite。它没有 SwiftUI、图标或用户界面，不会在构建时运行 guest，也不代表完整或可交付的 Watch App。
- `iSHWatch` 是真正的 SwiftUI Watch App，包含终端、AArch64 rootfs、Linux runtime 与持久容器；其 UI 测试由 `iSHWatchUITests` target 提供。

普通 Xcode 开发构建无需设置额外模式：`iSHWatch` 的 target 依赖会先构建核心，
再复制当前平台的通用归档。做发布型构建验收时，应只运行一次显式门禁，再让后续
LinkSmoke 和完整 App 以 `prebuilt` 模式复用同一个产物根，避免每个 Xcode 命令
重建四个核心切片。下面依次覆盖 Apple 五切片 core、Watch 四切片 LinkSmoke 和
Watch Simulator `arm64` 完整 App；只验 Watch core 四切片时，可在门禁命令前增加
`APPLE_SKIP_IOS=1`：

```sh
artifacts="$PWD/build-apple-core"
products="$PWD/build-apple-products"
rootfs_cache="$PWD/build/apple-rootfs-cache"
MESON="$(command -v meson)" \
NINJA="$(command -v ninja)" \
tools/apple-core-gate.sh "$artifacts"
```

随后各构建一个 device 和 Simulator fat LinkSmoke。device 的两个切片最低系统
版本不同；一次构建 fat 产物是允许的，但必须在产物上逐架构核验元数据，不能只检查
整体架构集合：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchos \
    ARCHS="arm64_32 arm64" \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    ISH_WATCH_ARTIFACT_ROOT="$artifacts" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    SYMROOT="$products" \
    CODE_SIGNING_ALLOWED=NO \
    build

xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatchLinkSmoke \
    -configuration Release \
    -sdk watchsimulator \
    ARCHS="arm64 x86_64" \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    ISH_WATCH_ARTIFACT_ROOT="$artifacts" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    SYMROOT="$products" \
    CODE_SIGNING_ALLOWED=NO \
    build

device="$products/Release-watchos/iSHWatchLinkSmoke.app/iSHWatchLinkSmoke"
simulator="$products/Release-watchsimulator/iSHWatchLinkSmoke.app/iSHWatchLinkSmoke"
xcrun lipo "$device" -verify_arch arm64_32 arm64
xcrun lipo "$simulator" -verify_arch arm64 x86_64
xcrun vtool -arch arm64_32 -show-build "$device"
xcrun vtool -arch arm64 -show-build "$device"
xcrun vtool -arch arm64 -show-build "$simulator"
xcrun vtool -arch x86_64 -show-build "$simulator"
```

`vtool` 输出必须分别为 watchOS `arm64_32` minOS 10.0、watchOS `arm64`
minOS 26.0，以及两个 Watch Simulator 切片 minOS 10.0。

最后构建完整 Watch App。它还会生成固定 AArch64 rootfs seed，但不会启动
Simulator 或 guest；显式缓存目录会让后续产品构建复用同一份已校验归档：

```sh
xcodebuild \
    -project iSH.xcodeproj \
    -scheme iSHWatch \
    -configuration Release \
    -sdk watchsimulator \
    ARCHS=arm64 \
    WATCHOS_DEPLOYMENT_TARGET=10.0 \
    ONLY_ACTIVE_ARCH=NO \
    ISH_WATCH_ARTIFACT_ROOT="$artifacts" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    ISH_AARCH64_ROOTFS_CACHE="$rootfs_cache" \
    SYMROOT="$products" \
    CODE_SIGNING_ALLOWED=NO \
    build
```

两次 LinkSmoke 命令只使用 SDK 编译和链接；完整 App 的 rootfs phase 会在
缓存无效时下载并核验固定归档，同时构建并运行宿主 `fakefsify`。缓存已经通过
摘要校验时，可为完整 App 命令再增加 `ISH_AARCH64_ROOTFS_OFFLINE=1`，保证不会
联网。上述命令都不要求安装或启动 Simulator runtime。完整验收把核心产物固定在
`build-apple-core/`，把 Xcode 产品固定在 `build-apple-products/`。普通 Xcode
构建未显式指定时，交叉构建中间产物位于 `ISH_WATCH_ARTIFACT_ROOT`，默认展开为
DerivedData 下的：

```text
Build/Intermediates.noindex/iSH.build/iSHWatchArtifacts/<配置><平台后缀>/
```

其中仍使用与命令行门禁相同的 `universal/watchos/`、`universal/watchsimulator/` 和 `xcframeworks/` 子目录。当前平台供 LinkSmoke 使用的三份通用归档会复制到：

```text
Build/Products/<配置><平台后缀>/iSHWatchLibraries/
```

LinkSmoke 的验证包位于
`Build/Products/<配置><平台后缀>/iSHWatchLinkSmoke.app/`；这个路径下出现
`.app` 只表示 Xcode 完成了链接。完整产品位于同级 `iSHWatch.app/`，但无签名
SDK 构建仍不等于实体设备安装、启动或发行证据。`iSHWatchUITests` 会真实启动
Simulator、安装 Alpine 包并运行长时软件矩阵，不属于上述纯编译命令。

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
tests/aarch64/alpine-smoke.bash build/ish /tmp/ish-a64-alpine \
    build/libish_aarch64_e2e_dns_redirect.dylib
```

冒烟脚本不会下载或提交 rootfs。macOS 普通进程不能监听 UDP 53，因此测试专用动态库只在该次 `ish` 子进程中把 guest 可见的 `127.0.0.53:53` 映射到本地夹具随机选择的高位端口，并把响应来源恢复为 guest 看到的 53 端口；生产 socket 实现不含测试重定向。DNS 阶段会先把 fakefs 复制到宿主临时目录，resolver 只写入隔离副本，不会改动输入 rootfs 的 resolver；顶层存储或 `data` 树含真实宿主符号链接的非规范 fakefs 会被拒绝。脚本会拒绝与已有 `ish` 进程或同一 rootfs 的另一份验收重叠运行，并为每个 guest 命令设置硬超时；正常退出或收到可捕获信号时会清理隔离副本和自己启动的 DNS/HTTP 服务。若宿主直接以 `SIGKILL` 终止脚本，夹具会监测父进程并自行退出，带硬上限的 guest 子进程也会释放锁，但随机命名的临时 fakefs 副本可能留在磁盘上，需要人工删除。

本次发布候选已经完成以下门禁：

- 默认非交叉 `kernel=ish` 配置当前登记的普通 Meson 测试已在 fresh 本机构建和公开 Linux Clang/GCC 矩阵通过；ASan+UBSan 证据只覆盖相应改动的定向回归，当前不声称完整测试集已重新通过 sanitizer 或 TSan。
- iOS device `arm64`，watchOS device `arm64_32`/`arm64` 与 Simulator `arm64`/`x86_64` 的 core、完整静态库、普通消费者、全归档消费者、ABI 和二进制元数据门禁通过，并成功生成包含 device/Simulator 变体的三份 XCFramework。
- 命令行 Alpine 冒烟的动态 `/bin/sh`、文件操作、子进程等待、信号终止、数字地址 HTTP、musl `getent`、BusyBox `nslookup` 与主机名 HTTP 获取通过；查询日志证明三条工作负载都实际经过本地 UDP DNS responder。
- 专用 iPhone 与 Watch Simulator 的完整产品分别通过启动/交互、真实 resolver、HTTP/HTTPS、`apk update`、SQLite WAL 与复启持久化、Python、guest GCC/pthread、本地 Git 操作和离线 SSH 客户端/密钥/配置固定矩阵。此类长时 UI 门禁是发布候选实证，不在每次公开 CI 中重放，也不能替代实体设备验证。
- 公开 CI 会构建 iPhone device `arm64` Release、Apple 五切片 core、Watch 四切片 LinkSmoke 和 Watch Simulator `arm64` 完整 App；它逐字比较 iPhone 的项目/Alpine/宿主正文、Watch 的项目/Alpine 正文和 ReleaseLinux 的项目/公共宿主正文，并验证每个产品都排除不属于自身范围的声明资源。

## 来源、许可与独立实现边界

本仓库从官方 `ish-app/ish` 提交 `997642f3787cc63e65f7134b7bb0362c74bff8e0` 延续开发，保留其 Git 历史、版权声明以及 `LICENSE.md`、`LICENSE.IOS`。使用或分发时仍须遵守这些文件中的许可条件。

三种 Apple App 已通过 `PROJECT-LICENSES.txt` 显示上述项目许可、完整
GPLv2/GPLv3 原文和当前公开仓库入口。该入口证明源码仓库当前可访问，不证明
任一二进制对应的精确 revision 已经发布；最终 Release 仍须绑定并公开验证
主仓库、gitlink 与对应源码资产。

多架构改动依据公开的 Arm 指令集、Linux AArch64 ABI、ELF 与 Apple 平台 ABI 资料独立设计和实现。实现过程中以官方基线为集成边界，不引入其他衍生实现的源代码。最终发布前必须以官方基线到发布提交为范围，重新执行来源、相似性、受跟踪文件路径与提交信息审计；无法用公开规范解释的显著重合必须重写或明确标注来源。

## 当前限制

以下条目记录超出固定软件矩阵的兼容边界，不是要求在产品已经运行后继续
预写所有 Linux 长尾。只有真实 iPhone/Watch 工作负载出现可复现失败时，才把
对应差异提升为产品修复。

- AArch64 指令和 Linux 系统调用覆盖以运行现有工作负载为驱动，尚不等同于完整 ISA 或内核兼容层；当前已实现 32/64 位 `CASP`、`CASPA`、`CASPL` 与 `CASPAL`，但尚未覆盖完整 `FEAT_LSE`，guest 的 `AT_HWCAP` 暂不宣告 `HWCAP_ATOMICS`。
- 未支持的 AArch64 指令会安全投递 `SIGILL`，未知 Linux 系统调用会返回 `ENOSYS`。
- AArch64 `futex` 已支持 `WAIT`、`WAKE`、`REQUEUE`、匿名共享后备的跨进程键、robust list 与 clear-child-tid 退出清理。ELF 主映像和解释器的文件页以活跃 inode 代际与绝对文件字节偏移建键，因此 fork 后以及独立装载到不同虚拟地址的同一文件别名可以协作。fork 后的私有文件页会先共享同一受同步保护的物理后备；普通写、CAS 与有效 STXR 都在首次写前换入匿名副本，共享 futex 写固定也使用同一 COW 入口，再进入当前 `mm` 的匿名键域。跨页 COW 先完整分配再提交，失败不会留下部分后备、来源或映射世代变化；futex 键准备遇到该分配失败时返回 `ENOMEM`。同一宿主文件若经不同 guest mount 打开，当前会保守地产生不同 inode 代际并形成安全假阴性。i386 的非 PRIVATE `WAIT`/`WAKE`/`REQUEUE` 也会按稳定后备身份建键：匿名共享映射可跨 fork 后的不同 `mm` 协作，独立普通文件映射按 inode 代际与实际文件字节偏移匹配；robust list 和 clear-child-tid 的退出唤醒沿用相同共享键。
- i386 与 AArch64 均实现系统调用 449 的基础 `futex_waitv`：支持 1～128 个 U32 等待项、逐项 PRIVATE/共享稳定键、重复地址、与传统 `REQUEUE` 交互、MONOTONIC/REALTIME 绝对 time64 截止时间，以及 wake、信号、超时和分配失败竞争下的统一队列回收。AArch64 可按 `SA_RESTART` 恢复等待；i386 当前在信号中断后返回 `EINTR`，尚无完整的系统调用重启框架。`FUTEX2_NUMA`、`FUTEX2_MPOL`、`FUTEX_LOCK_PI` 等扩展仍未实现，`clone3` 仅接受当前任务模型可安全表达的受限标志集。
- AArch64 `mmap` 当前支持匿名私有映射、严格的 `MAP_SHARED | MAP_ANONYMOUS`，以及普通文件的 `MAP_PRIVATE`、`MAP_SHARED` 和 `MAP_SHARED_VALIDATE`。取得普通文件 fd 后会先拒绝 `MAP_HUGETLB`；其余文件映射先用 `MAP_FIXED`/`MAP_FIXED_NOREPLACE` 选址，普通 `MAP_SHARED` 随后按固定 Linux 历史掩码丢弃未知非历史位和不支持的 `MAP_SYNC`，`MAP_SHARED_VALIDATE` 则以 `EOPNOTSUPP` 拒绝。
  `MAP_DENYWRITE`/`MAP_EXECUTABLE` 作为历史空操作接受，普通文件的 `MAP_GROWSDOWN` 在权限、挂载和 provider 检查后返回 `EINVAL`；`MAP_LOCKED`、`MAP_POPULATE` 等其余历史 hint 当前只兼容参数，不提供锁页或预 fault 副作用。共享可写能力要求读写 fd 和精确定位写回接口，只读 fd 的共享映射不会保留后续写权限，noexec 同时限制当前与后续执行权限。
  普通文件页按 fault 延迟读取；同一活跃 inode 代际与 4 KiB 文件偏移共享单飞页缓存，不依赖 fd 槽或虚拟地址。关闭或复用原 fd 不影响既有映射；私有映射首次普通写、CAS 或有效 STXR 会 COW 到匿名页，共享映射则直接修改共同后备并记录脏世代。普通 `read`/`pread` 会叠加驻留页内容，成功的 `write`/`pwrite` 会同步合并驻留后备；这一一致性域目前只覆盖同一 guest inode 代际内的 AArch64 pager 别名与经内核文件服务发起的普通 guest I/O。
  同一代际上的 i386 realfs `MAP_SHARED` host 后备与任一 AArch64 文件 pager 由代码强制互斥，后发请求返回 `EBUSY`，以避免两套缓存整页写回造成静默覆盖；这只是临时安全边界，并非 Linux 的统一页缓存实现。i386 `MAP_PRIVATE`/ELF host 映射仍使用独立后备；外部 host 映射以及同一宿主文件经不同 guest mount 打开的别名也不进入该互斥域，后两者与 AArch64 pager 并存时仍可能出现驻留陈旧或整页写回覆盖，统一 host 与 guest 文件页身份仍是后续工作。
  pager 写回会按当前 EOF 限界、忽略 `O_APPEND` 并循环处理短写；pager 存活期间的显式同步失败会保留脏状态或待确认 durability 代际以供重试。最后一份映射引用只能执行无法向 guest 报错的 drain；若实际脏页写回或已有待确认 durability 的同步失败，无分配的 dirty orphan registry 会保留 pager 强引用和原 inode 弱槽，后续同 inode 映射、普通 I/O 或显式同步会接管并复用同一 cache，避免失败脏页被销毁或与新 pager 分叉。没有脏页和待确认写入的纯净 pager 不会仅为析构触发文件同步。维护入口每次只重试调用开始时的 orphan 快照，持久失败不会在一次调用中忙循环，也不创建常驻后台线程。尾页有效内容之后补零，整页越过当前 EOF 或后备读取失败会投递 `SIGBUS/BUS_ADRERR`。映射只接受显式声明可分页、可定位读取的普通文件提供者；伪文件返回 `ENODEV`，只读 fd 可建立可写私有映射。futex 与 `futex_waitv` 若命中尚未驻留的文件页，会先释放全局 futex 锁再执行 page-in 并重试。
  i386 的 `truncate/ftruncate` 92/93 与 `truncate64/ftruncate64` 193/194、AArch64 的 45/46 共用 retained-fd 尺寸服务；负长度分别优先于 pathname 和 fd 访问。`open(O_TRUNC)` 不再把一次性标志提前交给 provider，而是在 inode、类型与 guest 权限确认后对稳定 fd 提交；`O_RDONLY|O_TRUNC` 会同时检查读写权限，但返回 fd 仍保持逻辑只读。provider 会原子报告 `O_CREAT` 是否实际创建了最终对象，新建文件跳过 inode 模式复核和冗余截断。realfs、fakefs 与 tmpfs 的成功 resize 都在同 inode I/O 域内先发布底层大小，再无失败地通知 pager；底层失败不改变 resident cache，fd 顺序 offset 也保持不变。
  文件页失效域登记 cache/shared backing 与私有 COW clone。每次成功 shrink、grow 或等长 truncate 都清理文件缓存的新 EOF 尾部，并永久撤销新 EOF 后的整页 backing；这包括已经 COW 的 `MAP_PRIVATE` 整页。shrink 后 grow 会重新 page-in 全新 backing，不复活旧 cache/COW 内容；旧 TLB、CAS、STXR 与 futex 路径都在持有 backing 同步锁后复核可访问状态。部分页私有 COW 尾部仍属于私有映射，不随文件尾部清零。该协议使用普通锁保护 64 位文件偏移和失效登记，不依赖 watchOS `arm64_32` 的 lock-free 64 位原子。
  匿名共享后备在 fork 后双向可见，而权限、解除映射和固定替换仍归各地址空间独立管理。AArch64 系统调用 216 已支持原位缩放、`MREMAP_MAYMOVE`、`MREMAP_FIXED`、`MREMAP_DONTUNMAP` 和共享映射的零旧长度复制；移动事务完整保留驻留页的私有 COW、共享后备、文件来源与绝对文件偏移，lazy 文件页不提前换入，fork 出的其他 `mm` 仍保留原地址。文件私有/共享映射可以原位延长或搬到新地址，`DONTUNMAP` 后旧文件地址按 pager 重新 fault；匿名私有旧地址取得全新零页，匿名共享源与目标继续别名同一后备。当前匿名私有 `DONTUNMAP` 会在提交前事务性预分配整个旧范围的零页，而 Linux 允许旧范围在后续 fault 时按需取得零页或交给 userfaultfd，因此大范围调用的驻留内存与 `ENOMEM` 时机仍不同。AArch64 与 i386 的匿名共享扩展仍缺少可供多个既存 `mm` 共同引用的 VMA 级 shmem 对象，因此当前明确拒绝；AArch64 的一次调用也暂不移动跨越多个不同 VMA 的 fixed 等长范围。i386 仍只支持原位缩小和私有匿名原位扩展，尚未实现 `MAYMOVE`、`FIXED` 与文件扩展。
  `MADV_DONTNEED` 会清零匿名私有映射或已分配 brk 页；匿名共享页保留共同后备内容，因为当前模型尚不表达可单独驱逐的 PTE/RSS 驻留状态。文件私有与共享 VMA 则只撤销当前 `mm` 的驻留 PTE，不换入 lazy 页、不删除 VMA，也不驱逐 inode pager cache；私有 COW 副本会被丢弃并在下次 fault 时恢复文件内容，共享脏页仍留在 cache 中等待显式或最终写回。跨空洞调用会继续处理后续 VMA，最终返回 `ENOMEM`；fork 出的其他地址空间不受影响。
- i386 与 AArch64 都已实现 `msync`、`fsync` 和 `fdatasync`。`msync` 保留 Linux 的 flags、页对齐、32/64 位无符号长度环绕、区间空洞与错误覆盖顺序；AArch64 还会在校验前移除用户地址顶字节标签。`MS_ASYNC`、flags=0 和单独的 `MS_INVALIDATE` 不启动 I/O，也不会把 lazy 文件页提前换入；只有 `MS_SYNC` 的可写共享文件映射会同步。同步期间只在地址空间锁内取得 pager 或 fd 强引用，provider I/O 在锁外执行，返回后从旧段末端重新查询，因此并发 `munmap`、`MAP_FIXED` 与 fork 不会使用陈旧映射指针。
  AArch64 会先写回请求文件区间内的驻留脏页；i386 的 host 共享映射以及当前 realfs/tmpfs provider 再执行整文件 data-only durability，这比 Linux 的区间 durability 更强但范围更粗。两个 ABI 的 `fsync`/`fdatasync` 都先同步同 inode pager 的全部驻留脏页，再分别执行完整或 data-only 文件回调；无效 fd 返回 `EBADF`，缺少同步操作返回 `EINVAL`。Apple SDK 没有公开 `fdatasync` 时使用更强的 `fsync`，tmpfs 同步为空操作成功。
  pager/provider 的结果枚举目前会把 `ENOSPC`、`EROFS` 等细分写回或 durability 错误收敛成 `EIO`，直接文件 `fsync` 回调的错误则原样传播。VMA 尚不保存真实锁页状态，所以 `MS_INVALIDATE` 对 `MAP_LOCKED` 应返回的 `EBUSY` 仍无法表达；它也不执行缓存驱逐。dirty orphan registry 只保证失败页缓存仍有所有者和重试路径，不等于 Linux 的后台 writeback，也不解决跨 mount 别名或 host/guest 统一页缓存。
- `pidfd` 类接口尚缺少稳定的任务代际、引用和权限模型；`openat2` 尚未表达 `RESOLVE_*` 路径约束。
- `pselect6`、`ppoll` 与 `epoll_create1/epoll_ctl/epoll_pwait` 已接入当前的文件事件和信号掩码模型，不代表所有 Linux I/O 复用语义均已实现。
- `FPREM` 在单次模拟中完成完整余数，不暴露实现相关的 `C2=1` 中间化简步骤。
- `FXTRACT` 已覆盖数值分类，但通用 x87 异常标志、控制字掩码与未掩码陷阱尚未完整模拟；依赖 `FNSTSW` 精确观察异常状态的程序仍可能存在差异。
- 一般有限正数输入的 `log2` 仅承诺确定性近似，不承诺正确舍入。
- i386→i386、i386→AArch64 与 AArch64→AArch64 的多线程 `exec` 已执行 Linux 风格的 de-thread、非 leader TGID 接管、`files`/`sighand` 私有化和安全点映像换代；AArch64→i386 的反向架构切换仍未实现，当前返回 `ENOEXEC` 且保留旧映像。
- `DC ZVA` 当前通过 `DCZID_EL0.DZP` 声明不可用，guest libc 会回退到普通清零路径。
- AArch64 已接入 204 `getsockname`、205 `getpeername`、209 `getsockopt` 与 210 `shutdown`。名称查询保持协议状态检查、完整长度回写与截断地址复制的 Linux 顺序；socket option 支持零长度和短缓冲，区分 SOL_SOCKET 的值优先 copyout 与 TCP/IP 的长度优先 copyout，并在 guest copyout 前消费 `SO_ERROR`。`SO_PROTOCOL` 返回 guest 的实际 TCP/UDP 协议号，`SO_ACCEPTCONN` 使用显式监听状态；AArch64 的 OLD/NEW timeout 都按固定 16 字节 wire 转换，watchOS `arm64_32` 的 host `timeval` 宽度不会泄漏。AF_UNIX 现在以 guest 状态累加读写关闭方向，STREAM/SEQPACKET 向对端传播反向方向，DGRAM 保留已排队 payload/SCM；Unix listener 的读关闭会封住新连接并按原顺序排空既有 pending。尚未完成的是 INET 未连接调用的方向副作用、连接中/已连接 TCP 的统一错误消费，以及 INET listener 读关闭后的退监听状态机。
- AArch64 已接入 199 `socketpair`、201 `listen`、202 `accept` 与 242 `accept4`。返回 fd 会先以不可见事务预留，socketpair 的两个编号逐字写回后才创建协议对象，accept 则在地址写回成功后才发布 accepted fd；满表不会提前消费 pending 连接或 AF_UNIX 在途 SCM。AF_UNIX 的 STREAM、DGRAM、RAW 归一化可用；本地 SEQPACKET 当前只覆盖 socketpair，并保持记录截断、清洁对端关闭后的 EOF/HUP、写入 EPIPE，以及关闭端存在未读记录时对端一次性的 `ECONNRESET/POLLERR/SO_ERROR`，命名、connect 与 listen 仍未接入。DGRAM connect 使用单向弱路由，支持非对称重连、显式替代目的地后恢复、同名重绑身份校验、dead-peer 一次性错误和关闭时无分配分批拆边；hidden transport 在 guest bind 前已排队的记录保留发送时的匿名源快照。accepted fd 不继承 Darwin listener 的非阻塞状态。监听恢复会保存 backlog、非阻塞与重绑定所需 option，并在挂起快照中持有内部 fd 身份锚点；恢复时以同一 open-file-description 共享的状态变化拒绝同号外来 socket，而不是依赖 Darwin 对 socket 不唯一的 `fstat` 字段。仍有效的宿主 listener 保持原身份，失效时仅在原 raw fd 号未被复用且 Unix 后备路径身份匹配时安全重建，再重新挂接既有 poll 后端登记并恢复 listen。当前协议要求集成方在 guest/socket 生命周期静止期由主线程串行调用，并保持宿主 raw fd 与内部身份锚点仅由 socket 层持有、不被外部长期复制或绕过 socket 锁替换；身份与监听代次复核用于失败关闭保护，不使 poll 重登记成为可与 shutdown/listen 并发的通用事务。这里的恢复证据来自受控宿主 fd 失效测试，不等于已经完成真机生命周期验收。
- 产品网络矩阵已经覆盖 iPhone/Watch Simulator 的系统 resolver、IPv4、HTTP、HTTPS 与 `apk update`；命令行夹具还覆盖本地 UDP DNS A 查询、并行 AAAA 无数据响应及 `getent`/`nslookup`。尚未认证 DNS 截断后的 TCP 回退、完整 IPv6 resolver、IP ancillary、UDP GSO 或全部 socket 选项。
- Apple core 门禁验证五切片的 AArch64 auto 后端选择、C/threaded 归档共存、函数指针 ABI、`arm64e` 指针认证，以及 core 与完整静态库消费者的链接闭包、重定位、Mach-O 平台和 minOS。独立 Xcode 门禁另行构建 iPhone App、Watch 四切片 LinkSmoke 和 Watch Simulator App；专用 Simulator 已运行双端产品矩阵。实体 iPhone/Watch 的签名、entitlement、安装、挂起恢复和 App Store 交付仍无最终证据。
- Alpine 冒烟是目标工作负载验证，不等价于完整发行版兼容认证。
