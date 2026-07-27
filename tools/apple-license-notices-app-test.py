#!/usr/bin/env python3

import hashlib
import json
import re
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

from apple_pbx import (
    objects,
    owners_for_reference,
    phase_owners,
    property_value,
    resolve_file_references,
)
from apple_host_manifest import (
    APPLE_HOST_RAW_INPUT_PATHS,
)


ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / "iSH.xcodeproj/project.pbxproj"
ALPINE_NOTICES_RELATIVE = PurePosixPath(
    "third_party/alpine/3.24.1-aarch64/THIRD-PARTY-NOTICES.txt"
)
HOST_NOTICES_RELATIVE = PurePosixPath(
    "third_party/apple-host/APPLE-HOST-NOTICES.txt"
)
PROJECT_NOTICES_RELATIVE = PurePosixPath(
    "distribution/apple/project-license/PROJECT-LICENSES.txt"
)
PROJECT_RAW_INPUTS = (
    PurePosixPath("LICENSE.md"),
    PurePosixPath("LICENSE.IOS"),
    PurePosixPath("distribution/apple/project-license/inputs.tsv"),
    PurePosixPath(
        "distribution/apple/project-license/license-inputs/GPL-2.0.txt"
    ),
    PurePosixPath(
        "distribution/apple/project-license/license-inputs/GPL-3.0.txt"
    ),
)
HOST_RAW_INPUTS = tuple(
    PurePosixPath(relative)
    for relative in APPLE_HOST_RAW_INPUT_PATHS
)
RAW_RESOURCE_INPUTS = PROJECT_RAW_INPUTS + HOST_RAW_INPUTS
RESOURCE_CONTRACTS = {
    ALPINE_NOTICES_RELATIVE: {
        "owners": {"iSH", "iSHWatch"},
        "excluded": {
            "iSH+Linux",
            "iSHFileProvider",
            "iSHUITests",
            "iSHWatchUITests",
            "iSHWatchLinkSmoke",
        },
    },
    HOST_NOTICES_RELATIVE: {
        "owners": {"iSH", "iSH+Linux"},
        "excluded": {
            "iSHWatch",
            "iSHFileProvider",
            "iSHUITests",
            "iSHWatchUITests",
            "iSHWatchLinkSmoke",
        },
    },
    PROJECT_NOTICES_RELATIVE: {
        "owners": {"iSH", "iSH+Linux", "iSHWatch"},
        "excluded": {
            "iSHFileProvider",
            "iSHUITests",
            "iSHWatchUITests",
            "iSHWatchLinkSmoke",
        },
    },
}


def fail(message):
    raise ValueError(message)


def read_text(relative):
    path = ROOT / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        fail(f"无法读取 UTF-8 文件 {relative}：{error}")


def run_git(*arguments):
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        fail(f"Git 检查失败：{detail or '未知错误'}")
    return result.stdout


def require_unique_reference(resolved, relative):
    references = resolved.get(relative, [])
    if len(references) != 1:
        fail(f"Xcode 工程必须唯一引用 {relative}")
    return references[0]


def verify_notices_file(relative, tracked):
    notices = ROOT / relative
    try:
        metadata = notices.lstat()
    except OSError as error:
        fail(f"找不到声明正文 {relative}：{error}")
    if not stat.S_ISREG(metadata.st_mode):
        fail(
            f"声明正文 {relative} 必须是常规文件，"
            "不能是目录、链接或特殊文件"
        )

    stage = run_git("ls-files", "--stage", "--", str(relative))
    stage_lines = stage.decode("utf-8", errors="strict").splitlines()
    expected_suffix = "\t" + str(relative)
    if (
        len(stage_lines) != 1
        or not stage_lines[0].startswith("100644 ")
        or not stage_lines[0].endswith(expected_suffix)
    ):
        fail(f"声明正文 {relative} 必须以唯一的 Git 100644 文件受跟踪")
    same_name = [
        path
        for path in tracked
        if path and PurePosixPath(path).name == relative.name
    ]
    if same_name != [str(relative)]:
        fail(f"仓库中只能跟踪一份 {relative.name}")


