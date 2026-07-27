#!/usr/bin/env python3

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Optional


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent.parent
VALIDATOR = ROOT / "tools" / "apple-release-profiles.py"
FIXTURE_INPUTS = (
    "app/App.xcconfig",
    "app/iOS.xcconfig",
    "app/iSH.xcconfig",
    "app/Linux.xcconfig",
    "app/NotLinux.xcconfig",
    "app/Project.xcconfig",
    "app/ProjectDebug.xcconfig",
    "app/ProjectDebugLinux.xcconfig",
    "app/ProjectRelease.xcconfig",
    "app/ProjectReleaseLinux.xcconfig",
    "app/WatchApp.xcconfig",
    "app/WatchOS.xcconfig",
    "app/WatchUITests.xcconfig",
    "app/XcodeDebug.xcconfig",
    "app/XcodeDefault.xcconfig",
    "app/XcodeRelease.xcconfig",
    "app/xcode-meson.sh",
    "distribution/apple/release-profiles.tsv",
    "iSH.xcodeproj/project.pbxproj",
    "iSH.xcodeproj/xcshareddata/xcschemes/Screenshots.xcscheme",
    "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
    "iSH.xcodeproj/xcshareddata/xcschemes/iSH.xcscheme",
    "iSH.xcodeproj/xcshareddata/xcschemes/iSHWatch.xcscheme",
    "tools/apple-aarch64-rootfs-inputs.xcfilelist",
    "tools/apple-watch-rootfs-phase.sh",
)
HEADER = (
    "profile\trole\ttarget\tscheme\tdebug_configuration"
    "\trelease_configuration\tkernel\trootfs\n"
)
EXPECTED_SHOW = {
    "core": HEADER
    + (
        "core\tiphone-app\tiSH\tiSH\t"
        "Debug-ApplePleaseFixFB19282108\tRelease\tish\t"
        "fixed-alpine-seed\n"
        "core\twatch-app\tiSHWatch\tiSHWatch\t"
        "Debug-ApplePleaseFixFB19282108\tRelease\tish\t"
        "fixed-alpine-seed\n"
    ),
    "with-linux": HEADER
    + (
        "with-linux\tiphone-app\tiSH+Linux\tiSH+Linux\t"
        "DebugLinux\tReleaseLinux\tlinux\tonline-root-tar\n"
        "with-linux\twatch-app\tiSHWatch\tiSHWatch\t"
        "Debug-ApplePleaseFixFB19282108\tRelease\tish\t"
        "fixed-alpine-seed\n"
    ),
}


class TestFailure(Exception):
    pass


def fail(message: str) -> None:
    raise TestFailure(message)


def run_validator(
    root: Path,
    arguments: list[str],
    expect_success: bool = True,
    expected_error: Optional[str] = None,
    validator: Path = VALIDATOR,
) -> str:
    command = [
        sys.executable,
        "-B",
        str(validator),
        *arguments,
        "--root",
        str(root),
    ]
    result = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={"PATH": str(Path(sys.executable).parent) + ":/usr/bin:/bin"},
    )
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    output = stdout + stderr
    if expect_success:
        if result.returncode != 0:
            fail(f"命令意外失败：{' '.join(command)}\n{output}")
    else:
        if result.returncode == 0:
            fail(f"负例意外成功：{' '.join(command)}")
        if "错误：" not in output or "Traceback" in output:
            fail(f"负例没有返回简洁中文错误：\n{output}")
        if expected_error and expected_error not in output:
            fail(f"负例没有命中“{expected_error}”：\n{output}")
    return stdout


def copy_fixture(destination: Path) -> None:
    destination.mkdir()
    for relative in FIXTURE_INPUTS:
        source = ROOT / relative
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target, follow_symlinks=False)


