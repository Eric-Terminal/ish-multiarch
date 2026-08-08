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
- `libz.tbd`

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

1. 把项目生成的只读 fakefs seed，或工具生成的压缩归档及清单放入 App
   bundle。
2. 目录 seed 使用 `ish_apple_rootfs_install_seed`；压缩 seed 使用
   `ish_apple_rootfs_install_archive` 并传入清单固定的 SHA-256、展开字节数和
   条目数。Swift 分别对应 `RootFS.installSeed` 与
   `RootFS.installArchive`。两种入口都安装到 App 私有持久目录。
3. 以安装目录中的 `data/`、一个真实的共享目录、短 socket prefix、
   hostname 和 PID 1 启动命令调用 `ish_apple_runtime_start_v2`；需要在
   首个进程接受作业前可见的目录通过 `startupMounts` 一并传入。
4. runtime 返回成功后，再创建结构化命令会话。

`root_data` 必须指向 fakefs 的 `data/`；`shared_directory` 会挂载为
guest 的 `/mnt/shared`。真机上的 socket prefix 应放在 App 临时目录并
控制在 82 个 UTF-8 字节以内。runtime 不能在同一进程内停止后重启；若启动
失败，应读取 phase 和 last error，再由宿主决定是否结束当前进程生命周期。

## 压缩 RootFS seed

发布构建可把已经 fakefs 化并验证完成的目录 seed 压成确定性归档：

```sh
tools/apple-rootfs-seed-archive.py \
    /path/to/seed /path/to/rootfs.tar.gz \
    --metadata /path/to/rootfs.json
```

JSON 清单记录归档 SHA-256、压缩/展开字节数、条目数、AArch64、Alpine
版本、上游摘要和 seed manifest 摘要。宿主必须把其中的
`archiveSHA256`、`uncompressedBytes` 与 `entryCount` 原样传给安装 API；
不能根据设备上的归档重新计算后再把结果当成可信期望值。

安装器只接受该工具输出的 gzip/USTAR 普通目录与普通文件，不接受符号链接、
设备节点、绝对路径、路径穿越、重复条目或 USTAR 扩展。它先校验整个压缩文件，
再在现有托管 staging 中展开，最后复用 seed manifest、AArch64 BusyBox、
SQLite fakefs 与 hardlink 校验并原子发布。进度回调返回非零只会在安全检查点
取消；已存在且收据有效的 RootFS 仍返回 `alreadyPresent`，不会被归档覆盖。

启动返回后应读取一次 `Runtime.capabilities()` 并把结果保存到当前进程的运行时
状态。快照明确报告 PTY、动态挂载、结构化诊断、guest 文件接口、AArch64 架构、
实际 C/threaded 后端和公共 ABI 版本；在 runtime 尚未接受作业时返回 `EAGAIN`，
不会让 UI 根据版本号推测能力。当前进程进入 RUNNING 后这些字段保持不变。

## 多目录挂载

`RuntimeMountConfiguration` 只接收调用方已经打开的目录 fd，不接收宿主
路径。SDK 在调用返回前复制 fd，因此 security-scoped URL 的授权、File
Provider 物化和原 fd 生命周期仍由 App 管理；guest、状态结构和诊断中只会
出现稳定 UUID 与 `/mnt/` 下的 guest 路径。`/mnt/shared` 保留给 runtime 的
兼容共享目录，其他挂载使用 `/mnt/etos/<stable-id>`、`/mnt/icloud` 等路径。

```swift
let selected = RuntimeMountConfiguration(
    id: mountID,
    hostDirectoryDescriptor: directoryFD,
    guestDirectory: "/mnt/etos/01234567",
    access: .readOnly
)
try await RuntimeMounts().add(selected)
```

只读属性进入 guest mount flags，写打开、创建、删除、重命名、链接、属性和
时间戳修改都会返回 Linux `EROFS`，不是 Swift 侧的软约束。registry 使用动态
链表，不设置挂载数量常量。

