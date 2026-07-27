# Apple 宿主交付输入锁

本目录固定 Apple 产品实际使用的宿主第三方输入、target 路由和可复现的
宿主声明正文。它回答“哪个锁定源码进入哪个产品、以什么交付单元进入、声明
逐字取自哪里”，但不替代法律审查，也不表示整个产品的发行义务已经闭合。

## 锁文件

- `dependencies.tsv` 固定组件版本、子模块来源与 gitlink、交付单元，以及
  实际构建输入路径集合的数量和摘要。
- `target-inputs.tsv` 区分普通 iPhone App、Watch App、嵌入扩展和可选的
  `iSH+Linux`。Apple SDK 项只表示平台提供，不表示仓库分发其源码。
- `license-inputs.tsv` 固定许可、源码内声明和来源证据的完整文件摘要。
  `license` 表示收入正文的完整许可，`inline-notice` 表示可提取片段的源码
  复核输入，`provenance` 只表示来源或生成证据；后二者都不替版权方选择许可
  分支，也不证明外部许可已经闭合。
- `notice-fragments.tsv` 固定 22 段来源、生成或许可证据，以仓库相对路径、
  1 起始闭区间、字节数和 SHA-256 约束原始字节。
- `APPLE-HOST-NOTICES.txt` 是确定性生成物，正文带锁定组件版本、全部来源
  路径和原始文本摘要；相同字节只保留一份，来源不会随去重丢失。

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

`libarchive/COPYING` 明确以各源码文件内的声明为准。声明生成器从 Xcode
target 的 128 个实际 `.c` 输入出发，递归跟随 `deps/libarchive` gitlink
内存在的本地双引号 include，当前得到 37 个头文件。它逐文件提取开头连续
的 C 块注释；同一文件连续的多个块连同其原始间隔组成一个提取单元，再按
整个单元的精确字节去重。`notice-fragments.tsv` 另行固定 22 段证据：
13 段来自 hterm 交付输入及其锁定生成来源：Material 图标上游 README、
libdot ChangeLog、
`lib_colors.js`、wcwidth `lib_wc.js` 与 `ranges.py`；其中 `ranges.py`
只作为 provenance，不进入 53 项实际交付闭包。另有 9 段来自 libarchive
的中段来源或许可文本。
普通代码里名称相近的 `copyright-file` 选项注释不会被搜索启发式误收。

闭包当前固定五个有意不解析的条件 include：
`archive.h`/`archive_entry.h` 各自引用的 `android_lf.h`、两份 BLAKE2
reference `.c` 引用的 `blake2-kat.h`，以及 `archive_platform.h` 引用的
生成型 `config.h`。前四项在当前 gitlink 中不存在；Apple 手工配置由已锁定
的 `deps/config.h` 提供，不属于 gitlink notices 闭包。除此之外的任何
缺失 include 都会失败，不能被静默忽略。

当前收入宿主正文的八条完整许可归属记录中，Unicode Data Files 许可由
wcwidth 与 libarchive 共享同一路径，hterm 与 libdot 的原始字节也相同，
因此去重后仍是 6 份唯一完整许可。165 个 libarchive 编译/include 闭包文件
得到 97 份唯一前导文本；22 个锁定片段当前也各自唯一。生成器由此固定
`6 + 97 + 22 = 125` 个唯一正文 section，再加 `overview`、
`material-provenance`、`wcwidth-unicode-provenance`、
`libarchive-unicode-provenance` 与 `unresolved-provenance` 五个边界
section，共有 130 对 BEGIN/END 标记。
数量漂移会使永久回归失败，避免提取范围在无人察觉时扩张或收缩。