def fixture_digest(root: Path) -> str:
    digest = hashlib.sha256()
    paths = [root, *root.rglob("*")]
    for path in sorted(paths, key=lambda item: str(item.relative_to(root))):
        relative = str(path.relative_to(root)) or "."
        metadata = path.lstat()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(stat.S_IFMT(metadata.st_mode)).encode("ascii"))
        digest.update(b":")
        digest.update(str(stat.S_IMODE(metadata.st_mode)).encode("ascii"))
        digest.update(b"\0")
        if stat.S_ISREG(metadata.st_mode):
            digest.update(path.read_bytes())
        elif stat.S_ISLNK(metadata.st_mode):
            digest.update(str(path.readlink()).encode("utf-8"))
        elif not stat.S_ISDIR(metadata.st_mode):
            fail(f"fixture 含不支持的文件类型：{relative}")
        digest.update(b"\0")
    return digest.hexdigest()


def write_utf8(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(content)


def replace_once(root: Path, relative: str, old: str, new: str) -> None:
    path = root / relative
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        fail(f"{relative} 没有唯一待替换文本：{old}")
    write_utf8(path, text.replace(old, new, 1))


def replace_in_pbx_object(
    root: Path,
    identifier: str,
    old: str,
    new: str,
) -> None:
    path = root / "iSH.xcodeproj/project.pbxproj"
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\t\t{re.escape(identifier)}(?: /\*.*\*/)? = \{{",
        text,
        re.MULTILINE,
    )
    if match is None:
        fail(f"Xcode 工程缺少测试对象：{identifier}")
    start = match.start()
    end = text.find("\n\t\t};", start)
    if end < 0:
        fail(f"Xcode 测试对象没有闭合：{identifier}")
    body = text[start:end]
    if body.count(old) != 1:
        fail(f"Xcode 测试对象没有唯一待替换文本：{identifier}")
    write_utf8(path, text[:start] + body.replace(old, new, 1) + text[end:])


def case_manifest_row_missing(root: Path) -> None:
    replace_once(
        root,
        "distribution/apple/release-profiles.tsv",
        (
            "with-linux\twatch-app\tiSHWatch\tiSHWatch\t"
            "Debug-ApplePleaseFixFB19282108\tRelease\tish\t"
            "fixed-alpine-seed\n"
        ),
        "",
    )


def case_manifest_extra_role(root: Path) -> None:
    path = root / "distribution/apple/release-profiles.tsv"
    text = path.read_text(encoding="utf-8")
    write_utf8(
        path,
        text
        + (
            "with-linux\tstatic-library\tlibish\tlibish\tDebug\t"
            "Release\tish\tfixed-alpine-seed\n"
        ),
    )


def case_scheme_archive_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
        (
            '<ArchiveAction\n'
            '      buildConfiguration = "ReleaseLinux"'
        ),
        '<ArchiveAction\n      buildConfiguration = "Release"',
    )


def case_scheme_archive_disabled(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
        'buildForArchiving = "YES"',
        'buildForArchiving = "NO"',
    )


def case_scheme_testable_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
        'BlueprintIdentifier = "BB41591B255EF9E300E0950C"',
        'BlueprintIdentifier = "BBEF1964268066D1001225BD"',
    )


def case_scheme_selected_tests(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH.xcscheme",
        "            <SkippedTests>\n",
        (
            "            <SelectedTests>\n"
            "               <Test\n"
            '                  Identifier = "UITests/testAArch64基础网络与软件源">\n'
            "               </Test>\n"
            "            </SelectedTests>\n"
            "            <SkippedTests>\n"
        ),
    )


def case_scheme_skipped_tests_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
        "            <SkippedTests>\n",
        (
            "            <SkippedTests>\n"
            "               <Test\n"
            '                  Identifier = "UITests">\n'
            "               </Test>\n"
        ),
    )


