# 常用 AArch64 浮点与 NEON 指令覆盖

本次补齐沿用 #145 的真实软件兼容方向，选择基础数学运算、浮点舍入、
浮点转整数和编译器可以生成的基础 NEON 算术。实现依据公开 Arm 语义，
通过原生 AArch64 硬件独立验证结果，不依赖宿主 C 浮点环境。

## 支持范围与计数口径

`guest/aarch64/decode.h` 的有效操作类别由 **226 增至 248**，不计
`AARCH64_OP_COUNT`。这是项目内部的解码/执行类别数：同一助记符的标量、
向量或不同寻址形式可能分属多个类别，不能用它除以某一版本 Arm 手册的
助记符数量来表示完整 ISA 覆盖率。

本次新增 18 个标量助记符和 4 个 NEON 操作类别，共 65 种宽度/排列形式：

| 操作 | 新增支持 |
| --- | --- |
| `FABS` | S、D；保留 NaN 载荷，不改变异常标志 |
| `FMIN`、`FMAX`、`FMINNM`、`FMAXNM` | S、D；区分负零、quiet NaN 和 signaling NaN |
| `FRINTN/P/Z/A/I/X` | S、D；与原有 `FRINTM` 共用舍入实现 |
| `FCVTMS/NS/NU/PS/PU/AS/AU` | S/D 到 W/X；与原有 `FCVTZS/ZU/MU` 共用范围检查 |
| NEON 整数 `MUL` | 8B、16B、4H、8H、2S、4S；按通道截断乘积 |
| NEON `FADD`、`FSUB`、`FMUL` | 2S、4S、2D；每个通道计算并累积 FPSR |

显式舍入方向的指令不受 FPCR.RMode 干扰；`FRINTI/X` 读取该字段。
`FRINTX` 与浮点转整数在结果不精确时累积 IXC，非法整数转换优先报告 IOC。
FZ、DN、输入非规格数、NaN 优先级和饱和值均有独立测试。

threaded 后端通过现有 C 回落执行这些新类别，缓存首次解码和命中路径均已覆盖。
本次不扩展浮点异常陷阱机制，也不宣告新的 HWCAP；FP16、SVE、SME、
向量舍入/转换和按元素乘法仍不属于本次支持范围。

## 软件样本证据

2026-09-15 从 Alpine v3.24 main 的 AArch64 包提取 ELF，按可执行节中的
四字节对齐机器字统计本次新增类别。以下是**静态编码出现次数**，不表示
执行频率或整款软件已完成兼容认证；可执行节中的常量池也可能影响静态统计。

| 样本 | 新增编码出现次数 | 主要命中 |
| --- | ---: | --- |
| BusyBox 1.37.0-r31：`busybox` | 3 | `FABS` |
| musl 1.2.6-r2：`ld-musl-aarch64.so.1` | 46 | `FABS`、`FMINNM/FMAXNM`、舍入和转换 |
| Node.js 24.18.1-r0：`node` | 933 | `FABS`、多种 `FRINT`、整数转换、NEON 算术 |
| Python 3.14.7-r1：`libpython3.14.so.1.0` | 30 | `FABS`、`FRINTP/A`、NEON `FADD/FSUB` |
| 同包的 `python3.14` 与 `libpython3.so` | 0 | 主逻辑位于上面的核心动态库 |

其中 Node 的 `FABS` 有 691 处、`FRINTP` 有 81 处、`FRINTZ` 有 69 处；
Python 核心库的向量 `FADD/FSUB` 各有 2 处。样本未命中 `FCVTNS/NU/AU`，
这三种形式随同一组通用舍入/转换语义补齐，避免只覆盖部分舍入方向。

## 回归与复现

新增三个 Meson 测试：

- `aarch64_scalar_fp_round`：141,696 组原生舍入/转换对照。
- `aarch64_scalar_fp_minmax`：61,440 组原生绝对值/极值对照。
- `aarch64_advsimd_basic_arithmetic`：122,880 组原生向量算术对照。

合计 **326,016 组原生对照**；每组分别检查 C、threaded 首次解码、threaded
缓存命中后的完整 CPU 状态。固定边界预期在所有宿主运行；原生对照仅在
AArch64 宿主编译执行。测试还穷举相关寄存器字段，拒绝 FP16/保留编码，
覆盖源/目标重叠、V31、WZR/XZR、高位清零、既有异常保留、随机位模式，
以及 4 种 RMode × FZ × DN 的 16 种 FPCR 配置。

在配置好的 native 构建目录运行：

```sh
meson test -C build-telemetry-native --print-errorlogs \
    aarch64_scalar_fp_round aarch64_scalar_fp_minmax \
    aarch64_advsimd_basic_arithmetic
meson test -C build-telemetry-native --print-errorlogs
```

Apple 主机沿用项目构建环境，调用前清除外部 `SDKROOT`、`LIBRARY_PATH`、
`CPATH`、`C_INCLUDE_PATH`、`CPLUS_INCLUDE_PATH`、`OBJC_INCLUDE_PATH`。
已有 FP-to-integer、FCVTMU、FRINTM、标量浮点及 NEON 测试仍参与完整回归。

本次 macOS AArch64 验证结果：完整 Meson **235/235 通过**，其中 e2e
内部 **8/8 通过**；新增三组测试另以 AddressSanitizer 和 UndefinedBehaviorSanitizer
构建运行，均通过，未报告内存或未定义行为错误。

语义参考：Arm 的 [FRINTX](https://www.scs.stanford.edu/~zyedidia/arm64/frintx_float.html)、
[FMINNM](https://www.scs.stanford.edu/~zyedidia/arm64/fminnm_float.html) 与
[FCVTAS](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtas_float.html) 指令定义。
