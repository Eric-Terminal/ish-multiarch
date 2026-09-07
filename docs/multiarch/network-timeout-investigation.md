# 网络超时兼容性排查

## 已确认的问题

2026-09-07 使用 ETOS LLM Studio 实际携带的 Alpine 3.24.1 AArch64
种子，在 `90c1923a` 上复现了网络命令超时失效。AArch64 系统调用分发
缺少 103 `setitimer`；musl 的 `alarm()` 依赖它。BusyBox `nc` 和 `wget`
设置 alarm 时未把失败当作致命错误，因此连接正常时仍可成功，目标不响应
时却失去预期的超时退出能力。

| 工作负载 | 修复前 | 修复后（macOS 宿主） |
| --- | --- | --- |
| `nc -vz -w 1 192.0.2.1 22` | 被宿主 4 秒硬超时终止 | 约 1.18 秒返回连接超时 |
| `wget -T 1 -O /dev/null http://192.0.2.1/` | 被宿主 4 秒硬超时终止 | 约 1.12 秒返回下载超时 |
| `nc -l -w 1 -p 0`，无客户端连接 | 被宿主 4 秒硬超时终止 | 约 1.09 秒退出 |
| 正常的本地 TCP、HTTP 和公网 HTTPS | 可以成功 | 继续可以成功 |

`192.0.2.1` 仅用于本次人工复现，自动回归使用本地无人连接的监听端口和
接受连接后不发送响应的 HTTP 服务，不依赖公网丢包或特定路由。

参考项目 `ish-arm64` 将 AArch64 的 103 直接接到其原生 64 位
`sys_setitimer`。多架构项目不能直接复用 i386 的 32 位 `itimerval`，本次
增加独立 LP64 编解码，并共用线程组定时器服务。

## 实现与覆盖范围

- 接入 AArch64 102 `getitimer`、103 `setitimer` 的 `ITIMER_REAL`，复用
  既有 SIGALRM、fork/exec 和退出清理路径；i386 保持原有 wire 布局。
- 覆盖微秒校验、超过 32 位的秒数、Linux KTIME_MAX 饱和、NULL 停用、
  非对齐高地址、地址越界、旧值写回失败的副作用顺序与实际信号投递。
- `ITIMER_VIRTUAL`、`ITIMER_PROF` 仍未实现；它们衡量进程 CPU 时间，
  不能用墙钟定时器冒充。本修复不宣称支持这两类 CPU 定时器。
- Alpine 冒烟覆盖 DNS A/AAAA、HTTP、拒绝未受信任的 HTTPS 证书、信任证书
  后的成功下载、TCP 连接、监听超时、
  HTTP 响应超时；Apple 命令桥探针同时要求成功结果和零兼容性诊断。

Linux 语义依据：[getitimer/setitimer 手册](https://www.man7.org/linux/man-pages/man2/setitimer.2.html)、
[Linux itimer 实现](https://github.com/torvalds/linux/blob/master/kernel/time/itimer.c)。

## 为什么“工具完成”不足以判断兼容性

使用旧库和真实 Alpine，通过公共 `ish_apple_command_session_start` 执行
一次正常的 `nc -z -w 1`：命令返回 `exit=0`，但该请求的诊断队列中有两条
`process=nc syscall=setitimer(103) errno=-38`。新库同样的连接加监听超时
工作负载返回成功，诊断数量为零。

这表明当前内核的未实现系统调用采集能够捕获此问题。客户端也在任务结束
时排空剩余事件；仅凭折叠的工具截图，不能认定其展示或反馈链路丢失了事件。
正常的连接拒绝、路由不可达、DNS 失败和缺少 `ssh` 可执行文件，本身也不等于
未实现指令，不会自动变成 SIGILL 诊断。

可用下面的探针复查客户端入口；它会启动实际 runtime、执行脚本并打印
完成原因、耗时和诊断事件。应传入一次性 fakefs，启动时会配置其 DNS。

```sh
build/aarch64_apple_command_probe /tmp/ish-a64-alpine \
    'nc -l -w 1 -p 0; test $? -eq 1'
```

## 真机与交付边界

截图中的 SSH 目标地址、原始 stderr 和 iPhone 的本地网络授权状态未知，
因此不能把这次修复等同于对该用户内网 22 端口的连通性确认。宿主本地网络
权限也约束 BSD socket，参见 [Apple TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)。

验证使用客户端原始种子和公共命令桥在 macOS 上执行，另对相关源码执行
iOS arm64 与 watchOS arm64_32 编译检查；没有把这些结果当作实体 iPhone
或 Apple Watch 的运行证据。

本次 13 项定向 Meson 回归通过，包含新增 itimer ABI、原有 i386 定时器、
timerfd、nanosleep、socket 和 Apple 命令桥测试；C 与 threaded 后端均通过
新增 itimer 测试和真实 `nc` 连接/超时验证。

额外执行完整 `apk update` 时，两个 HTTPS 仓库索引均已下载至缓存（约
517 KB 与 2.4 MB），之后仍在解释执行索引解析，最终触及宿主 300 秒上限。
因此这里只把该次运行作为 HTTPS 传输证据，不记作完整包管理验收通过；
索引解析速度是仍需单独处理的性能边界。