def case_watch_empty_skipped_tests(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/iSHWatch.xcscheme",
        (
            '               ReferencedContainer = "container:iSH.xcodeproj">\n'
            "            </BuildableReference>\n"
            "         </TestableReference>"
        ),
        (
            '               ReferencedContainer = "container:iSH.xcodeproj">\n'
            "            </BuildableReference>\n"
            "            <SkippedTests>\n"
            "            </SkippedTests>\n"
            "         </TestableReference>"
        ),
    )


def case_screenshots_host_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/Screenshots.xcscheme",
        'BlueprintIdentifier = "BB792B4F1F96D90D00FFB7A4"',
        'BlueprintIdentifier = "BBEF1964268066D1001225BD"',
    )


def case_screenshots_configuration_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/xcshareddata/xcschemes/Screenshots.xcscheme",
        (
            '<LaunchAction\n'
            '      buildConfiguration = "Debug-ApplePleaseFixFB19282108"'
        ),
        '<LaunchAction\n      buildConfiguration = "Debug"',
    )


def case_iphone_test_host_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB7F620A2688EAB5003C0220",
        'TEST_TARGET_NAME = "iSH+Linux";',
        "TEST_TARGET_NAME = iSH;",
    )


def case_watch_test_host_override(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "A1E000000000000000000007",
        "buildSettings = {\n\t\t\t};",
        (
            "buildSettings = {\n"
            "\t\t\t\tTEST_TARGET_NAME = iSH;\n"
            "\t\t\t};"
        ),
    )


def case_iphone_test_static_dependency(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB41591B255EF9E300E0950C",
        "\t\t\tdependencies = (\n\t\t\t);",
        (
            "\t\t\tdependencies = (\n"
            "\t\t\t\tBB4A922824ED940C002F5A96 "
            "/* PBXTargetDependency */,\n"
            "\t\t\t);"
        ),
    )


def case_recursive_xcconfig_missing(root: Path) -> None:
    replace_once(
        root,
        "app/WatchApp.xcconfig",
        '#include "WatchOS.xcconfig"',
        '#include "MissingWatchOS.xcconfig"',
    )


def case_ordinary_kernel_missing(root: Path) -> None:
    replace_once(
        root,
        "app/NotLinux.xcconfig",
        "ISH_KERNEL = ish\n",
        "",
    )


def case_linux_ninja_drift(root: Path) -> None:
    replace_once(
        root,
        "app/Linux.xcconfig",
        "NINJA_TARGETS = deps/liblinux.a libfakefs.a libish_emu.a",
        "NINJA_TARGETS = libish.a libfakefs.a libish_emu.a",
    )


def case_meson_implicit_kernel(root: Path) -> None:
    replace_once(
        root,
        "app/xcode-meson.sh",
        "kernel=$ISH_KERNEL",
        "kernel=ish",
    )


def case_meson_kernel_call_commented(root: Path) -> None:
    replace_once(
        root,
        "app/xcode-meson.sh",
        "kernel=$ISH_KERNEL\n",
        '# kernel=$ISH_KERNEL\nkernel="${ISH_KERNEL:-ish}"\n',
    )


def case_core_gate_missing(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            "check-build --profile core --role iphone-app "
            '--target \\"$TARGET_NAME\\"'
        ),
        (
            "check-build-disabled --profile core --role iphone-app "
            '--target \\"$TARGET_NAME\\"'
        ),
    )


def case_linux_gate_missing(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            "check-build --profile with-linux --role iphone-app "
            '--target \\"$TARGET_NAME\\"'
        ),
        (
            "check-build-disabled --profile with-linux --role iphone-app "
            '--target \\"$TARGET_NAME\\"'
        ),
    )


def case_linux_fail_fast_missing(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF200000000000000000002",
        'shellScript = "set -eu\\npython3 -B',
        'shellScript = "python3 -B',
    )


def case_core_gate_suppressed(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF200000000000000000001",
        "--rootfs fixed-alpine-seed\\n\";",
        "--rootfs fixed-alpine-seed || true\\n\";",
    )


def case_watch_gate_missing(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        "check-build --profile core --role watch-app",
        "check-build-disabled --profile core --role watch-app",
    )