作业在启动前应通过 `acquireLease(id:)` 持有 lease。普通 `remove` 会先把
条目改为 draining、拒绝新 lease，并在宿主 lease 或 guest fd/cwd 引用仍活跃
时返回 `EBUSY`；最后一个 lease 释放后会自动尝试卸载。`force: true` 可以忽略
宿主 lease，但不会释放或伪造仍被 guest 内核引用的对象，调用方应先取消关联
作业并在引用退出后重试。卸载只移除映射，不删除宿主目录或文件。

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
需要让用户自己决定长任务何时结束时，可分别传入
`CommandRequest.timeoutDisabled` 与 `CommandRequest.outputLimitDisabled`；
关闭 bridge 终止阈值不等于允许停止消费输出，宿主仍必须持续 drain 并落盘。

资源合同如下。活跃命令由动态注册表管理，不设置 SDK 固定会话配额；宿主可按
实际用途并发启动，资源不足时会收到真实的 Linux errno 或进程终止结果。

| 项目 | 上限或默认值 |
| --- | --- |
| argv 或 environment 项数 | 各 4096 |
| argv 与 environment 合计字节 | 128 KiB |
| 单路径字节 | 4096 |
| 单次 stdin 写入 | 最大 2,147,483,647 字节 |
| 原生输出回调块 | 最大 16 KiB |
| 合计输出 | 默认 8 MiB，可设最大 64 MiB 或显式关闭终止阈值 |
| wall-clock timeout | 默认 300 秒，可设最大 1 小时或显式关闭 |

同一 session 的回调不会重叠，也不会在持有 session 内部锁时进入宿主代码；
不同 session 可以并发。每路输出各有一次终止事件，两路都终止后才产生完成
结果。C 调用统一返回 `0` 或负 Linux errno，不能拿 Darwin `<errno.h>` 的
值直接比较；应使用 `iSHAppleLinuxErrno.h` 中的
`ISH_APPLE_LINUX_*` 常量。结果中的 signal 也是 Linux 信号编号。

## 交互式终端

`TerminalSession` 与管道式 `CommandSession` 是两种不同的进程合同。前者为
每个会话创建独立 PTY、控制终端与前台进程组，适用于登录 Shell、readline、
编辑器、`top`、SSH 和 REPL；后者保留独立 stdout/stderr 和结构化结果，适合
Agent 普通命令与 stdio MCP。两者共享同一个 runtime、RootFS 和挂载，但不
共享 cwd、环境变更、stdin/stdout 或 Shell 历史。

```swift
let terminal = try TerminalSession.start(
    TerminalRequest(
        terminalID: 2,
        executable: "/bin/sh",
        argv: ["/bin/sh", "-l"],
        environment: [
            "TERM=xterm-256color",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "HOME=/root",
        ],
        workingDirectory: "/root",
        columns: 80,
        rows: 24
    )
)

async let output: Void = {
    for try await event in terminal.output {
        // event.bytes 是未解码的 PTY 字节；交给宿主终端解析器。
    }
}()
try await terminal.send(Array("uname -a\n".utf8))
try terminal.resize(columns: 100, rows: 30)
let result = try await terminal.result()
try await output
```

终端数量由动态注册表和系统实际资源决定，SDK 不设置固定配额。raw output 在
底层按需扩容，Swift 包装使用有界事件通道限制已进入 Swift 的待消费数据；消费
停滞时原生缓冲继续保存 PTY 输出。若宿主内存分配失败，事件与最终结果会准确
报告 `droppedBytes`，不会把缺失输出伪装成完整。
`finishInput()` 与 `interrupt()` 分别写入真实终端的 Ctrl-D 与 Ctrl-C；
`resize` 更新 `TIOCSWINSZ` 并沿用 iSH 的 `SIGWINCH`、控制终端和前台进程组
语义。调用方传入的是完整 guest environment，runtime 不继承宿主进程环境。