def verify_project_resources(project, build_files, resolved):
    _, _, target_names = phase_owners(
        project, "PBXResourcesBuildPhase"
    )
    for relative, contract in RESOURCE_CONTRACTS.items():
        reference = require_unique_reference(resolved, str(relative))
        build_ids, used_ids, owners = owners_for_reference(
            project, reference, build_files, "PBXResourcesBuildPhase"
        )
        if (
            len(build_ids) != len(contract["owners"])
            or build_ids != used_ids
        ):
            fail(
                f"{relative.name} 的 Resources build file 数量"
                "必须与 owner 数量一致"
            )
        if owners != contract["owners"]:
            fail(
                f"{relative.name} 的 Resources owners 漂移，实际为："
                + ", ".join(sorted(owners))
            )
        missing = contract["excluded"] - set(target_names)
        if missing:
            fail(
                f"{relative.name} 缺少必须验证的排除 target："
                + ", ".join(sorted(missing))
            )
        for build_id in build_ids:
            if len(re.findall(rf"\b{build_id}\b", project)) != 2:
                fail(f"资源成员 {build_id} 只能出现在定义和目标阶段各一次")

    for relative in RAW_RESOURCE_INPUTS:
        for reference in resolved.get(str(relative), []):
            build_ids, used_ids, owners = owners_for_reference(
                project,
                reference,
                build_files,
                "PBXResourcesBuildPhase",
            )
            if build_ids or used_ids or owners:
                fail(f"许可与来源原始输入不得进入 Resources：{relative}")

    rootfs_inputs = read_text(
        "tools/apple-aarch64-rootfs-inputs.xcfilelist"
    )
    rootfs_packager = read_text("tools/apple-aarch64-rootfs.sh")
    watch_phase = read_text("tools/apple-watch-rootfs-phase.sh")
    for relative, content in {
        "rootfs 输入清单": rootfs_inputs,
        "rootfs 打包器": rootfs_packager,
        "Watch rootfs 阶段": watch_phase,
    }.items():
        for notices_relative in RESOURCE_CONTRACTS:
            if (
                notices_relative.name in content
                or str(notices_relative) in content
            ):
                fail(f"{notices_relative.name} 不能进入{relative}")

    shell_phases = objects(project, "PBXShellScriptBuildPhase")
    for body in shell_phases.values():
        name = property_value(body, "name") or ""
        if "RootFS" not in name:
            continue
        for notices_relative in RESOURCE_CONTRACTS:
            if notices_relative.name in body:
                fail(
                    f"RootFS Xcode 阶段不能读取或写入 {notices_relative.name}"
                )


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_product_bundles(values):
    forbidden = {}
    for relative in HOST_RAW_INPUTS:
        path = ROOT / relative
        metadata = path.lstat()
        if not stat.S_ISREG(metadata.st_mode):
            fail(f"Apple 宿主原始输入必须是常规文件：{relative}")
        digest = sha256_file(path)
        if digest in forbidden:
            fail("Apple 宿主原始输入摘要必须互不相同")
        forbidden[digest] = relative

    for value in values:
        bundle = Path(value)
        try:
            metadata = bundle.lstat()
        except OSError as error:
            fail(f"无法检查产品 bundle {bundle}：{error}")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(
            metadata.st_mode
        ):
            fail(f"产品 bundle 必须是非符号链接目录：{bundle}")
        for candidate in bundle.rglob("*"):
            if not stat.S_ISREG(candidate.lstat().st_mode):
                continue
            source = forbidden.get(sha256_file(candidate))
            if source is not None:
                fail(
                    "产品不得携带 Apple 宿主原始输入字节："
                    f"{candidate}（来源 {source}）"
                )


