# iSHApple 集成说明

`iSHApple` 是供 iOS 与 watchOS 宿主 App 使用的进程内 Linux runtime
接口。它复用产品中的同一套 iSH 内核、AArch64 guest 与 fakefs，不会启动
第二个 App，也不通过 URL Scheme 或进程间通信转发命令。

公共交付由两层组成：

- `iSHApple.xcframework`：稳定的 C v1 ABI、静态运行时和 Clang module；
- `Sources/iSHAppleSwift/`：基于 Swift 6 结构化并发的源码包装层。

当前门禁覆盖 iOS device `arm64`、iOS Simulator `arm64`/`x86_64`、
watchOS device `arm64_32`/`arm64` 和 Watch Simulator
`arm64`/`x86_64`。这些是构建与链接证据；实体设备签名、安装、挂起恢复和
App Store 发行仍需集成 App 自行验收。

## 生成产物

需要完整 Xcode、Meson 与 Ninja：

```sh
MESON="$(command -v meson)" \
NINJA="$(command -v ninja)" \
tools/apple-core-gate.sh
```

公共产物位于：

```text
build-apple-core/xcframeworks/iSHApple.xcframework
```

脚本固定使用两个 Ninja 作业，构建七个 ABI 切片，严格编译公共实现，合并
`libish`、`libish_emu` 与 `libfakefs`。打包阶段会先解析内部跨对象引用，
再把 `PublicSymbols.txt` 之外的定义局部化，避免把内核内部符号带入集成
App 的全局链接命名空间；最后从 XCFramework 的成品 Headers 对七个独立
模块消费者逐一链接。产物不是受跟踪文件。

## Xcode 接入

把 `iSHApple.xcframework` 加入 App target，选择静态链接而不是嵌入。
target 还需要链接以下系统库：

- `libsqlite3.tbd`
- `libresolv.tbd`
- `libm.tbd`
- `libdl.tbd`

C 使用 `#include <iSHApple.h>`，Objective-C/Objective-C++ 可以使用
`@import iSHApple;` 或包含同一头文件。Swift 项目可以直接导入底层 C
module；若需要 async/await 接口，把 `Sources/iSHAppleSwift/` 作为独立源码
target 编译，并让它依赖 `iSHApple`。公开包装以模块名 `iSHAppleSwift`
通过 Swift 6 完整并发检查。

可运行的最小 Swift 消费者位于
`examples/iSHAppleCommandConsumer/`。以下命令只做包装层和七个 Apple
目标的编译检查，不启动 Simulator：

```sh
sdk/iSHApple/Tests/typecheck.sh
```

## 启动顺序

每个宿主进程只有一个 Linux runtime，启动成功后由所有可见终端与结构化
命令会话共享：

1. 把项目生成的只读 fakefs seed 放入 App bundle。
2. 用 `ish_apple_rootfs_install_seed` 或 Swift `RootFS.installSeed` 安装到
   App 私有持久目录。
3. 以安装目录中的 `data/`、一个真实的共享目录、短 socket prefix、
   hostname 和 PID 1 启动命令调用 `ish_apple_runtime_start`。
4. runtime 返回成功后，再创建结构化命令会话。

`root_data` 必须指向 fakefs 的 `data/`；`shared_directory` 会挂载为
guest 的 `/mnt/shared`。真机上的 socket prefix 应放在 App 临时目录并
控制在 82 个 UTF-8 字节以内。runtime 不能在同一进程内停止后重启；若启动
失败，应读取 phase 和 last error，再由宿主决定是否结束当前进程生命周期。

## 结构化命令

底层接口接收 executable、argv、完整环境数组和可选工作目录，不接受未经
区分的 shell 字符串。需要 shell 语法时显式执行：

```swift
let request = CommandRequest(
    requestID: 1,
    executable: "/bin/sh",
    argv: ["/bin/sh", "-lc", "uname -a"],
    environment: ["PATH=/usr/bin:/bin"],
    workingDirectory: "/"
)
let session = try CommandSession.start(request)

async let output: Void = {
    for try await event in session.output {
        // 按 event.stream 处理原始字节。
    }
}()
let result = try await session.result()
try await output
```

必须并发消费 `output` 和等待 `result()`。Swift 层的输出队列有固定字节
容量；消费者落后时，原生输出回调会反压 guest，而不是继续占用内存。若只
等待结果却不读取输出，一个持续输出的命令会按设计停在反压边界。

stdin 写入是非阻塞的，Swift `send` 会处理部分写入和 Linux `EAGAIN`。
任务取消会取消整个 guest 作业；该作业身份不受 guest 的 `fork`、`setsid`
或 `setpgid` 影响，因此后台后代不能继续持有管道逃逸。显式 timeout 和
stdout/stderr 合计输出上限也会终止整个作业。

资源合同如下。活跃命令由动态注册表管理，不设置 SDK 固定会话配额；宿主可按
实际用途并发启动，资源不足时会收到真实的 Linux errno 或进程终止结果。

| 项目 | 上限或默认值 |
| --- | --- |
| argv 或 environment 项数 | 各 4096 |
| argv 与 environment 合计字节 | 128 KiB |
| 单路径字节 | 4096 |
| 单次 stdin 写入 | 最大 2,147,483,647 字节 |
| 原生输出回调块 | 最大 16 KiB |
| 合计输出 | 默认 8 MiB，最大 64 MiB |
| wall-clock timeout | 默认 300 秒，最大 1 小时 |

同一 session 的回调不会重叠，也不会在持有 session 内部锁时进入宿主代码；
不同 session 可以并发。每路输出各有一次终止事件，两路都终止后才产生完成
结果。C 调用统一返回 `0` 或负 Linux errno，不能拿 Darwin `<errno.h>` 的
值直接比较；应使用 `iSHAppleLinuxErrno.h` 中的
`ISH_APPLE_LINUX_*` 常量。结果中的 signal 也是 Linux 信号编号。

## C 句柄生命周期

`ish_apple_command_session_start` 成功时交付一个调用方引用。回调可能在线程
上早于 `start` 返回，但 `session_out` 一定先写入。回调中的 session 是借用
引用；需要跨出回调保存时必须 `retain`，使用结束后 `release`。最后一个
公开引用释放会请求取消并关闭 stdin，但已经进入的回调仍可安全 retain。

不要在 stream 或 completed 回调中同步调用 `wait`；该调用会返回
`ISH_APPLE_LINUX_EDEADLK`。回调中的 `bytes` 和 result 指针只在当前调用
期间有效，需要保存时复制。

## RootFS 与许可

XCFramework 不包含 Alpine rootfs、对应源码包或 App 的许可页面。集成方必须
使用项目的受控 rootfs 工具生成并校验 seed，并把所分发二进制对应的完整源码、
GNU GPL 文本、项目声明和 Alpine/宿主依赖声明接入自己的产品。

项目许可边界以仓库根目录的 `LICENSE.md` 与 `LICENSE.IOS` 为准。静态链接
`iSHApple` 不会消除 GPL 义务；发布 App 前应按实际源码 revision 重新运行
来源、许可、产品资源与成品链接门禁。