def case_gate_input_missing(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF200000000000000000001",
        (
            '\t\t\t\t"$(SRCROOT)/tools/'
            'apple-release-profiles.py",\n'
        ),
        "",
    )


def case_gate_incremental_skip(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF200000000000000000001",
        "\t\t\talwaysOutOfDate = 1;\n",
        "",
    )


def case_gate_deployment_only(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF200000000000000000001",
        "\t\t\trunOnlyForDeploymentPostprocessing = 0;\n",
        "\t\t\trunOnlyForDeploymentPostprocessing = 1;\n",
    )


def case_gate_phase_order(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            "\t\t\tbuildPhases = (\n"
            "\t\t\t\tBBF200000000000000000001 "
            "/* 校验发行候选 Profile */,\n"
            "\t\t\t\tBB21A17826890A5B00BD19B4 /* Sources */,\n"
        ),
        (
            "\t\t\tbuildPhases = (\n"
            "\t\t\t\tBB21A17826890A5B00BD19B4 /* Sources */,\n"
            "\t\t\t\tBBF200000000000000000001 "
            "/* 校验发行候选 Profile */,\n"
        ),
    )


def case_project_configuration_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBEF19AB26806D1F001225BD",
        (
            "BB28C7522689522700BDC834 "
            "/* ProjectReleaseLinux.xcconfig */"
        ),
        "BB0B85A52586F1E100208600 /* ProjectRelease.xcconfig */",
    )


def case_target_configuration_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB792B651F96D90D00FFB7A4",
        "BB0B86CD2586FD8800208600 /* App.xcconfig */",
        "A1D00000000000000000000D /* WatchApp.xcconfig */",
    )


def case_core_rootfs_input_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF1248B1FA7BF530088FB50",
        (
            '\t\t\t\t"$(SRCROOT)/tools/'
            'apple-aarch64-rootfs-inputs.xcfilelist",\n'
        ),
        "",
    )


def case_watch_wrapper_input_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "A1D000000000000000000019",
        (
            '\t\t\t\t"$(SRCROOT)/tools/'
            'apple-watch-rootfs-phase.sh",\n'
        ),
        "",
    )


def case_core_rootfs_fail_fast_missing(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BBF1248B1FA7BF530088FB50",
        'shellScript = "set -euo pipefail\\nresources=',
        'shellScript = "resources=',
    )


def case_core_rootfs_call_commented(root: Path) -> None:
    command = (
        '\\"$SRCROOT/tools/apple-aarch64-rootfs.sh\\" '
        '\\"$seed\\"\\n'
    )
    replace_in_pbx_object(
        root,
        "BBF1248B1FA7BF530088FB50",
        command,
        f"# {command}true\\n",
    )


def case_linux_rootfs_call_commented(root: Path) -> None:
    command = (
        'curl -L \\"https://$ROOTFS_URL\\" -o '
        '\\"$BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/root.tar.gz\\"\\n'
    )
    replace_in_pbx_object(
        root,
        "BBEF1979268066D1001225BD",
        command,
        f"# {command}true\\n",
    )


def case_watch_phase_call_commented(root: Path) -> None:
    command = (
        '/bin/bash \\"$SRCROOT/tools/apple-watch-rootfs-phase.sh\\"\\n'
    )
    replace_in_pbx_object(
        root,
        "A1D000000000000000000019",
        command,
        f"# {command}true\\n",
    )


def case_watch_rootfs_fail_fast_missing(root: Path) -> None:
    replace_once(
        root,
        "tools/apple-watch-rootfs-phase.sh",
        "set -euo pipefail\n",
        "",
    )


def case_watch_rootfs_call_commented(root: Path) -> None:
    command = '"$SRCROOT/tools/apple-aarch64-rootfs.sh" "$seed"\n'
    replace_once(
        root,
        "tools/apple-watch-rootfs-phase.sh",
        command,
        f"# {command}true\n",
    )


