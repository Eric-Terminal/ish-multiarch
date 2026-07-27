# Alpine AArch64 许可证与声明锁

本目录覆盖固定 Alpine 3.24.1 AArch64 种子中的 16 个二进制包，以及 BusyBox 静态吸收的
utmps 与 skalibs 源码。它不替代仓库根目录的项目许可证，也不覆盖宿主 App 使用的其他
第三方组件。

## 锁定文件

- `packages.tsv` 固定包名、版本、apk origin、apk 许可证表达式与 aports commit。
- `static-link-sources.tsv` 把两只静态源码的版本、ISC 许可和 aports 构建点快照限定到
  `busybox@1.37.0-r31`，但不把快照误称为依赖 APK 自身的 build commit。
- `source-assets.tsv` 固定 12 份 aports 目录归档和 11 份上游源码资产。
- `license-inputs.tsv` 把每个包版本映射到许可与来源证据，并锁定源码成员、大小、SHA-256、
  声明 section 与 section SHA-256。同一输入可以支持多个声明 section，同一 section 也可以
  汇总多个输入；其中 `license` 列记录包元数据表达式，不覆盖源码成员自己的许可声明。
- `license-inputs/` 保存无法从前述源码资产直接取得的权威文本字节，包括完整 LGPL 2.1
  正文。
- `THIRD-PARTY-NOTICES.txt` 保存 19 个具名声明 section。marker 之外只允许空行，所有正文
  都由 `license-inputs.tsv` 的摘要保护。

## 离线验证

```sh
python3 tools/apple-aarch64-rootfs-licenses.py check-locks
python3 tools/apple-aarch64-rootfs-licenses.py validate-sources \
    build/alpine-source-cache
```

`check-locks` 不读取源码缓存，也不访问网络。它验证 16 个二进制包、12 个源码 origin、
23 份源码资产、全部声明 section 和权威文件的闭合关系，并确认每份权威文本在所声明的
section 中逐字出现且只出现一次。

`validate-sources` 在此基础上读取已有源码缓存，复核每份资产的大小与 SHA-512，再从归档中
唯一定位清单指定的普通文件并核对其大小与 SHA-256。静态来源的 APKBUILD 还必须逐字锁定
`pkgname/pkgver/pkgrel`。它不会下载或执行这些源码。

合成回归可单独运行：

```sh
tools/apple-aarch64-rootfs-licenses-test.sh "$(command -v python3)"
```

## 解释边界

包的许可证表达式来自固定 apk 元数据；版权与署名内容只取自固定源码输入或清单锁定的权威
文本。APKBUILD 的 maintainer 或 contributor 字段不被解释为版权所有者。部分 Alpine origin
没有提供可确认的所有者声明，因此通用 MIT 模板会诚实保留 `<year>` 和
`<copyright holders>` 占位符。utmps 与 skalibs 的版本来自 BusyBox 构建点的 aports 快照；
现有固定二进制没有保留可证明两只依赖 APK 独立 build commit 的 `.BUILDINFO`，因此清单不
虚构该字段。

完整 GPLv2 正文逐字取自锁定的 `pax-utils-1.3.9/COPYING`。apk-tools 自带的 `LICENSE`
存在改词与错误并行，仍作为上游来源证据保留，但不再充当共用 GPLv2 正文。BusyBox
`volume_id` 的 21 个输入与 pax-utils `elf.h` 明确保留 LGPL-2.1-or-later 声明；
`volume_id/bcache.c` 只写 LGPL、没有指定版本。本文不从 apk 的 GPL-2.0-only 元数据推断
组合程序的法律结论，也不声称已经执行 LGPL 2.1 第 3 节的 notice replacement。公开发行前
必须解决 bcache 的版本依据，并基于实际交付物确认采用该转换，或提供未转换 LGPL 条款要求
的可重新链接材料。

校验器自动证明完整源码成员、权威文本和 section 各自的字节身份；除权威文本的逐字包含关系
外，普通源码成员到 section 的摘录选择仍依赖本次人工内容审计，并非通用许可解释器。
musl 的 `crypt_sha256.c`、`crypt_sha512.c` 与 `fmtmsg.c` 只声明 public domain、没有要求
保留的 notice，其源码字节仍在对应源码包中。

这些检查不构成法律意见。Alpine 声明资源与只读查看入口已经接入普通 iPhone 和 Watch
App，并由静态归属门禁、双端 UI 用例及公开 CI 的 bundle 字节比较覆盖；iSH+Linux
明确排除该资源。独立的项目许可正文与当前公开仓库入口已经进入普通 iSH、iSH+Linux 与
iSHWatch 三种 App，因此 iPhone/Watch 的共享页面会按产品实际资源组合显示项目许可和
Alpine 声明。
Apple 宿主侧 libarchive 与 hterm 闭包已经使用独立的确定性正文和产品资源门禁；三个
已交付 Material 图标的同时期上游快照、固定格式化关系和 Apache-2.0 适用证据也已由该
宿主锁闭合。但 BLAKE2 许可分支、wcwidth Unicode 13.0.0 三份 UCD 输入及许可、
lib_colors 的 W3C/X11 来源、libarchive 的 UnicodeData/UAX #15 来源及可选 Linux 产品
范围仍须单独闭合。项目许可与规范仓库入口的产品接线不等于精确二进制 revision 或
Release 已经绑定：这些剩余外部来源、上述 LGPL 发行决策和固定对应源码制品仍须在公开
release 位置形成可回读、可校验的完整交付链。
