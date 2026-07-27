#!/usr/bin/env python3

import base64
from contextlib import ExitStack
from dataclasses import replace
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parent.parent
TOOL_PATH = ROOT / "tools" / "apple-aarch64-lgpl-surface.py"


def load_surface_tool():
    spec = importlib.util.spec_from_file_location(
        "apple_aarch64_lgpl_surface_under_test", TOOL_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("无法载入 LGPL surface 校验器")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


SURFACE = load_surface_tool()

VERSIONED_LGPL = b"""/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
"""
UNVERSIONED_LGPL = b"""/*
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */
"""
VERSIONED_LGPL3 = b"""/*
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License version 3.
 */
"""
GPLV2 = b"""/*
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
"""
GPLV2_LATER = b"""/*
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
"""

BUSYBOX_LGPL_C = (
    "bcache.c",
    "btrfs.c",
    "cramfs.c",
    "exfat.c",
    "ext.c",
    "fat.c",
    "iso9660.c",
    "jfs.c",
    "linux_raid.c",
    "linux_swap.c",
    "luks.c",
    "minix.c",
    "nilfs.c",
    "ntfs.c",
    "ocfs2.c",
    "reiserfs.c",
    "udf.c",
    "util.c",
    "volume_id.c",
    "xfs.c",
)
BUSYBOX_GPLV2_C = (
    "erofs.c",
    "f2fs.c",
    "lfs.c",
    "squashfs.c",
    "ubifs.c",
)


def busybox_fixture():
    aports = {
        "aports/main/busybox/APKBUILD": b"""
pkgname=busybox
pkgver=1.37.0
pkgrel=31
source="https://busybox.net/downloads/busybox-$pkgver.tar.bz2
	harmless.patch
	acpid-poweroff.sh
	acpid.logrotate
	bbsuid.c
	busyboxconfig
	busyboxconfig-extras
	dad.if-up
	default.script
	securetty
	ssl_client.c
	udhcpc.conf
	$_openrc_files
	$_mdev_openrc_files
	$_extras_openrc_files
"
_config="$srcdir"/busyboxconfig

prepare() {
	default_prepare
}

build() {
	cd "$_dyndir"
	cp "$_config" .config
	make -C "$builddir" O="$PWD" silentoldconfig
	make CONFIG_EXTRA_CFLAGS="$_extra_cflags" CONFIG_EXTRA_LDLIBS="$_extra_libs"
}

package() {
	cd "$_dyndir"
	install -Dm755 busybox "$pkgdir"/bin/busybox
}
""",
        "aports/main/busybox/harmless.patch": (
            b"--- a/networking/example.c\n"
            b"+++ b/networking/example.c\n"
        ),
    }
    upstream = {}
    enabled = []
    active_names = (
        BUSYBOX_LGPL_C + BUSYBOX_GPLV2_C + ("get_devname.c",)
    )
    for index, name in enumerate(active_names):
        selector = "SURFACE_{:02d}".format(index)
        enabled.append("CONFIG_{}=y".format(selector))
        if name == "bcache.c":
            notice = UNVERSIONED_LGPL
        elif name in BUSYBOX_LGPL_C:
            notice = VERSIONED_LGPL
        elif name == "get_devname.c":
            notice = GPLV2_LATER
        else:
            notice = GPLV2
        body = notice + (
            "//kbuild:lib-$(CONFIG_{}) += {}\n"
            '#include "volume_id_internal.h"\n'
        ).format(selector, name[:-2] + ".o").encode()
        upstream[SURFACE.BUSYBOX_VOLUME_PREFIX + name] = body
    aports["aports/main/busybox/busyboxconfig"] = (
        "\n".join(enabled) + "\n"
    ).encode()
    upstream[SURFACE.BUSYBOX_PUBLIC_HEADER] = VERSIONED_LGPL
    upstream[SURFACE.BUSYBOX_INTERNAL_HEADER] = (
        VERSIONED_LGPL + b'#include "volume_id.h"\n'
    )
    upstream[SURFACE.BUSYBOX_KBUILD_GENERATOR] = (
        b"""#!/bin/sh
src="$srctree/$d/Kbuild.src"
dst="$d/Kbuild"
if test -f "$src"; then
    sed -n 's@^//kbuild:@@p' "$srctree/$d"/*.c \\
    | generate \\
        "${src}" "${dst}" \\
        "# DO NOT EDIT. This file is generated from Kbuild.src"
fi
exit 0
"""
    )
    expected = {
        SURFACE.BUSYBOX_VOLUME_PREFIX + name for name in BUSYBOX_LGPL_C
    }
    expected.update(
        {SURFACE.BUSYBOX_PUBLIC_HEADER, SURFACE.BUSYBOX_INTERNAL_HEADER}
    )
    return aports, upstream, expected


def pax_fixture():
    aports = {
        "aports/main/pax-utils/APKBUILD": b"""
pkgname=pax-utils
pkgver=1.3.9
pkgrel=1
subpackages="$pkgname-doc scanelf:_scanelf"
source="https://dev.gentoo.org/~sam/distfiles/app-misc/pax-utils/pax-utils-$pkgver.tar.xz
	"

build() {
	abuild-meson \\
		. output
	meson compile -C output
}

package() {
	DESTDIR="$pkgdir" meson install --no-rebuild -C output
}

_scanelf() {
	amove usr/bin/scanelf
}
"""
    }
    upstream = {
        SURFACE.PAX_ROOT + "/meson.build": b"""
executable('scanelf',
  'paxelf.c',
  'paxldso.c',
  'scanelf.c',
  version_h,
  dependencies : [libcap],
  link_with : common,
  install : true
)
""",
        SURFACE.PAX_ROOT + "/scanelf.c": b'#include "paxinc.h"\n',
        SURFACE.PAX_ROOT + "/paxinc.h": b'#include "elf.h"\n',
        SURFACE.PAX_ELF_HEADER: VERSIONED_LGPL,
    }
    return aports, upstream


def valid_elf():
    data = bytearray(121)
    data[:4] = b"\x7fELF"
    data[4] = 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<HH", data, 16, 3, 183)
    struct.pack_into("<I", data, 20, 1)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HHH", data, 52, 64, 56, 1)
    struct.pack_into(
        "<IIQQQQQQ",
        data,
        64,
        1,
        5,
        120,
        0,
        0,
        1,
        1,
        1,
    )
    data[120] = 0xC3
    return data


def tar_bytes(files, mode="w"):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode=mode) as archive:
        for path, value in sorted(files.items()):
            if isinstance(value, tuple):
                data, member_mode = value
            else:
                data, member_mode = value, 0o644
            member = tarfile.TarInfo(path)
            member.size = len(data)
            member.mode = member_mode
            archive.addfile(member, io.BytesIO(data))
    return result.getvalue()