def case_product_name_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            '\t\t\tname = "iSH+Linux";\n'
            "\t\t\tproductName = iSH;"
        ),
        (
            '\t\t\tname = "iSH+Linux";\n'
            "\t\t\tproductName = iSHLinux;"
        ),
    )


def case_file_provider_owner_missing(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            "\t\t\t\tBBEF1997268066D1001225BD "
            "/* Embed Foundation Extensions */,\n"
        ),
        "",
    )


def case_file_provider_duplicate_embedding(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB88F4A32154760800A341FD",
        (
            "\t\t\t\tBB88F49F2154760800A341FD "
            "/* iSHFileProvider.appex in Embed Foundation Extensions */,\n"
        ),
        (
            "\t\t\t\tBB88F49F2154760800A341FD "
            "/* iSHFileProvider.appex in Embed Foundation Extensions */,\n"
            "\t\t\t\tBB88F49F2154760800A341FD "
            "/* iSHFileProvider.appex in Embed Foundation Extensions */,\n"
        ),
    )


def case_file_provider_destination_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB88F4A32154760800A341FD",
        "dstSubfolderSpec = 13;",
        "dstSubfolderSpec = 7;",
    )


def case_file_provider_execution_drift(root: Path) -> None:
    replace_in_pbx_object(
        root,
        "BB88F4A32154760800A341FD",
        "buildActionMask = 2147483647;",
        "buildActionMask = 8;",
    )


def case_file_provider_attributes_drift(root: Path) -> None:
    replace_once(
        root,
        "iSH.xcodeproj/project.pbxproj",
        (
            "BB88F49F2154760800A341FD "
            "/* iSHFileProvider.appex in Embed Foundation Extensions */ = "
            "{isa = PBXBuildFile; fileRef = BB88F4902154760800A341FD "
            "/* iSHFileProvider.appex */; settings = {ATTRIBUTES = "
            "(RemoveHeadersOnCopy, ); }; };"
        ),
        (
            "BB88F49F2154760800A341FD "
            "/* iSHFileProvider.appex in Embed Foundation Extensions */ = "
            "{isa = PBXBuildFile; fileRef = BB88F4902154760800A341FD "
            "/* iSHFileProvider.appex */; settings = {ATTRIBUTES = "
            "(CodeSignOnCopy, ); }; };"
        ),
    )


def case_cross_section_identifier_collision(root: Path) -> None:
    path = root / "iSH.xcodeproj/project.pbxproj"
    text = path.read_text(encoding="utf-8")
    identifier = "D7E4C6A0F1284B3E9A5C7012"
    if text.count(identifier) != 2:
        fail("Watch profile 门禁对象引用数量漂移")
    write_utf8(
        path,
        text.replace(identifier, "A1F000000000000000000001"),
    )