## 结构化兼容性诊断

命令和终端的 request ID 会在进程发布前写入不可变的宿主诊断归属，并由
`fork` 后代继承。未定义 AArch64 指令和返回 `ENOSYS` 的未实现 syscall
不会只剩一段 stderr：SDK 会产生带精确 guest PC、opcode、syscall
number/name、负 Linux errno、signal、guest architecture、实际 C/threaded
后端与构建身份的 `DiagnosticEvent`。普通程序以非零状态退出不产生兼容性
事件。

```swift
let diagnostics = RuntimeDiagnostics()
let scope = DiagnosticScope.command(requestID: request.requestID)
while try await diagnostics.pendingCount(for: scope) != 0 {
    for event in try await diagnostics.drain(
        scope,
        maximumEventCount: 128
    ) {
        // 先落盘，再由 ETOS 生成模型可见的脱敏说明或反馈引用。
    }
}
```

底层队列不设事件数量常量；`maximumEventCount` 只是单次跨 ABI 复制的批量，
宿主应在任务运行期间和完成时持续分批 drain。C API 也可用
`events == NULL, capacity == 0` 查询数量，或用
`ish_apple_diagnostics_clear` 明确丢弃某个 scope 的待消费事件。runtime
启动故障使用全局 runtime scope 和 `requestID == 0`；command、terminal 与
guest 文件请求均要求非零且在活跃任务间唯一的 ID。

Apple 交付门禁把当前 Git revision 写入 `build_identity`；没有 Git 元数据的
普通源码构建使用明确的 `ish-multiarch-source`，不会伪造某个提交哈希。

## Guest 文件系统

`GuestFileSystem` 供 App 文件浏览器和 Agent 的结构化文件工具使用。路径始终
是 Linux guest 中的绝对路径，接口通过同一套路径解析、fakefs 元数据和 mount
权限执行；不得用宿主 `FileManager` 直接修改 rootfs 的 `data/`，否则会绕过
fakefs 数据库并破坏权限、符号链接和 inode 语义。

```swift
let files = GuestFileSystem()
let page = try files.list(
    path: "/root/project",
    requestID: 200,
    cursor: 0,
    maximumEntryCount: 128
)
let contents = try files.read(
    path: "/root/project/main.c",
    requestID: 201,
    offset: 0,
    maximumByteCount: 64 * 1024
)
try files.edit(
    path: "/root/project/main.c",
    requestID: 202,
    offset: 10,
    removedByteCount: 4,
    replacement: Array("value".utf8)
)
try files.copy(
    path: "/root/project/main.c",
    to: "/root/project/main.backup.c",
    requestID: 203
)
```

`list` 使用不透明 cursor 分页，`read` 使用 64 位 offset；两者都不会要求把整
个目录或文件一次装进内存。`write` 与 `edit` 在同目录创建临时文件，完整写入
并同步后才以 guest rename 发布。`edit` 会流式复制未修改区间，并在提交前发现
目标被并发替换时返回 `ESTALE`。`copy` 在 guest 内使用固定缓冲区复制普通文件，
并通过同目录 staging 原子替换目标；失败时保留原目标。失败只清理由本次请求创建
的精确临时文件。

删除、重命名和递归建目录同样执行 guest 操作。只读 mount 的创建、写入、删除
和重命名由内核返回 `EROFS`；递归删除不会跟随最终符号链接，但如果用户明确对
挂载目录执行递归删除，仍会按 Linux 可见命名空间访问其内容。SDK 不设置文件
大小、目录项总数或递归目标数量的产品配额，单次 ABI 复制量由调用方选择。

所有调用均为同步阻塞接口，应由 App 的后台任务或 actor 调用，不能放进 SwiftUI
渲染链。每次请求使用非零唯一 `requestID`，遇到 `ENOSYS`/`ENOTSUP` 时可从
`DiagnosticScope.guestFile` 消费对应结构化兼容性诊断。

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