class LGPLSurfaceTests(unittest.TestCase):
    def test_production_locks(self):
        result = subprocess.run(
            [sys.executable, "-B", str(TOOL_PATH), "check-locks"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
        self.assertEqual(
            result.returncode,
            0,
            "生产 LGPL 锁未通过：\n{}".format(result.stderr),
        )

    def test_validate_sources_main_wiring(self):
        args = SimpleNamespace(
            command="validate-sources",
            cache=Path("synthetic-cache"),
            payloads=Path("synthetic-payloads"),
        )
        parser = SimpleNamespace(parse_args=lambda: args)
        locks = SimpleNamespace(packages={})
        with ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    SURFACE,
                    "make_parser",
                    return_value=parser,
                )
            )
            stack.enter_context(
                mock.patch.object(
                SURFACE.LICENSE_TOOL,
                "load_locks",
                return_value=locks,
                )
            )
            stack.enter_context(
                mock.patch.object(
                    SURFACE,
                    "parse_payloads",
                    return_value={},
                )
            )
            stack.enter_context(
                mock.patch.object(SURFACE, "validate_fact_locks")
            )
            stack.enter_context(
                mock.patch.object(
                    SURFACE.LICENSE_TOOL,
                    "validate_sources",
                )
            )
            validate_surface = stack.enter_context(
                mock.patch.object(
                    SURFACE,
                    "validate_source_surface",
                )
            )
            self.assertEqual(SURFACE.main(), 0)
        validate_surface.assert_called_once_with(args.cache, locks)

    def test_payload_mapping_rejects_duplicate_and_drift(self):
        packages = {
            "busybox@1.37.0-r31": SimpleNamespace(origin="busybox"),
            "scanelf@1.3.9-r1": SimpleNamespace(origin="pax-utils"),
        }
        cases = {
            "重复 origin": (
                {
                    **packages,
                    "busybox@2-r0": SimpleNamespace(origin="busybox"),
                },
                (
                    SURFACE.PAYLOAD_HEADER
                    + "\n"
                    + "busybox\tbusybox@1.37.0-r31\tbin/busybox\n"
                    + "busybox\tbusybox@2-r0\tusr/bin/scanelf\n"
                ),
                "重复 source_origin",
            ),
            "规范路径漂移": (
                packages,
                (
                    SURFACE.PAYLOAD_HEADER
                    + "\n"
                    + "busybox\tbusybox@1.37.0-r31\tbin/busybox\n"
                    + "pax-utils\tscanelf@1.3.9-r1\tusr/bin/scanelf-alt\n"
                ),
                "规范路径发生漂移",
            ),
            "包 origin 漂移": (
                {
                    **packages,
                    "scanelf@1.3.9-r1": SimpleNamespace(origin="busybox"),
                },
                (
                    SURFACE.PAYLOAD_HEADER
                    + "\n"
                    + "busybox\tbusybox@1.37.0-r31\tbin/busybox\n"
                    + "pax-utils\tscanelf@1.3.9-r1\tusr/bin/scanelf\n"
                ),
                "固定包版本或 origin 不一致",
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "payloads.tsv"
            for name, (package_map, text, expected) in cases.items():
                with self.subTest(name=name):
                    path.write_text(text, encoding="utf-8")
                    with self.assertRaisesRegex(
                        SURFACE.SurfaceError, expected
                    ):
                        SURFACE.parse_payloads(path, package_map)

    def test_fact_lock_mapping_rejects_drift_and_substring_lures(self):
        args = SURFACE.make_parser().parse_args(["check-locks"])
        locks = SURFACE.LICENSE_TOOL.load_locks(args)
        payloads = SURFACE.parse_payloads(args.payloads, locks.packages)
        SURFACE.validate_fact_locks(locks, payloads)

        busybox, _ = SURFACE.select_lgpl_facts(locks)
        target = busybox[0]
        changed_input = replace(
            target,
            source_asset="distfiles/busybox/wrong.tar.bz2",
        )
        changed_inputs = tuple(
            changed_input if item is target else item
            for item in locks.inputs
        )
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "源码事实、payload 或声明范围"
        ):
            SURFACE.validate_fact_locks(
                replace(locks, inputs=changed_inputs),
                payloads,
            )

        member = target.source_member.encode()
        section_name = "busybox-embedded-notices"
        section = locks.sections[section_name]
        changed_section = section.replace(
            b"- " + member + b"\n",
            b"- lure/" + member + b"\n",
            1,
        )
        changed_sections = {
            **locks.sections,
            section_name: changed_section,
        }
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "源码事实、payload 或声明范围"
        ):
            SURFACE.validate_fact_locks(
                replace(locks, sections=changed_sections),
                payloads,
            )

        authority_inputs = tuple(
            item
            for item in locks.inputs
            if not (
                item.origin == "pax-utils"
                and item.source_kind == "authority"
                and item.source_asset == "license-inputs/LGPL-2.1.txt"
            )
        )
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "恰有一项 LGPL 2.1 权威正文映射"
        ):
            SURFACE.validate_fact_locks(
                replace(locks, inputs=authority_inputs),
                payloads,
            )

    def test_synthetic_source_cache_end_to_end(self):
        busybox_aports, busybox_upstream, expected = busybox_fixture()
        pax_aports, pax_upstream = pax_fixture()
        snapshots = (
            ("busybox", "aports", busybox_aports),
            ("busybox", "upstream", busybox_upstream),
            ("pax-utils", "aports", pax_aports),
            ("pax-utils", "upstream", pax_upstream),
        )
        facts = [
            SimpleNamespace(
                origin="busybox",
                source_kind="upstream",
                source_member=member,
            )
            for member in sorted(expected)
        ]
        facts.append(
            SimpleNamespace(
                origin="pax-utils",
                source_kind="upstream",
                source_member=SURFACE.PAX_ELF_HEADER,
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            assets = {}
            for index, (origin, kind, files) in enumerate(snapshots):
                data = tar_bytes(files)
                bundle_path = "snapshot-{}.tar".format(index)
                (cache / bundle_path).write_bytes(data)
                assets[bundle_path] = SimpleNamespace(
                    origin=origin,
                    kind=kind,
                    bundle_path=bundle_path,
                    size=len(data),
                    sha512=hashlib.sha512(data).hexdigest(),
                )
            locks = SimpleNamespace(
                assets=assets,
                inputs=tuple(facts),
            )
            SURFACE.validate_source_surface(
                cache,
                locks,
                require_reviewed_snapshots=False,
            )

    def test_reviewed_source_snapshot_fingerprints(self):
        args = SURFACE.make_parser().parse_args(["check-locks"])
        locks = SURFACE.LICENSE_TOOL.load_locks(args)
        SURFACE.validate_reviewed_source_snapshots(locks)
        target_key, target = next(
            (key, asset)
            for key, asset in locks.assets.items()
            if (asset.origin, asset.kind) == ("busybox", "aports")
        )
        changed_assets = {
            **locks.assets,
            target_key: replace(target, sha512="0" * 128),
        }
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "已审阅源码快照指纹"
        ):
            SURFACE.validate_reviewed_source_snapshots(
                replace(locks, assets=changed_assets)
            )

    def test_busybox_build_surface_drifts(self):
        baseline_aports, baseline_upstream, baseline_expected = (
            busybox_fixture()
        )
        SURFACE.validate_busybox_sources(
            baseline_aports, baseline_upstream, baseline_expected
        )

        def config_drift(aports, upstream):
            path = "aports/main/busybox/busyboxconfig"
            aports[path] = aports[path].replace(
                b"CONFIG_SURFACE_01=y",
                b"# CONFIG_SURFACE_01 is not set",
            )

        def kbuild_drift(aports, upstream):
            path = SURFACE.BUSYBOX_VOLUME_PREFIX + "btrfs.c"
            upstream[path] = upstream[path].replace(
                b"btrfs.o", b"missing.o"
            )

        def kbuild_path_drift(aports, upstream):
            path = SURFACE.BUSYBOX_VOLUME_PREFIX + "btrfs.c"
            upstream[path] = upstream[path].replace(
                b"btrfs.o", b"../../hidden/btrfs.o"
            )

        def include_drift(aports, upstream):
            path = SURFACE.BUSYBOX_VOLUME_PREFIX + "btrfs.c"
            upstream[path] = upstream[path].replace(
                b'#include "volume_id_internal.h"\n',
                b'/* #include "volume_id_internal.h" */\n',
            )

        def conditional_include_drift(aports, upstream):
            path = SURFACE.BUSYBOX_VOLUME_PREFIX + "btrfs.c"
            upstream[path] = upstream[path].replace(
                b'#include "volume_id_internal.h"\n',
                b'#if 0U\n#include "volume_id_internal.h"\n#endif\n',
            )

        def patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = (
                b"--- a/util-linux/volume_id/btrfs.c\n"
                b"+++ b/util-linux/volume_id/btrfs.c\n"
            )

        def noncanonical_patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = (
                b"--- a/util-linux/./volume_id/btrfs.c\n"
                b"+++ b/util-linux/./volume_id/btrfs.c\n"
            )

        def global_build_patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = b"--- a/Makefile\n+++ b/Makefile\n"

        def global_config_patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = b"--- a/Config.in\n+++ b/Config.in\n"

        def quoted_patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = (
                b'diff --git "a/scripts/gen_build_files.sh" '
                b'"b/scripts/gen_build_files.sh"\n'
                b'--- "a/scripts/gen_build_files.sh"\n'
                b'+++ "b/scripts/gen_build_files.sh"\n'
            )

        def generator_patch_drift(aports, upstream):
            path = "aports/main/busybox/harmless.patch"
            aports[path] = (
                b"--- a/scripts/gen_build_files.sh\n"
                b"+++ b/scripts/gen_build_files.sh\n"
            )

        def generator_decoy_drift(aports, upstream):
            upstream[SURFACE.BUSYBOX_KBUILD_GENERATOR] = (
                b"#!/bin/sh\n"
                b"exit 0\n"
                b"# sed -n 's@^//kbuild:@@p' \"$srctree/$d\"/*.c\n"
            )

        def generator_exec_drift(aports, upstream):
            path = SURFACE.BUSYBOX_KBUILD_GENERATOR
            upstream[path] = upstream[path].replace(
                b"#!/bin/sh\n",
                b"#!/bin/sh\nexec true\n",
                1,
            )

        def build_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b'\tmake -C "$builddir" O="$PWD" silentoldconfig\n', b""
            )

        def commented_build_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b'\tcd "$_dyndir"\n',
                b'\t# cd "$_dyndir"\n',
                1,
            )

        def grouped_build_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b'\tcd "$_dyndir"\n',
                b'\tfalse && {\n\tcd "$_dyndir"\n',
                1,
            ).replace(
                b'\tmake CONFIG_EXTRA_CFLAGS="$_extra_cflags" '
                b'CONFIG_EXTRA_LDLIBS="$_extra_libs"\n',
                b'\tmake CONFIG_EXTRA_CFLAGS="$_extra_cflags" '
                b'CONFIG_EXTRA_LDLIBS="$_extra_libs"\n\t}\n',
                1,
            )

        def heredoc_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = (
                b": <<'DECOY'\n" + aports[path] + b"DECOY\n"
            )

        def early_return_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b"build() {\n",
                b"build() {\n\tif true; then return; fi\n",
                1,
            )

        def function_override_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] += b"\nfunction build { :; }\n"

        def source_override_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] += b"\nsource=\n"

        def identity_control_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b"\npkgname=busybox\n",
                b"\nif false; then\npkgname=busybox\n",
                1,
            ).replace(
                b'_config="$srcdir"/busyboxconfig\n',
                b'_config="$srcdir"/busyboxconfig\nfi\n',
                1,
            )

        def source_config_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tbusyboxconfig\n", b""
            )

        def source_overlay_drift(aports, upstream):
            path = "aports/main/busybox/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tharmless.patch\n",
                b"\tharmless.patch\n\toverlay.tar.gz\n",
            )

        def grouped_generator_drift(aports, upstream):
            path = SURFACE.BUSYBOX_KBUILD_GENERATOR
            upstream[path] = upstream[path].replace(
                b'src="$srctree/$d/Kbuild.src"\n',
                b'false && {\nsrc="$srctree/$d/Kbuild.src"\n',
            ).replace(
                b"fi\nexit 0\n",
                b"fi\n}\nexit 0\n",
            )

        cases = (
            ("配置", config_drift, "26 个 active"),
            ("Kbuild", kbuild_drift, "不存在的 volume_id C"),
            ("Kbuild 路径", kbuild_path_drift, "Kbuild 对象格式非法"),
            ("include", include_drift, "不再消费 volume_id_internal.h"),
            (
                "条件禁用 include",
                conditional_include_drift,
                "不再消费 volume_id_internal.h",
            ),
            ("patch", patch_drift, "patch 触及已审计 LGPL 路径"),
            (
                "非规范 patch",
                noncanonical_patch_drift,
                "patch 成员路径不是规范相对路径",
            ),
            (
                "全局构建 patch",
                global_build_patch_drift,
                "patch 触及已审计 LGPL 路径",
            ),
            (
                "全局配置 patch",
                global_config_patch_drift,
                "patch 触及已审计 LGPL 路径",
            ),
            (
                "quoted patch",
                quoted_patch_drift,
                "patch 含不支持或不安全的成员路径",
            ),
            (
                "Kbuild 生成器 patch",
                generator_patch_drift,
                "patch 触及已审计 LGPL 路径",
            ),
            (
                "Kbuild 生成器诱饵",
                generator_decoy_drift,
                "Kbuild 生成器",
            ),
            (
                "Kbuild 生成器提前 exec",
                generator_exec_drift,
                "Kbuild 生成链",
            ),
            ("构建函数", build_drift, "动态配置与 make 构建链"),
            (
                "注释构建命令",
                commented_build_drift,
                "动态配置与 make 构建链",
            ),
            (
                "不执行的构建分组",
                grouped_build_drift,
                "不支持的 shell 控制结构",
            ),
            ("here-document", heredoc_drift, "here-document"),
            ("提前 return", early_return_drift, "提前终止命令"),
            (
                "function 覆盖",
                function_override_drift,
                "函数定义不唯一",
            ),
            ("source 覆盖", source_override_drift, "source 清单"),
            (
                "身份字段进入死分支",
                identity_control_drift,
                "固定顶层字段",
            ),
            (
                "source 缺少配置",
                source_config_drift,
                "固定上游 source 或 busyboxconfig",
            ),
            (
                "source 增加覆盖归档",
                source_overlay_drift,
                "固定上游 source 或 busyboxconfig",
            ),
            (
                "不执行的生成器分组",
                grouped_generator_drift,
                "Kbuild 生成链",
            ),
        )
        for name, mutate, expected_error in cases:
            with self.subTest(name=name):
                aports, upstream, expected = busybox_fixture()
                mutate(aports, upstream)
                with self.assertRaisesRegex(
                    SURFACE.SurfaceError, expected_error
                ):
                    SURFACE.validate_busybox_sources(
                        aports, upstream, expected
                    )

    def test_busybox_fact_scope_drifts(self):
        missing = SURFACE.BUSYBOX_VOLUME_PREFIX + "btrfs.c"
        extra = SURFACE.BUSYBOX_VOLUME_PREFIX + "erofs.c"

        def remove_fact(facts):
            facts.remove(missing)

        def replace_fact(facts):
            facts.remove(missing)
            facts.add(extra)

        for name, mutate in (
            ("事实缺失", remove_fact),
            ("范围替换", replace_fact),
        ):
            with self.subTest(name=name):
                aports, upstream, expected = busybox_fixture()
                mutate(expected)
                with self.assertRaisesRegex(
                    SURFACE.SurfaceError, "许可输入清单不一致"
                ):
                    SURFACE.validate_busybox_sources(
                        aports, upstream, expected
                    )

    def test_busybox_license_boundaries(self):
        negated_version = VERSIONED_LGPL.replace(
            b"version 2.1 of the License, or (at your option) any later version.",
            b"not version 2.1 of the License and not any later version.",
        )
        edition_lure = UNVERSIONED_LGPL.replace(
            b" */\n",
            b" * This notice selects the third edition.\n */\n",
        )
        cases = (
            (
                "bcache 被版本化",
                "bcache.c",
                VERSIONED_LGPL,
                "无版本 LGPL 事实被擅自解释",
            ),
            (
                "bcache 被改成 v3",
                "bcache.c",
                VERSIONED_LGPL3,
                "无版本 LGPL 事实被擅自解释",
            ),
            (
                "普通成员去版本化",
                "btrfs.c",
                UNVERSIONED_LGPL,
                "LGPL-2.1-or-later 原文发生漂移",
            ),
            (
                "普通成员否定版本",
                "btrfs.c",
                negated_version,
                "LGPL-2.1-or-later 原文发生漂移",
            ),
            (
                "bcache edition 诱饵",
                "bcache.c",
                edition_lure,
                "无版本 LGPL 事实被擅自解释",
            ),
        )
        for name, member, replacement, expected_error in cases:
            with self.subTest(name=name):
                aports, upstream, expected = busybox_fixture()
                path = SURFACE.BUSYBOX_VOLUME_PREFIX + member
                marker = upstream[path].find(b"//kbuild:")
                upstream[path] = replacement + upstream[path][marker:]
                with self.assertRaisesRegex(
                    SURFACE.SurfaceError, expected_error
                ):
                    SURFACE.validate_busybox_sources(
                        aports, upstream, expected
                    )

    def test_pax_build_and_license_drifts(self):
        baseline_aports, baseline_upstream = pax_fixture()
        SURFACE.validate_pax_sources(
            baseline_aports, baseline_upstream
        )

        def target_drift(aports, upstream):
            path = SURFACE.PAX_ROOT + "/meson.build"
            upstream[path] = upstream[path].replace(
                b"'scanelf.c'", b"'other.c'"
            )

        def include_drift(aports, upstream):
            path = SURFACE.PAX_ROOT + "/paxinc.h"
            upstream[path] = b'/* #include "elf.h" */\n'

        def conditional_include_drift(aports, upstream):
            path = SURFACE.PAX_ROOT + "/paxinc.h"
            upstream[path] = b'#if 0U\n#include "elf.h"\n#endif\n'

        def subpackage_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"scanelf:_scanelf", b"scanelf"
            )

        def license_drift(aports, upstream):
            upstream[SURFACE.PAX_ELF_HEADER] = UNVERSIONED_LGPL

        def build_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tmeson compile -C output\n", b""
            )

        def commented_build_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tabuild-meson \\\n",
                b"\t# abuild-meson \\\n",
            )

        def grouped_build_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tabuild-meson \\\n",
                b"\tfalse && {\n\tabuild-meson \\\n",
            ).replace(
                b"\tmeson compile -C output\n",
                b"\tmeson compile -C output\n\t}\n",
            )

        def commented_grouped_build_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tabuild-meson \\\n",
                b"\tfalse && { # disabled\n\tabuild-meson \\\n",
            ).replace(
                b"\tmeson compile -C output\n",
                b"\tmeson compile -C output\n\t} # disabled\n",
            )

        def commented_subshell_build_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"\tabuild-meson \\\n",
                b"\tfalse && ( # disabled\n\tabuild-meson \\\n",
            ).replace(
                b"\tmeson compile -C output\n",
                b"\tmeson compile -C output\n\t) # disabled\n",
            )

        def heredoc_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = (
                b": <<'DECOY'\n" + aports[path] + b"DECOY\n"
            )

        def early_return_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"build() {\n",
                b"build() {\n\tif true; then return; fi\n",
                1,
            )

        def function_override_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] += b"\nfunction build { :; }\n"

        def eval_override_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] += b"\neval 'build() { :; }'\n"

        def unset_function_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] += b"\nunset -f build\n"

        def dynamic_source_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] += b"\n. ./override.sh\n"

        def source_override_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] += b"\nsource=\n"

        def version_override_drift(aports, upstream):
            path = "aports/main/pax-utils/APKBUILD"
            aports[path] = aports[path].replace(
                b"pkgver=1.3.9\n",
                b"pkgver=1.3.9\npkgver=9.9.9\n",
            )

        def false_target_drift(aports, upstream):
            path = SURFACE.PAX_ROOT + "/meson.build"
            upstream[path] = (
                b"if false\n" + upstream[path] + b"endif\n"
            )

        def disabled_target_drift(aports, upstream):
            path = SURFACE.PAX_ROOT + "/meson.build"
            upstream[path] = upstream[path].replace(
                b"  dependencies : [libcap],\n",
                b"  dependencies : [libcap],\n"
                b"  dependencies : disabler(),\n",
            )

        def patch_drift(aports, upstream):
            apkbuild = "aports/main/pax-utils/APKBUILD"
            aports[apkbuild] = aports[apkbuild].replace(
                b"pax-utils-$pkgver.tar.xz\n\t\"",
                b"pax-utils-$pkgver.tar.xz\n\ttouch.patch\n\t\"",
            )
            aports["aports/main/pax-utils/touch.patch"] = (
                b"--- a/elf.h\n+++ b/elf.h\n"
            )

        def duplicate_patch_drift(aports, upstream):
            apkbuild = "aports/main/pax-utils/APKBUILD"
            aports[apkbuild] = aports[apkbuild].replace(
                b"pax-utils-$pkgver.tar.xz\n\t\"",
                b"pax-utils-$pkgver.tar.xz\n"
                b"\ttouch.patch\n\ttouch.patch\n\t\"",
            )
            aports["aports/main/pax-utils/touch.patch"] = (
                b"--- a/README.md\n+++ b/README.md\n"
            )

        def noncanonical_patch_drift(aports, upstream):
            apkbuild = "aports/main/pax-utils/APKBUILD"
            aports[apkbuild] = aports[apkbuild].replace(
                b"pax-utils-$pkgver.tar.xz\n\t\"",
                b"pax-utils-$pkgver.tar.xz\n\ttouch.patch\n\t\"",
            )
            aports["aports/main/pax-utils/touch.patch"] = (
                b"--- a/./elf.h\n+++ b/./elf.h\n"
            )

        def global_build_patch_drift(aports, upstream):
            apkbuild = "aports/main/pax-utils/APKBUILD"
            aports[apkbuild] = aports[apkbuild].replace(
                b"pax-utils-$pkgver.tar.xz\n\t\"",
                b"pax-utils-$pkgver.tar.xz\n\ttouch.patch\n\t\"",
            )
            aports["aports/main/pax-utils/touch.patch"] = (
                b"--- a/meson_options.txt\n+++ b/meson_options.txt\n"
            )

        def quoted_patch_drift(aports, upstream):
            apkbuild = "aports/main/pax-utils/APKBUILD"
            aports[apkbuild] = aports[apkbuild].replace(
                b"pax-utils-$pkgver.tar.xz\n\t\"",
                b"pax-utils-$pkgver.tar.xz\n\ttouch.patch\n\t\"",
            )
            aports["aports/main/pax-utils/touch.patch"] = (
                b'diff --git "a/elf.h" "b/elf.h"\n'
                b'--- "a/elf.h"\n+++ "b/elf.h"\n'
            )

        cases = (
            ("Meson target", target_drift, "Meson scanelf target"),
            ("include", include_drift, "不再消费本地 elf.h"),
            (
                "条件禁用 include",
                conditional_include_drift,
                "不再消费本地 elf.h",
            ),
            ("subpackage", subpackage_drift, "固定顶层字段"),
            ("elf.h 版本", license_drift, "LGPL-2.1-or-later 原文"),
            ("构建函数", build_drift, "Meson 构建"),
            (
                "注释构建命令",
                commented_build_drift,
                "动态 shell 构造",
            ),
            (
                "不执行的构建分组",
                grouped_build_drift,
                "不支持的 shell 控制结构",
            ),
            (
                "带注释的不执行构建分组",
                commented_grouped_build_drift,
                "不支持的 shell 控制结构",
            ),
            (
                "带注释的不执行构建子 shell",
                commented_subshell_build_drift,
                "不支持的 shell 控制结构",
            ),
            ("here-document", heredoc_drift, "here-document"),
            ("提前 return", early_return_drift, "提前终止命令"),
            (
                "function 覆盖",
                function_override_drift,
                "函数定义不唯一",
            ),
            (
                "eval 覆盖",
                eval_override_drift,
                "动态 shell 构造",
            ),
            (
                "unset 函数",
                unset_function_drift,
                "动态 shell 构造",
            ),
            (
                "动态 source",
                dynamic_source_drift,
                "动态 shell 构造",
            ),
            ("source 覆盖", source_override_drift, "source 清单"),
            ("版本覆盖", version_override_drift, "固定顶层字段"),
            ("Meson false target", false_target_drift, "Meson scanelf target"),
            (
                "Meson disabler",
                disabled_target_drift,
                "Meson scanelf target",
            ),
            ("patch", patch_drift, "patch 触及 scanelf LGPL 链"),
            ("重复 patch", duplicate_patch_drift, "source 清单含重复成员"),
            (
                "非规范 patch",
                noncanonical_patch_drift,
                "patch 成员路径不是规范相对路径",
            ),
            (
                "全局构建 patch",
                global_build_patch_drift,
                "patch 触及 scanelf LGPL 链",
            ),
            (
                "quoted patch",
                quoted_patch_drift,
                "patch 含不支持或不安全的成员路径",
            ),
        )
        for name, mutate, expected_error in cases:
            with self.subTest(name=name):
                aports, upstream = pax_fixture()
                mutate(aports, upstream)
                with self.assertRaisesRegex(
                    SURFACE.SurfaceError, expected_error
                ):
                    SURFACE.validate_pax_sources(aports, upstream)

    def test_elf_identity_fields(self):
        SURFACE.validate_elf(bytes(valid_elf()), "bin/fixture")
        cases = (
            ("class", 4, 1),
            ("data", 5, 2),
            ("ident version", 6, 0),
            ("type", 16, 2),
            ("machine", 18, 62),
            ("ELF version", 20, 2),
            ("header size", 52, 63),
            ("program header size", 54, 55),
            ("program header count", 56, 0),
        )
        for name, offset, value in cases:
            with self.subTest(name=name):
                data = valid_elf()
                if offset < 16:
                    data[offset] = value
                elif offset == 20:
                    struct.pack_into("<I", data, offset, value)
                else:
                    struct.pack_into("<H", data, offset, value)
                with self.assertRaises(SURFACE.SurfaceError):
                    SURFACE.validate_elf(bytes(data), "bin/fixture")
        program_cases = (
            ("program header offset", 32, "<Q", 4096),
            ("program type", 64, "<I", 0),
            ("segment file size", 96, "<Q", 2),
            ("segment memory size", 104, "<Q", 0),
        )
        for name, offset, encoding, value in program_cases:
            with self.subTest(name=name):
                data = valid_elf()
                struct.pack_into(encoding, data, offset, value)
                with self.assertRaises(SURFACE.SurfaceError):
                    SURFACE.validate_elf(bytes(data), "bin/fixture")
        with self.assertRaises(SURFACE.SurfaceError):
            SURFACE.validate_elf(bytes(valid_elf()[:20]), "bin/fixture")

    def test_payload_digest_paths_preserve_duplicates(self):
        record = [
            "F:bin",
            "R:busybox",
            "Z:Q1-first",
            "F:bin",
            "R:busybox",
            "Z:Q1-second",
            "F:usr/bin",
            "R:scanelf",
            "Z:Q1-third",
        ]
        self.assertEqual(
            SURFACE.payload_digests(record),
            {
                "bin/busybox": ["Q1-first", "Q1-second"],
                "usr/bin/scanelf": ["Q1-third"],
            },
        )
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "F/R 路径链非法"
        ):
            SURFACE.payload_digests(["R:orphan", "Z:Q1-bad"])
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "R/Z 摘要链非法"
        ):
            SURFACE.payload_digests(
                ["F:bin", "R:busybox", "Z:Q1-first", "Z:Q1-second"]
            )
        for invalid in (
            ["F:", "R:bin/busybox", "Z:Q1-bad"],
            ["F:bin", "R:../busybox", "Z:Q1-bad"],
            ["F:bin", "R:busybox", "Z:"],
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(SURFACE.SurfaceError):
                    SURFACE.payload_digests(invalid)

    def test_payload_binary_ownership_and_digest(self):
        binary = bytes(valid_elf())
        package = "busybox@1.37.0-r31"
        other_package = "scanelf@1.3.9-r1"
        payload = SURFACE.Payload("busybox", package, "bin/busybox")
        digest = "Q1" + base64.b64encode(
            hashlib.sha1(binary).digest()
        ).decode()
        owner_record = ["F:bin", "R:busybox", "Z:" + digest]
        SURFACE.validate_payload_binary(
            payload, binary, {package: owner_record}
        )

        cases = (
            (
                "错误 owner",
                {other_package: owner_record},
                "没有唯一拥有",
            ),
            (
                "重复 owner",
                {
                    package: owner_record,
                    other_package: owner_record,
                },
                "没有唯一拥有",
            ),
            (
                "错误摘要",
                {
                    package: [
                        "F:bin",
                        "R:busybox",
                        "Z:Q1"
                        + base64.b64encode(
                            hashlib.sha1(b"drift").digest()
                        ).decode(),
                    ]
                },
                "SHA-1 不匹配",
            ),
        )
        for name, records, expected_error in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    SURFACE.SurfaceError, expected_error
                ):
                    SURFACE.validate_payload_binary(
                        payload, binary, records
                    )

    def test_synthetic_rootfs_cli_and_shell_end_to_end(self):
        package_data = SURFACE.DEFAULT_PACKAGES.read_bytes()
        packages = SURFACE.LICENSE_TOOL.parse_packages(package_data)
        busybox = bytes(valid_elf())
        scanelf = bytes(valid_elf())
        payload_data = {
            "busybox@1.37.0-r31": ("bin", "busybox", busybox),
            "scanelf@1.3.9-r1": ("usr/bin", "scanelf", scanelf),
        }
        records = []
        for package in sorted(packages.values(), key=lambda item: item.name):
            lines = [
                "P:" + package.name,
                "V:" + package.version,
                "o:" + package.origin,
                "L:" + package.license,
                "c:" + package.commit,
                "A:aarch64",
            ]
            payload = payload_data.get(package.key)
            if payload is not None:
                directory, filename, binary = payload
                digest = "Q1" + base64.b64encode(
                    hashlib.sha1(binary).digest()
                ).decode()
                lines.extend(
                    ["F:" + directory, "R:" + filename, "Z:" + digest]
                )
            records.append("\n".join(lines).encode())
        installed = b"\n\n".join(records) + b"\n"
        archive_data = tar_bytes(
            {
                "bin/busybox": (busybox, 0o755),
                "lib/apk/db/installed": installed,
                "usr/bin/scanelf": (scanelf, 0o755),
            },
            mode="w:gz",
        )
        version = "3.24.1"
        archive_name = (
            "alpine-minirootfs-{}-aarch64.tar.gz".format(version)
        )
        source_url = (
            "https://dl-cdn.alpinelinux.org/alpine/v3.24/"
            "releases/aarch64/" + archive_name
        )
        def reference_for(data):
            reference = "\t".join(
                (
                    version,
                    archive_name,
                    str(len(data)),
                    hashlib.sha256(data).hexdigest(),
                    source_url,
                    str(len(installed)),
                    hashlib.sha256(installed).hexdigest(),
                    str(len(package_data)),
                    hashlib.sha256(package_data).hexdigest(),
                    str(len(packages)),
                    str(len({item.origin for item in packages.values()})),
                )
            )
            return SURFACE.REFERENCE_HEADER + "\n" + reference + "\n"

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / archive_name
            binary_reference = root / "binary-reference.tsv"
            archive.write_bytes(archive_data)
            binary_reference.write_text(
                reference_for(archive_data),
                encoding="utf-8",
            )

            def run_checks(archive_path, reference_path):
                direct = subprocess.run(
                    [
                        sys.executable,
                        "-B",
                        str(TOOL_PATH),
                        "validate-rootfs",
                        str(archive_path),
                        "--packages",
                        str(SURFACE.DEFAULT_PACKAGES),
                        "--payloads",
                        str(SURFACE.DEFAULT_PAYLOADS),
                        "--binary-reference",
                        str(reference_path),
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=30,
                )
                shell = subprocess.run(
                    [
                    str(ROOT / "tools/apple-aarch64-rootfs-packages.sh"),
                    str(archive_path),
                    str(SURFACE.DEFAULT_PACKAGES),
                    str(reference_path),
                    str(SURFACE.DEFAULT_PAYLOADS),
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=30,
                )
                return direct, shell

            direct, shell = run_checks(archive, binary_reference)
            self.assertEqual(
                direct.returncode,
                0,
                "合成 rootfs CLI 未通过：\n{}".format(direct.stderr),
            )
            self.assertEqual(
                shell.returncode,
                0,
                "合成 rootfs shell 接线未通过：\n{}".format(shell.stderr),
            )

            nonexecutable_data = tar_bytes(
                {
                    "bin/busybox": busybox,
                    "lib/apk/db/installed": installed,
                    "usr/bin/scanelf": scanelf,
                },
                mode="w:gz",
            )
            nonexecutable = root / "nonexecutable.tar.gz"
            nonexecutable_reference = root / "nonexecutable-reference.tsv"
            nonexecutable.write_bytes(nonexecutable_data)
            nonexecutable_reference.write_text(
                reference_for(nonexecutable_data),
                encoding="utf-8",
            )
            direct, shell = run_checks(
                nonexecutable,
                nonexecutable_reference,
            )
            self.assertNotEqual(
                direct.returncode,
                0,
                "合成 rootfs CLI 接受了不可执行 payload。",
            )
            self.assertNotEqual(
                shell.returncode,
                0,
                "rootfs shell 接线没有执行 LGPL payload 门禁。",
            )

    def test_archive_path_normalization(self):
        self.assertEqual(
            SURFACE.normalize_archive_path("././bin/busybox"),
            "bin/busybox",
        )
        self.assertEqual(
            SURFACE.normalize_archive_path("usr/bin/scanelf"),
            "usr/bin/scanelf",
        )
        for path in (
            "",
            "/bin/busybox",
            "bin/../busybox",
            "usr//bin/scanelf",
            r"usr\bin\scanelf",
        ):
            with self.subTest(path=path):
                self.assertIsNone(SURFACE.normalize_archive_path(path))

    def test_source_archive_rejects_alias_before_filtering(self):
        archive_data = io.BytesIO()
        with tarfile.open(fileobj=archive_data, mode="w") as archive:
            member = tarfile.TarInfo("./ignored")
            member.size = 1
            archive.addfile(member, io.BytesIO(b"x"))
        with self.assertRaisesRegex(
            SURFACE.SurfaceError, "不是安全的相对路径"
        ):
            SURFACE.archive_files(
                archive_data.getvalue(),
                lambda _name: False,
                "合成源码归档",
            )


if __name__ == "__main__":
    unittest.main()