def check_build_matrix() -> None:
    positive = (
        (
            "core",
            "iphone-app",
            "iSH",
            "Debug-ApplePleaseFixFB19282108",
            "ish",
            "fixed-alpine-seed",
        ),
        (
            "core",
            "iphone-app",
            "iSH",
            "Release",
            "ish",
            "fixed-alpine-seed",
        ),
        (
            "core",
            "watch-app",
            "iSHWatch",
            "Debug-ApplePleaseFixFB19282108",
            "ish",
            "fixed-alpine-seed",
        ),
        (
            "core",
            "watch-app",
            "iSHWatch",
            "Release",
            "ish",
            "fixed-alpine-seed",
        ),
        (
            "with-linux",
            "iphone-app",
            "iSH+Linux",
            "DebugLinux",
            "linux",
            "online-root-tar",
        ),
        (
            "with-linux",
            "iphone-app",
            "iSH+Linux",
            "ReleaseLinux",
            "linux",
            "online-root-tar",
        ),
        (
            "with-linux",
            "watch-app",
            "iSHWatch",
            "Release",
            "ish",
            "fixed-alpine-seed",
        ),
    )
    for profile, role, target, configuration, kernel, rootfs in positive:
        run_validator(
            ROOT,
            [
                "check-build",
                "--profile",
                profile,
                "--role",
                role,
                "--target",
                target,
                "--configuration",
                configuration,
                "--kernel",
                kernel,
                "--rootfs",
                rootfs,
            ],
        )

    negative = (
        ("core", "iphone-app", "iSH", "ReleaseLinux", "ish", "fixed-alpine-seed"),
        (
            "with-linux",
            "iphone-app",
            "iSH+Linux",
            "Release",
            "linux",
            "online-root-tar",
        ),
        (
            "core",
            "watch-app",
            "iSHWatch",
            "ReleaseLinux",
            "ish",
            "fixed-alpine-seed",
        ),
        ("core", "iphone-app", "iSH", "Release", "linux", "fixed-alpine-seed"),
        ("core", "iphone-app", "iSH", "Release", "ish", "online-root-tar"),
        (
            "core",
            "iphone-app",
            "iSH+Linux",
            "Release",
            "ish",
            "fixed-alpine-seed",
        ),
    )
    for profile, role, target, configuration, kernel, rootfs in negative:
        run_validator(
            ROOT,
            [
                "check-build",
                "--profile",
                profile,
                "--role",
                role,
                "--target",
                target,
                "--configuration",
                configuration,
                "--kernel",
                kernel,
                "--rootfs",
                rootfs,
            ],
            expect_success=False,
            expected_error="与发行候选 profile 不一致",
        )

    for profile, role in (
        ("unknown", "iphone-app"),
        ("core", "unknown"),
    ):
        run_validator(
            ROOT,
            [
                "check-build",
                "--profile",
                profile,
                "--role",
                role,
                "--target",
                "iSH",
                "--configuration",
                "Release",
                "--kernel",
                "ish",
                "--rootfs",
                "fixed-alpine-seed",
            ],
            expect_success=False,
            expected_error="没有匹配唯一发行候选 profile 角色",
        )