hterm、libdot、intl-segmenter、wcwidth、`libarchive/COPYING` 和
Material Design Icons 同期快照的根 `LICENSE` 按完整文件字节收入；
Material README 只收入完整 License section，三个上游 SVG 只保留锁定
输入及格式化映射证据。Unicode Data Files 完整许可来自官方
unicodetools 固定提交；聚合显示只移除原文件开头的 UTF-8 BOM，其余正文
逐字保留。去重只比较最终收入正文的字节是否完全相同，不合并“看起来
等价”的许可文本，也不改写版权方、条款或 public-domain 字样。Linux
`COPYING` 仍由输入锁复核，但正文明确排除，因为公共宿主声明不能冒充
`iSH+Linux` 的 Linux kernel 许可与对应源码交付方案。

## 来源纳入政策与已知缺口

来源片段只在锁定文件或锁定本地历史明确使用 imported、taken from、
adapted from、derived from、generated from 等来源/生成表述时纳入。仅用于
解释行为的标准名称、术语或普通参考链接不会触发无边界的递归扩面；一旦存在
明确来源表述，则固定最小原始片段，并把尚未取得的上游字节、版本和权威条款
如实保留为未决项。

- 三个 hterm find bar Material SVG 的仓内字节和 libapps 导入提交
  `de5387e902ef285c2d2c6909a53d37d826843551` 已经固定；该提交只说明图标
  取自 `google/material-design-icons`，没有记录精确 revision。本锁选择
  作者时间点的官方 master tip
  `3d4a32b327272c458e12586437c3ca0696b28a69` 作为同时期不可变快照，
  固定三个 `production` 上游 SVG、根 README 的 Apache-2.0 适用范围和
  根 LICENSE 原字节。当前 SVG 只比上游多固定的元素间缩进和文件尾 LF；
  校验器离线逐字重放该变换。该证据只闭合这三个已交付图标，不声称导入者
  明确选择了该 revision，也不外推到其他来源。
- wcwidth 的 `lib_wc.js` 原始移植块逐字记录 node.js `wcwidth.js` 与 npm
  来源；完整 LICENSE 和 METADATA 固定其许可与版本。生成脚本和 ChangeLog
  证据把当前三张表标识为 Unicode 13.0.0；对应本地历史提交为
  `6b9f6ee9b9c94cfa4e3adf049c906610d1623ee8`。本锁保存官方
  unicodetools tag `release-2020-09-15` 对应提交
  `a87ae283358bf1858e7cbf6520c6bba0d3b58710` 的 `PropList.txt`、
  `UnicodeData.txt`、`EastAsianWidth.txt` 和根 LICENSE；三份数据与固定
  UCD 13.0.0 发布归档成员逐字一致，final ReadMe 另行确认最终版本身份。
  校验器用历史 `ranges.py` 在临时目录离线重放三张哨兵表，结果必须逐字
  恢复当前 `lib_wc.js`。这证明所选官方材料能重建当前表，不声称 libapps
  作者当年明确使用了该 Git 提交；UCD.zip 本身也不含 LICENSE。
- `lib_colors.js` 明确说明 HSL 算法改编自 W3C CSS Color 4，颜色名称表派生
  自 stock X11 `rgb.txt`。仓内没有所用上游版本、X11 原始数据或对应权威
  条款，收入来源注释不表示外部许可已经确定。
- `archive_string_composition.h` 明确由 Unicode 6.0.0 的
  `UnicodeData.txt` 生成，`archive_string.c` 的 Hangul 组合常量和流程
  明确来源于 Unicode Standard Annex #15。本锁保存官方固定提交中与
  Unicode 6.0.0 发布归档逐字一致的 `UnicodeData.txt`、
  `CompositionExclusions.txt` 和 final ReadMe；当前生成脚本在隔离临时
  目录、C locale 下先恢复历史生成基线，再经过唯一的拼写修正与 header
  guard 前移，逐字得到当前头文件。另一路语义校验以 81 个显式排除项和
  UnicodeData 的单码点、非 starter 规则逐项恢复正反两张 931 项表。
  Hangul 常量固定到 Unicode 6.0.0 对应的 UAX #15 Revision 33 URL、大小和
  摘要；技术报告全文不是生成输入，也不复制进仓库或产品。
