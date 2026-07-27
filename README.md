# [iSH](https://ish.app)

> **多架构实验分支：** 本仓库保留官方 iSH 的完整历史、i386 guest 与原有许可，并独立增加 AArch64 Linux guest。普通 iPhone App 和独立的 SwiftUI Watch App 都会安装固定 Alpine AArch64 种子并启动同一套 iSH 兼容内核；专用 iPhone/Watch Simulator 已通过 shell、文件、进程、信号、线程、DNS、HTTP/HTTPS、包管理、SQLite、Python、本地 C/pthread、本地 Git 与离线 SSH 客户端固定软件矩阵。Apple 构建门禁同时覆盖 iOS device `arm64`、watchOS device `arm64_32`/`arm64` 和 Watch Simulator `arm64`/`x86_64`；实体设备签名运行与 App Store 交付仍待最终验证。门禁生成的 XCFramework 没有公共头文件、模块映射或稳定 API 承诺，不是公共 SDK。它不是官方 iSH 发行版，完整状态、复现命令和已知边界见[多架构实现说明](docs/multiarch/README.md)。

[![Build Status](https://github.com/Eric-Terminal/ish-multiarch/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Eric-Terminal/ish-multiarch/actions/workflows/ci.yml)
[![goto counter](https://img.shields.io/github/search/ish-app/ish/goto.svg)](https://github.com/ish-app/ish/search?q=goto)
[![fuck counter](https://img.shields.io/github/search/ish-app/ish/fuck.svg)](https://github.com/ish-app/ish/search?q=fuck)
[![shit counter](https://img.shields.io/github/search/ish-app/ish/shit.svg)](https://github.com/ish-app/ish/search?q=shit)

<p align="center">
<a href="https://ish.app">
<img src="https://ish.app/assets/github-readme.png">
</a>
</p>

This branch retains the i386 Linux guest through usermode x86 emulation and adds an experimental AArch64 Linux guest path with syscall translation.

For the current status of the project, check the issues tab, and the commit logs.

以下 App Store、TestFlight、Wiki 与社区链接属于上游 iSH；它们不表示本分支的
AArch64 或 Watch App 已经通过这些渠道发布。

- [App Store page](https://apps.apple.com/us/app/ish-shell/id1436902243)
- [TestFlight beta](https://testflight.apple.com/join/97i7KM8O)
- [Discord server](https://discord.gg/HFAXj44)
- [Wiki with help and tutorials](https://github.com/ish-app/ish/wiki)
- [README 中文](README_ZH.md) (如若未能保持最新，请提交 PR 以更新)

# Hacking

This project has a git submodule, make sure to clone with `--recurse-submodules` or run `git submodule update --init` after cloning.

You'll need these things to build the project:

 - Python 3
   + Meson (`pip3 install meson`)
 - Ninja
 - Clang and LLD (on mac, `brew install llvm`, on linux, `sudo apt install clang lld` or `sudo pacman -S clang lld` or whatever)
 - sqlite3 (this is so common it may already be installed on linux and is definitely already installed on mac. if not, do something like `sudo apt install libsqlite3-dev`)
 - libarchive (`brew install libarchive`, `sudo port install libarchive`, `sudo apt install libarchive-dev`) TODO: bundle this dependency

## Build for iOS

Open the project in Xcode, open iSH.xcconfig, and change `ROOT_BUNDLE_IDENTIFIER` to something unique. You'll also need to update the development team ID in the project (not target!) build settings. Then click Run. There are scripts that should do everything else automatically. If you run into any problems, open an issue and I'll try to help.

Apple 发行候选没有默认 profile，必须显式选择 `core` 或 `with-linux`。
`core` 由普通 `iSH` iPhone App 和 `iSHWatch` 组成；`with-linux` 由与其
互斥的 `iSH+Linux` iPhone App 和同一个 `iSHWatch` 组成。FileProvider
只是所选 iPhone App 的嵌入扩展；静态库、XCFramework、aggregate 和
LinkSmoke 只用于构建验收，不属于候选产品。profile 门禁通过不表示许可
义务已经闭合或产品已经可以发布，具体命令与边界见
[多架构实现说明](docs/multiarch/README.md#apple-发行候选-profile)。

## 构建与验证 watchOS App

共享 Scheme `iSHWatch` 构建真正的 SwiftUI Watch App，包含终端、AArch64
rootfs 首次安装和 Linux runtime；`iSHWatchUITests` 覆盖启动、终端快捷键和
固定软件矩阵。`iSHCore-watchOS` 仍是跨架构静态库与 XCFramework 打包门禁，
`iSHWatchLinkSmoke` 则只验证最终链接闭包。请使用
[多架构实现说明](docs/multiarch/README.md#xcode-scheme-验收)中的完整 App
与逐切片元数据核验命令；fat 产物必须分别检查各架构的平台和最低系统版本，
也不要把无签名 SDK 构建当作实体设备运行证据。

## Build command line tool for testing

To set up your environment, cd to the project and run `meson build` to create a build directory in `build`. Then cd to the build directory and run `ninja`.

To set up a self-contained Alpine linux filesystem, download the Alpine minirootfs tarball for i386 from the [Alpine website](https://alpinelinux.org/downloads/) and run `./tools/fakefsify`, with the minirootfs tarball as the first argument and the name of the output directory as the second argument. Then you can run things inside the Alpine filesystem with `./ish -f alpine /bin/sh`, assuming the output directory is called `alpine`. If `tools/fakefsify` doesn't exist for you in your build directory, that might be because it couldn't find libarchive on your system (see above for ways to install it.)

You can replace `ish` with `tools/ptraceomatic` to run the program in a real process and single step and compare the registers at each step. I use it for debugging. Requires 64-bit Linux 4.11 or later.

## Logging

iSH has several logging channels which can be enabled at build time. By default, all of them are disabled. To enable them:

- In Xcode: Set the `ISH_LOG` setting in iSH.xcconfig to a space-separated list of log channels.
- With Meson (command line tool for testing): Run `meson configure -Dlog="<space-separated list of log channels>"`.

Available channels:

- `strace`: The most useful channel, logs the parameters and return value of almost every system call.
- `instr`: Logs every instruction executed by the emulator. This slows things down a lot.
- `verbose`: Debug logs that don't fit into another category.
- Grep for `DEFAULT_CHANNEL` to see if more log channels have been added since this list was updated.

# A note on the interpreter

Possibly the most interesting thing I wrote as part of iSH is the interpreter. It's not quite a JIT since it doesn't target machine code. Instead it generates an array of pointers to functions called gadgets, and each gadget ends with a tailcall to the next function; like the threaded code technique used by some Forth interpreters. The result is a speedup of roughly 3-5x compared to emulation using a simpler switch dispatch.

Unfortunately, I made the decision to write nearly all of the gadgets in assembly language. This was probably a good decision with regards to performance (though I'll never know for sure), but a horrible decision with regards to readability, maintainability, and my sanity. The amount of bullshit I've had to put up with from the compiler/assembler/linker is insane. It's like there's a demon in there that makes sure my code is sufficiently deformed, and if not, makes up stupid reasons why it shouldn't compile. In order to stay sane while writing this code, I've had to ignore best practices in code structure and naming. You'll find macros and variables with such descriptive names as `ss` and `s` and `a`. Assembler macros nested beyond belief. And to top it off, there are almost no comments.

So a warning: Long-term exposure to this code may cause loss of sanity, nightmares about GAS macros and linker errors, or any number of other debilitating side effects. This code is known to the State of California to cause cancer, birth defects, and reproductive harm.