def main() -> None:
    run_validator(ROOT, ["check-locks"])
    for profile, expected in EXPECTED_SHOW.items():
        output = run_validator(ROOT, ["show", "--profile", profile])
        if output != expected:
            fail(f"{profile} 的 show 输出漂移")
    run_validator(
        ROOT,
        ["show", "--profile", "unknown"],
        expect_success=False,
        expected_error="未知 Apple 发行候选 profile",
    )
    run_validator(
        ROOT,
        ["show"],
        expect_success=False,
        expected_error="命令参数不完整或非法",
    )
    check_build_matrix()

    cases = {
        "manifest-row-missing": (
            case_manifest_row_missing,
            "发行候选 profile 合同漂移",
        ),
        "manifest-extra-role": (
            case_manifest_extra_role,
            "发行候选 profile 清单字段非法",
        ),
        "scheme-archive-drift": (
            case_scheme_archive_drift,
            "ArchiveAction 的 configuration 漂移",
        ),
        "scheme-archive-disabled": (
            case_scheme_archive_disabled,
            "BuildAction 执行范围漂移",
        ),
        "scheme-testable-drift": (
            case_scheme_testable_drift,
            "TestAction 测试引用的产品引用漂移",
        ),
        "scheme-selected-tests": (
            case_scheme_selected_tests,
            "不能缩小发行候选测试集合",
        ),
        "scheme-skipped-tests-drift": (
            case_scheme_skipped_tests_drift,
            "TestAction 的跳过测试集合漂移",
        ),
        "watch-empty-skipped-tests": (
            case_watch_empty_skipped_tests,
            "TestAction 的跳过测试集合漂移",
        ),
        "screenshots-host-drift": (
            case_screenshots_host_drift,
            "Screenshots BuildAction 产品集合漂移",
        ),
        "screenshots-configuration-drift": (
            case_screenshots_configuration_drift,
            "Screenshots LaunchAction 的 configuration 漂移",
        ),
        "iphone-test-host-drift": (
            case_iphone_test_host_drift,
            "configuration 与被测 App 映射漂移",
        ),
        "watch-test-host-override": (
            case_watch_test_host_override,
            "不能覆盖 xcconfig 的被测 App",
        ),
        "iphone-test-static-dependency": (
            case_iphone_test_static_dependency,
            "不能静态依赖某一个互斥 iPhone App",
        ),
        "recursive-xcconfig-missing": (
            case_recursive_xcconfig_missing,
            "xcconfig不存在",
        ),
        "ordinary-kernel-missing": (
            case_ordinary_kernel_missing,
            "缺少唯一 ISH_KERNEL",
        ),
        "linux-ninja-drift": (
            case_linux_ninja_drift,
            "Linux 产品配置的 Ninja 输入漂移",
        ),
        "meson-implicit-kernel": (
            case_meson_implicit_kernel,
            "仍允许隐式内核选择",
        ),
        "meson-kernel-call-commented": (
            case_meson_kernel_call_commented,
            "仍允许隐式内核选择",
        ),
        "core-gate-missing": (
            case_core_gate_missing,
            "iSH 的发行候选 profile 门禁脚本漂移",
        ),
        "linux-gate-missing": (
            case_linux_gate_missing,
            "iSH+Linux 的发行候选 profile 门禁脚本漂移",
        ),
        "linux-fail-fast-missing": (
            case_linux_fail_fast_missing,
            "iSH+Linux 的发行候选 profile 门禁脚本漂移",
        ),
        "core-gate-suppressed": (
            case_core_gate_suppressed,
            "iSH 的发行候选 profile 门禁脚本漂移",
        ),
        "watch-gate-missing": (
            case_watch_gate_missing,
            "iSHWatch 的发行候选 profile 门禁脚本漂移",
        ),
        "gate-input-missing": (
            case_gate_input_missing,
            "iSH 的发行候选 profile 门禁执行合同漂移",
        ),
        "gate-incremental-skip": (
            case_gate_incremental_skip,
            "iSH 的发行候选 profile 门禁执行合同漂移",
        ),
        "gate-deployment-only": (
            case_gate_deployment_only,
            "iSH 的发行候选 profile 门禁执行合同漂移",
        ),
        "gate-phase-order": (
            case_gate_phase_order,
            "iSH 的发行候选 profile 门禁不是首个 App phase",
        ),
        "project-configuration-drift": (
            case_project_configuration_drift,
            "Xcode project configuration 与产品 xcconfig 映射漂移",
        ),
        "target-configuration-drift": (
            case_target_configuration_drift,
            "iSH target configuration 映射漂移",
        ),
        "core-rootfs-input-drift": (
            case_core_rootfs_input_drift,
            "普通 iSH rootfs phase 输入清单漂移",
        ),
        "watch-wrapper-input-drift": (
            case_watch_wrapper_input_drift,
            "Watch rootfs phase 包装脚本输入漂移",
        ),
        "core-rootfs-fail-fast-missing": (
            case_core_rootfs_fail_fast_missing,
            "普通 iSH rootfs phase 缺少可执行命令",
        ),
        "core-rootfs-call-commented": (
            case_core_rootfs_call_commented,
            "普通 iSH rootfs phase 缺少可执行命令",
        ),
        "linux-rootfs-call-commented": (
            case_linux_rootfs_call_commented,
            "iSH+Linux rootfs phase 缺少可执行命令",
        ),
        "watch-phase-call-commented": (
            case_watch_phase_call_commented,
            "Watch rootfs phase 缺少可执行命令",
        ),
        "watch-rootfs-fail-fast-missing": (
            case_watch_rootfs_fail_fast_missing,
            "Watch rootfs phase 包装脚本 缺少可执行命令",
        ),
        "watch-rootfs-call-commented": (
            case_watch_rootfs_call_commented,
            "Watch rootfs phase 包装脚本 缺少可执行命令",
        ),
        "product-name-drift": (
            case_product_name_drift,
            "ProductName 漂移",
        ),
        "file-provider-owner-missing": (
            case_file_provider_owner_missing,
            "FileProvider Copy Files phase 归属漂移",
        ),
        "file-provider-duplicate-embedding": (
            case_file_provider_duplicate_embedding,
            "FileProvider 没有被两个互斥 iPhone target 各嵌入一次",
        ),
        "file-provider-destination-drift": (
            case_file_provider_destination_drift,
            "iSH 的 FileProvider Copy Files phase 漂移",
        ),
        "file-provider-execution-drift": (
            case_file_provider_execution_drift,
            "iSH 的 FileProvider Copy Files phase 漂移",
        ),
        "file-provider-attributes-drift": (
            case_file_provider_attributes_drift,
            "iSH 的 FileProvider 嵌入属性漂移",
        ),
        "cross-section-identifier-collision": (
            case_cross_section_identifier_collision,
            "Xcode 工程对象 ID 跨 section 重复",
        ),
    }
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-release-profiles."
    ) as temporary:
        temporary_root = Path(temporary)
        minimal = temporary_root / "minimal-build-check"
        manifest = minimal / "distribution/apple/release-profiles.tsv"
        manifest.parent.mkdir(parents=True)
        shutil.copy2(
            ROOT / "distribution/apple/release-profiles.tsv",
            manifest,
            follow_symlinks=False,
        )
        isolated_validator = (
            minimal / "tools/apple-release-profiles.py"
        )
        isolated_validator.parent.mkdir()
        shutil.copy2(
            VALIDATOR,
            isolated_validator,
            follow_symlinks=False,
        )
        minimal_arguments = [
            "check-build",
            "--profile",
            "core",
            "--role",
            "iphone-app",
            "--target",
            "iSH",
            "--configuration",
            "Release",
            "--kernel",
            "ish",
            "--rootfs",
            "fixed-alpine-seed",
        ]
        run_validator(
            minimal,
            minimal_arguments,
            validator=isolated_validator,
        )
        root_loop = temporary_root / "root-loop"
        root_loop.symlink_to(root_loop.name)
        run_validator(
            root_loop,
            minimal_arguments,
            expect_success=False,
            expected_error="待校验仓库根目录无法解析",
        )
        base = temporary_root / "base"
        copy_fixture(base)
        before = fixture_digest(base)
        run_validator(base, ["check-locks"])
        after = fixture_digest(base)
        if before != after:
            fail("profile 校验器改写了输入工作树")
        unexpected_probe = temporary_root / "unexpected-file-probe"
        shutil.copytree(base, unexpected_probe, symlinks=True)
        probe_digest = fixture_digest(unexpected_probe)
        write_utf8(unexpected_probe / "unexpected.txt", "unexpected\n")
        if fixture_digest(unexpected_probe) == probe_digest:
            fail("fixture digest 没有覆盖意外新增文件")
        mode_probe = temporary_root / "mode-probe"
        shutil.copytree(base, mode_probe, symlinks=True)
        probe_digest = fixture_digest(mode_probe)
        mode_path = mode_probe / "distribution/apple/release-profiles.tsv"
        current_mode = stat.S_IMODE(mode_path.stat().st_mode)
        mode_path.chmod(current_mode ^ stat.S_IXUSR)
        if fixture_digest(mode_probe) == probe_digest:
            fail("fixture digest 没有覆盖文件模式")
        for name, (mutate, expected_error) in cases.items():
            fixture = temporary_root / name
            shutil.copytree(base, fixture, symlinks=True)
            mutate(fixture)
            run_validator(
                fixture,
                ["check-locks"],
                expect_success=False,
                expected_error=expected_error,
            )
    print("Apple 发行候选 profile 合成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (TestFailure, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