- Unicode 6.0.0 归档本身没有 LICENSE，数据只链接当时的可变 Terms of
  Use 的精确主体是 final ReadMe 与 `CompositionExclusions.txt`。本工程
  记录的许可依据来自 Unicode 官方 2020 固定 Git 树的根 LICENSE；该树
  同时包含逐字相同的数据与许可文件。这不冒充 2010 归档内许可原件，也不
  声称 Unicode 在该提交中专门重新许可旧数据。子模块中的 libarchive 测试
  源码、压缩夹具及其 `NormalizationTest.txt` 来源未进入 Apple App 编译、
  include 或资源闭包，留给未来完整子模块源码包审计。

## 生成与校验

生成器还会验证两个锁定的 libapps 历史提交、提交说明和对应 blob，离线
重放 Material 上游快照到当前三个 SVG 的固定格式化关系，用锁定的历史
`ranges.py` 与三份 UCD 输入重建 wcwidth 三张表，并用当前 libarchive
生成器和 Unicode 6.0.0 输入重建其固定头文件链。libarchive 重放只使用
当前 gitlink 文件，不要求历史对象；浅克隆只需补齐 `deps/libapps` 历史：

```sh
if [ "$(git -C deps/libapps rev-parse --is-shallow-repository)" = true ]; then
    git -C deps/libapps fetch --no-tags --unshallow origin
fi
```

上述 fetch 是浅克隆的一次性联网准备，不属于下面的只读离线校验；公开 CI
也只补齐这个子模块，不拉取无关子模块的完整历史。

```sh
python3 tools/apple-host-delivery-inputs.py check-locks
python3 tools/apple-host-notices.py check-locks
python3 tools/apple-host-notices.py render
```

第一条只读取 Git 索引、子模块、Xcode 工程、xcconfig、concat 和锁文件；
它要求四个交付 target 真实存在，并穷尽核对三个既有 gitlink 在主工程
Frameworks、Resources、Sources 与 Copy Files phase 中的直接输入。

第二条在内存中重建正文并与受跟踪生成物逐字节比较，不写文件。只有显式
`render` 才在全部输入和片段验证通过后，以同目录临时文件原子替换固定输出；
任何失败都保留旧文件且不遗留临时文件。两种校验都不联网、不构建 App，也
不启动 Simulator。

`APPLE-HOST-NOTICES.txt` 只应进入普通 `iSH` 与可选 `iSH+Linux` 的资源。
`iSHWatch` 没有 libarchive/hterm 宿主输入，因此不显示本文件；其共享许可页
显示独立的项目许可与 Alpine seed 声明。`iSHFileProvider`、测试 target 与
LinkSmoke 不应获得本文件。

## 不在本锁内闭合的事项

- Alpine AArch64 guest seed 使用相邻的独立来源、许可和对应源码锁。
- 项目许可正文与当前公开仓库入口使用独立的确定性资源，已经进入普通
  `iSH`、`iSH+Linux` 与 `iSHWatch`；它仍不等于二进制已经绑定并公开了精确
  release revision、gitlink 和对应源码资产。
- BusyBox 静态输入的 LGPL 版本依据和最终发行方案仍须单独决定。
- `iSH+Linux` 现有在线 root 下载、产品声明和 Linux 对应源码交付仍未闭合。
- BLAKE2 源码给出 CC0、OpenSSL 或 Apache 2.0 三选一，本生成器不替发行者
  选择分支。
- W3C/X11 来源缺口仍会阻断公共发行；Material 图标、wcwidth UCD 与
  libarchive Unicode 的证据闭合不表示其他未知边界或法律审查已经完成。
- 精确二进制 revision、公共 Release、真机安装运行与从公开位置回读项目及
  对应源码资产不由本地输入锁代替。
- 本锁不是通用 SBOM；以后引入三个既有 gitlink 之外的宿主代码时，必须
  重新审计并显式扩展清单与门禁。