def test_product_bundle_gate():
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-license-bundle."
    ) as temporary:
        bundle = Path(temporary) / "Synthetic.app"
        nested = bundle / "PlugIns/Synthetic.appex"
        nested.mkdir(parents=True)
        (bundle / "allowed.txt").write_bytes(b"allowed")
        verify_product_bundles([bundle])

        forbidden = nested / "renamed.bin"
        for relative in HOST_RAW_INPUTS:
            forbidden.write_bytes((ROOT / relative).read_bytes())
            try:
                verify_product_bundles([bundle])
            except ValueError as error:
                if (
                    "产品不得携带 Apple 宿主原始输入字节"
                    not in str(error)
                    or str(relative) not in str(error)
                ):
                    fail(f"产品原始输入负例命中错误诊断：{error}")
            else:
                fail(f"产品原始输入改名复制负例意外通过：{relative}")
            forbidden.unlink()


def normalized(content):
    return re.sub(r"\s+", " ", content)


def require_pattern(content, pattern, message):
    if re.search(pattern, content, re.DOTALL) is None:
        fail(message)


def verify_iphone_contract(project, build_files, resolved):
    about = read_text("app/AboutViewController.m")
    loader = read_text("app/ThirdPartyNoticesViewController.m")
    ui_test = read_text("app/UITests/ThirdPartyNoticesUITests.m")
    storyboard_path = ROOT / "app/Base.lproj/About.storyboard"
    try:
        document = ET.parse(storyboard_path)
    except (OSError, ET.ParseError) as error:
        fail(f"无法解析 About.storyboard：{error}")

    project_lookup = (
        r'URLForResource:\s*@"PROJECT-LICENSES"\s*'
        r'withExtension:\s*@"txt"\s*\]\s*!=\s*nil'
    )
    alpine_lookup = (
        r'URLForResource:\s*@"THIRD-PARTY-NOTICES"\s*'
        r'withExtension:\s*@"txt"\s*\]\s*!=\s*nil'
    )
    host_lookup = (
        r'URLForResource:\s*@"APPLE-HOST-NOTICES"\s*'
        r'withExtension:\s*@"txt"\s*\]\s*!=\s*nil'
    )
    notices_capability = re.search(
        r"self\.hasLicenseNotices\s*=\s*(.*?);",
        about,
        re.DOTALL,
    )
    if notices_capability is None:
        fail("About 页面缺少声明资源能力检查")
    capability = notices_capability.group(1)
    require_pattern(
        capability,
        project_lookup,
        "About 页面缺少项目许可资源能力检查",
    )
    require_pattern(
        capability,
        alpine_lookup,
        "About 页面缺少 Alpine 声明资源能力检查",
    )
    require_pattern(
        capability,
        host_lookup,
        "About 页面缺少 Apple 宿主声明资源能力检查",
    )
    if "ISH_LINUX" in capability:
        fail("About 声明入口不能按 ISH_LINUX 分叉")
    require_pattern(
        normalized(loader),
        r'NSArray<NSString \*> \*resourceNames = @\[\s*'
        r'@"PROJECT-LICENSES",\s*'
        r'@"THIRD-PARTY-NOTICES",\s*'
        r'@"APPLE-HOST-NOTICES",\s*\]',
        "iPhone 查看器必须按固定顺序声明三份可选资源",
    )
    require_pattern(
        loader,
        r"for\s*\(\s*NSString\s*\*\s*resourceName\s+in\s+resourceNames\s*\)"
        r".*?URLForResource:\s*resourceName\s*"
        r'withExtension:\s*@"txt"',
        "iPhone 查看器必须按固定顺序加载存在的声明资源",
    )
    require_pattern(
        loader,
        r"stringWithContentsOfURL:\s*noticesURL.*?"
        r"encoding:\s*NSUTF8StringEncoding",
        "iPhone 查看器必须按 UTF-8 延迟读取声明正文",
    )
    if loader.count('@"third-party-notices-text"') != 1:
        fail("iPhone 声明正文必须有唯一的辅助功能 identifier")
    if "ISH_LINUX" in loader:
        fail("共享 iPhone 声明查看器不能按 ISH_LINUX 分叉")
    if loader.count('self.title = @"Licenses and Source"') != 1:
        fail("iPhone 许可与源码查看器标题漂移")
    source_url = "https://github.com/Eric-Terminal/ish-multiarch"
    if about.count(f'@"{source_url}"') != 1:
        fail("iPhone About 必须唯一链接当前公开源码仓库")
    if "https://github.com/ish-app/ish" in about:
        fail("iPhone About 不能继续链接官方基线仓库")
    require_pattern(
        normalized(about),
        r"numberOfRowsInSection:.*?section == 2"
        r".*?!self\.hasLicenseNotices.*?rows--;",
        "共享 About 页面必须在缺少资源时移除第三段最后一行",
    )
    require_pattern(
        normalized(about),
        r"cell == self\.thirdPartyNoticesCell.*?"
        r'performSegueWithIdentifier:\s*@"showThirdPartyNotices"'
        r"\s*sender:\s*cell",
        "iPhone 声明入口必须由点击回调显式导航",
    )
    require_pattern(
        normalized(ui_test),
        r'launchArguments\s*=\s*@\[\s*@"-recovery"\s*,\s*@"YES"\s*\]',
        "iPhone 声明 UI 测试必须绕过 guest 启动",
    )
    for marker in (
        "===== BEGIN PROJECT LICENSE NOTICE: overview =====",
        "===== BEGIN NOTICE: overview =====",
        "===== BEGIN APPLE HOST NOTICE: overview =====",
    ):
        if ui_test.count(f'@"{marker}"') != 1:
            fail(f"iPhone 声明 UI 测试缺少固定正文标记：{marker}")
    require_pattern(
        normalized(ui_test),
        r"projectRange\.location,\s*0.*?"
        r"alpineRange\.location,\s*NSNotFound.*?"
        r"appleHostRange\.location,\s*NSNotFound.*?"
        r"projectRange\.location,\s*alpineRange\.location.*?"
        r"alpineRange\.location,\s*appleHostRange\.location",
        "iPhone UI 测试必须验证普通 iSH 的三份正文及固定顺序",
    )
    if ui_test.count(f'@"{source_url}"') != 1:
        fail("iPhone UI 测试必须验证项目许可中的公开源码入口")

    scheme_skips = {}
    for relative in (
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH.xcscheme",
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme",
    ):
        try:
            scheme = ET.parse(ROOT / relative)
        except (OSError, ET.ParseError) as error:
            fail(f"无法解析 Xcode scheme {relative}：{error}")
        scheme_skips[relative] = {
            test.get("Identifier")
            for test in scheme.getroot().findall(".//SkippedTests/Test")
            if test.get("Identifier")
        }
    test_class = "ThirdPartyNoticesUITests"
    if test_class in scheme_skips[
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH.xcscheme"
    ]:
        fail("iSH scheme 不能跳过声明 UI 测试")
    if test_class not in scheme_skips[
        "iSH.xcodeproj/xcshareddata/xcschemes/iSH+Linux.xcscheme"
    ]:
        fail("iSH+Linux scheme 必须跳过普通 iSH 双正文声明 UI 测试")
    screenshot_plan = json.loads(
        read_text("app/UITests/Screenshots.xctestplan")
    )
    screenshot_skips = set(
        screenshot_plan["testTargets"][0]["skippedTests"]
    )
    if test_class not in screenshot_skips:
        fail("截图 test plan 必须跳过声明 UI 测试")

    root = document.getroot()
    about_controllers = [
        element
        for element in root.iter("tableViewController")
        if element.get("customClass") == "AboutViewController"
    ]
    if len(about_controllers) != 1:
        fail("About.storyboard 必须有唯一的 AboutViewController")
    sections = about_controllers[0].findall(
        "./tableView/sections/tableViewSection"
    )
    if len(sections) < 4:
        fail("About 页面缺少既有的四段结构")
    if (
        sections[-1].get("id") != "d0T-DL-SuP"
        or sections[-1].get("headerTitle")
        != "Secret Advanced Debugging Options"
    ):
        fail("About 页面的 debug section 必须保持最后一段")
    if sections[2].get("id") != "DVR-sH-TdL":
        fail("声明入口必须位于 About 页面的第三段")
    cells = sections[2].findall("./cells/tableViewCell")
    notice_cells = []
    for cell in cells:
        accessibility = cell.find("./accessibility")
        if (
            accessibility is not None
            and accessibility.get("identifier")
            == "third-party-notices-entry"
        ):
            notice_cells.append(cell)
    if len(notice_cells) != 1:
        fail("About 页面必须有唯一的声明入口 identifier")
    notice_cell = notice_cells[0]
    if not cells or cells[-1] is not notice_cell:
        fail("声明入口必须是 About 页面第三段的最后一行")
    notice_labels = notice_cell.findall(
        "./tableViewCellContentView/subviews/label"
    )
    if len(notice_labels) != 1 or notice_labels[0].get(
        "text"
    ) != "Licenses and Source":
        fail("共享 iPhone 许可证与源码入口标题漂移")
    if len(root.findall('.//label[@text="Source Code on GitHub"]')) != 1:
        fail("iPhone About 必须有唯一的公开源码行标题")
    if notice_cell.findall("./connections/segue"):
        fail("iPhone 声明入口不能依赖静态行自动触发 segue")
    controller_connections = about_controllers[0].find("./connections")
    if controller_connections is None:
        fail("AboutViewController 缺少连接定义")
    notice_outlets = [
        outlet
        for outlet in controller_connections.findall("./outlet")
        if outlet.get("property") == "thirdPartyNoticesCell"
    ]
    if len(notice_outlets) != 1 or notice_outlets[0].get(
        "destination"
    ) != notice_cell.get("id"):
        fail("AboutViewController 必须唯一持有声明入口")
    segues = [
        segue
        for segue in controller_connections.findall("./segue")
        if segue.get("identifier") == "showThirdPartyNotices"
    ]
    if len(segues) != 1 or segues[0].get("kind") != "show":
        fail("AboutViewController 必须唯一导航到固定声明查看器")
    viewer_id = segues[0].get("destination")
    viewers = [
        element
        for element in root.iter("viewController")
        if element.get("id") == viewer_id
    ]
    if (
        len(viewers) != 1
        or viewers[0].get("customClass")
        != "ThirdPartyNoticesViewController"
    ):
        fail("About.storyboard 缺少固定的 iPhone 声明查看器")
    viewer_navigation = viewers[0].find("./navigationItem")
    if (
        viewers[0].get("title") != "Licenses and Source"
        or viewer_navigation is None
        or viewer_navigation.get("title") != "Licenses and Source"
    ):
        fail("iPhone 许可证与源码查看器 storyboard 标题漂移")

    source_reference = require_unique_reference(
        resolved, "app/ThirdPartyNoticesViewController.m"
    )
    build_ids, used_ids, owners = owners_for_reference(
        project, source_reference, build_files, "PBXSourcesBuildPhase"
    )
    if build_ids != used_ids or owners != {"libiSHApp"}:
        fail("iPhone 声明查看器源码必须只进入 libiSHApp")
    ui_test_reference = require_unique_reference(
        resolved, "app/UITests/ThirdPartyNoticesUITests.m"
    )
    build_ids, used_ids, owners = owners_for_reference(
        project, ui_test_reference, build_files, "PBXSourcesBuildPhase"
    )
    if build_ids != used_ids or owners != {"iSHUITests"}:
        fail("iPhone 声明 UI 测试源码必须只进入 iSHUITests")


