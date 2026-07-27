# Apple 产品项目许可与源码入口锁

本目录把仓库根目录的 `LICENSE.md`、`LICENSE.IOS`，以及它们明确引用的
GNU GPLv2、GPLv3 完整原文，确定性汇集为 `PROJECT-LICENSES.txt`。生成物还
记录当前项目的公开源码入口：

`https://github.com/Eric-Terminal/ish-multiarch`

收入两种 GPL 原文只是在产品中完整保留根许可说明引用的文本，不替发行者
选择 GPLv2 或 GPLv3 路径，也不构成法律结论。Linux kernel、Alpine guest、
Apple 宿主第三方组件及其对应源码仍由各自的发行门禁处理。

## 锁定输入

`inputs.tsv` 固定每个 section 的角色、仓库内路径、权威 URL、字节数与
SHA-256。其中：

- `LICENSE.md` 与 `LICENSE.IOS` 逐字读取仓库根目录文件；
- `GPL-2.0.txt` 逐字取自
  `https://www.gnu.org/licenses/gpl-2.0.txt`；
- `GPL-3.0.txt` 逐字取自
  `https://www.gnu.org/licenses/gpl-3.0.txt`；
- `source` 行固定公开源码仓库入口，不把可变分支当成精确发行源码证据。

法律原文保持原始英文和 LF 字节，不翻译、不改写。只有聚合后的
`PROJECT-LICENSES.txt` 应进入普通 iSH、iSH+Linux 与 iSHWatch 产品；本目录
中的输入锁和四份原始输入不应重复进入 App bundle。

## 生成与验证

只读校验会在内存中重建正文并与受跟踪输出逐字节比较，不访问网络，也不写
文件：

```sh
python3 tools/apple-project-license-notices.py check-locks
```

只有显式 `render` 才会在全部输入验证通过后原子替换生成物：

```sh
python3 tools/apple-project-license-notices.py render
```

永久回归覆盖确定性、原文边界、固定 URL、锁摘要漂移、缺失输入、输出漂移与
失败不写：

```sh
python3 tools/apple-project-license-notices-test.py
```

## 边界

公开仓库 URL 是源码入口，不等于某个 App 二进制已经绑定到精确源码 revision。
精确 release commit/tag、主仓库与子模块源码资产、校验清单及从公开位置回读
资产，必须由后续独立 Release 门禁闭合。本文和生成器不会把该后续工作表述为
已经完成。
