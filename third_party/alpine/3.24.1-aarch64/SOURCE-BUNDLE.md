# Alpine AArch64 对应源码包

该源码包对应本项目固定的 Alpine 3.24.1 AArch64 minirootfs。包内
`manifest/binary-reference.tsv` 锁定二进制归档及 apk installed 数据库，
`manifest/packages.tsv` 记录归档里的 16 个二进制包，`manifest/origins.tsv`
记录 10 个二进制包 origin 和 2 个静态源码 origin 的 aports 提交与 Git tree，
`manifest/static-link-sources.tsv` 把后两项限定为 BusyBox 使用的 utmps 与 skalibs，
`manifest/source-assets.tsv` 再锁定实际源码载荷。

`aports/` 中每份归档包含该提交下对应 origin 的 `APKBUILD`、补丁、配置、构建辅助文件，
并保留 Git tree 可表达的文件类型、可执行位与符号链接目标；`distfiles/` 包含这些
APKBUILD 引用的 11 份外部上游输入。两部分合在一起，保留了构建这些包所使用的源码与控制
编译、安装的脚本。

utmps 与 skalibs 使用 BusyBox 构建提交处的 aports 全树快照，并把版本逐字绑定到该快照的
`APKBUILD`；这里的快照提交不被解释为两只历史依赖 APK 各自的 build commit。

源码资产保持原始归档格式，没有混入 minirootfs 二进制或生成后的 fakefs。每项大小、来源
URL 和 SHA-512 都记录在 `manifest/source-assets.tsv`，整个确定性 tar 的 SHA-256 则由项目
旁边的 `corresponding-source.sha256` 锁定。

这个包只处理固定 Alpine rootfs 的对应源码输入。仓库外层另用 `license-inputs.tsv`、
`license-inputs/` 与 `THIRD-PARTY-NOTICES.txt` 锁定许可证和版权声明；这些文件不混入本
源码包。对应源码的公开发行位置、双端 App 资源与查看入口仍是独立门禁，不能因为生成了本
源码包或声明文件就视为已经完成全部发行合规。