def verify_watch_contract(project, build_files, resolved):
    content_view = read_text("app/Watch/ContentView.swift")
    notices_view = read_text("app/Watch/ThirdPartyNoticesView.swift")
    ui_test = read_text("app/WatchUITests/iSHWatchUITests.swift")
    watch_sources = sorted((ROOT / "app/Watch").glob("*"))
    for source in watch_sources:
        if source.suffix not in {".swift", ".h"}:
            continue
        relative = str(source.relative_to(ROOT))
        content = read_text(relative)
        if HOST_NOTICES_RELATIVE.name in content:
            fail(f"Watch 源码不得引用 Apple 宿主声明：{relative}")
    require_pattern(
        normalized(notices_view),
        r'resourceNames\s*=\s*\[\s*"PROJECT-LICENSES",\s*'
        r'"THIRD-PARTY-NOTICES",\s*\].*?'
        r"for\s+resourceName\s+in\s+resourceNames.*?"
        r"Bundle\.main\.url\(\s*forResource:\s*resourceName,\s*"
        r'withExtension:\s*"txt"\s*\)',
        "Watch 查看器必须按项目许可、Alpine 的固定顺序加载资源",
    )
    require_pattern(
        notices_view,
        r"private\s+static\s+let\s+loadState.*?"
        r"String\(\s*contentsOf:\s*url,\s*encoding:\s*\.utf8\)",
        "Watch 查看器必须按进程缓存并以 UTF-8 读取正文",
    )
    require_pattern(
        normalized(notices_view),
        r"linesPerChunk\s*=\s*\d+.*?"
        r"split\(.*?omittingEmptySubsequences:\s*false.*?"
        r"LazyVStack.*?Text\(verbatim:\s*chunks\[index\]\)",
        "Watch 查看器必须分块并延迟布局长声明正文",
    )
    required_view_identifiers = {
        "third-party-notices-content",
        "third-party-notices-error",
        "close-third-party-notices",
        "project-source-link",
    }
    for identifier in required_view_identifiers:
        if notices_view.count(f'"{identifier}"') != 1:
            fail(f"Watch 查看器 identifier 漂移：{identifier}")
    if content_view.count('"third-party-notices-button"') != 1:
        fail("Watch 声明入口 identifier 漂移")
    if "ThirdPartyNoticesView()" not in content_view:
        fail("Watch 声明入口没有展示固定查看器")
    if 'accessibilityLabel("许可证与源码")' not in content_view:
        fail("Watch 入口没有使用许可证与源码标题")
    source_url = "https://github.com/Eric-Terminal/ish-multiarch"
    if notices_view.count(source_url) != 1:
        fail("Watch 查看器必须唯一链接当前公开源码仓库")
    if '.navigationTitle("许可证与源码")' not in notices_view:
        fail("Watch 许可证与源码查看器标题漂移")
    for identifier in (
        "third-party-notices-button",
        "third-party-notices-content",
        "project-source-link",
        "close-third-party-notices",
    ):
        if ui_test.count(f'"{identifier}"') != 1:
            fail(f"Watch UI 测试没有覆盖 identifier：{identifier}")

    for relative in (
        "app/Watch/ContentView.swift",
        "app/Watch/ThirdPartyNoticesView.swift",
    ):
        reference = require_unique_reference(resolved, relative)
        build_ids, used_ids, owners = owners_for_reference(
            project, reference, build_files, "PBXSourcesBuildPhase"
        )
        if build_ids != used_ids or owners != {"iSHWatch"}:
            fail(f"{relative} 必须只进入 iSHWatch Sources")
    ui_test_reference = require_unique_reference(
        resolved, "app/WatchUITests/iSHWatchUITests.swift"
    )
    build_ids, used_ids, owners = owners_for_reference(
        project, ui_test_reference, build_files, "PBXSourcesBuildPhase"
    )
    if build_ids != used_ids or owners != {"iSHWatchUITests"}:
        fail("Watch UI 测试源码必须只进入 iSHWatchUITests")


def main():
    tracked = run_git("ls-files", "-z").decode(
        "utf-8", errors="strict"
    ).split("\0")
    for relative in RESOURCE_CONTRACTS:
        verify_notices_file(relative, tracked)
    project = read_text("iSH.xcodeproj/project.pbxproj")
    build_files = objects(project, "PBXBuildFile")
    file_references = objects(project, "PBXFileReference")
    if not build_files or not file_references:
        fail("Xcode 工程缺少显式 build file 或 file reference")
    resolved = resolve_file_references(project, file_references)
    verify_project_resources(project, build_files, resolved)
    verify_iphone_contract(project, build_files, resolved)
    verify_watch_contract(project, build_files, resolved)
    test_product_bundle_gate()
    verify_product_bundles(sys.argv[1:])
    print("Apple 产品许可、源码与第三方声明接线及产物排除测试通过")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
