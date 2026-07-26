# Apple 宿主交付输入锁

本目录固定 Apple 产品实际使用的宿主第三方输入及其 target 路由。它回答
“哪个锁定源码进入哪个产品、以什么交付单元进入、后续生成声明时必须复核
哪些原始许可输入”，不替代法律审查，也不生成最终第三方声明。

## 锁文件

- `dependencies.tsv` 固定组件版本、子模块来源与 gitlink、交付单元，以及
  实际构建输入路径集合的数量和摘要。
- `target-inputs.tsv` 区分普通 iPhone App、Watch App、嵌入扩展和可选的
  `iSH+Linux`。Apple SDK 项只表示平台提供，不表示仓库分发其源码。
- `license-inputs.tsv` 固定独立许可文本与源码内的声明复核点。表中
  `inline-notice` 是后续生成宿主声明的输入，不代表已经替版权方选择许可
  分支或完成义务判断。

路径集合摘要统一按仓库根目录相对 POSIX 路径排序，每项追加一个 LF，再对
完整 UTF-8 字节串计算 SHA-256。`libarchive` 集合来自其 Xcode target 的
128 个实际编译源；hterm 集合静态展开四份 concat 文件、普通源码、文件型
资源与 ChangeLog，共 53 项。`date` 的运行时值不属于文件输入；
`git-rev HEAD` 由同一份 `deps/libapps` gitlink 固定。

## 当前产品边界

- `iSH` 交付 `libarchive.a` 和由 `deps/libapps` 生成的
  `hterm_all.js`。后者同时包含 hterm、libdot、intl-segmenter 与 wcwidth。
- `iSHWatch` 不链接 libarchive，也不携带 hterm；其显式外部链接项均来自
  Apple SDK。`arm64_32` 不会因此获得另一套宿主第三方来源。
- `iSHFileProvider` 只链接本项目生成的核心库，没有单独的 vendored
  子模块输入。
- `iSH+Linux` 还从锁定的 `deps/linux` 构建 `liblinux.a`，但是否公开分发
  该产品仍是独立的产品决定；本锁不能代替 Linux GPLv2 交付方案。

`libarchive/COPYING` 明确以各源码文件内的声明为准。本目录因此同时锁定
Xcode 编译源集合和当前已识别的 UC Regents、BLAKE2、xxHash、NetBSD、
PPMd、7zip 公有领域等复核点，但仍不宣称宿主 notices 已经生成完毕。

## 校验

```sh
python3 tools/apple-host-delivery-inputs.py check-locks
```

校验器只读取 Git 索引、子模块、Xcode 工程、xcconfig、concat 和锁文件；
它要求四个交付 target 真实存在，并穷尽核对三个既有 gitlink 在主工程
Frameworks、Resources、Sources 与 Copy Files phase 中的直接输入。静态
libarchive 还会回溯主工程产品代理到锁定子工程的唯一静态库产品；重复
输入、弱链接属性、条件过滤和未声明路径都会失败。校验不联网、不生成
资源、不构建 App，也不启动 Simulator。

## 不在本锁内闭合的事项

- Alpine AArch64 guest seed 使用相邻的独立来源、许可和对应源码锁。
- 项目自身 GPL 正文与当前 fork 源码入口仍须形成产品内可证明的交付链。
- BusyBox 静态输入的 LGPL 版本依据和最终发行方案仍须单独决定。
- `iSH+Linux` 现有在线 root 下载、产品声明和 Linux 对应源码交付仍未闭合。
- 公共 Release、真机安装运行与从公开位置回读资产不由本地输入锁代替。
- 本锁不是通用 SBOM；以后引入三个既有 gitlink 之外的宿主代码时，必须
  重新审计并显式扩展清单与门禁。
