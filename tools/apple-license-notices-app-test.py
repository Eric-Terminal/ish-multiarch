#!/usr/bin/env python3

import json
import re
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / "iSH.xcodeproj/project.pbxproj"
NOTICES_RELATIVE = PurePosixPath(
    "third_party/alpine/3.24.1-aarch64/THIRD-PARTY-NOTICES.txt"
)
NOTICES_NAME = NOTICES_RELATIVE.name
EXPECTED_RESOURCE_TARGETS = {"iSH", "iSHWatch"}
EXCLUDED_RESOURCE_TARGETS = {
    "iSH+Linux",
    "iSHFileProvider",
    "iSHUITests",
    "iSHWatchUITests",
    "iSHWatchLinkSmoke",
}
OBJECT_ID = re.compile(r"\b[A-F0-9]{24}\b")


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


def section(project, name):
    begin = f"/* Begin {name} section */"
    end = f"/* End {name} section */"
    if project.count(begin) != 1 or project.count(end) != 1:
        fail(f"Xcode 工程缺少唯一的 {name} section")
    return project.split(begin, 1)[1].split(end, 1)[0]


def objects(project, section_name):
    parsed = {}
    lines = section(project, section_name).splitlines()
    index = 0
    start = re.compile(
        r"^(\s*)([A-F0-9]{24})(?: /\*.*\*/)? = \{(.*)$"
    )
    while index < len(lines):
        match = start.match(lines[index])
        if match is None:
            index += 1
            continue
        indent, identifier, remainder = match.groups()
        if identifier in parsed:
            fail(f"{section_name} 中对象 {identifier} 重复")
        if remainder.rstrip().endswith("};"):
            body = remainder.rstrip()[:-2]
        else:
            body_lines = [remainder]
            index += 1
            closing = indent + "};"
            while index < len(lines) and lines[index] != closing:
                body_lines.append(lines[index])
                index += 1
            if index == len(lines):
                fail(f"{section_name} 中对象 {identifier} 没有闭合")
            body = "\n".join(body_lines)
        parsed[identifier] = body
        index += 1
    return parsed


def unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
        value = value.replace(r"\"", '"').replace(r"\\", "\\")
    return value


def property_value(body, name):
    match = re.search(
        rf"(?:^|[;\n])\s*{re.escape(name)}\s*=\s*"
        r'("(?:\\.|[^"])*"|[^;\n]+);',
        body,
    )
    return None if match is None else unquote(match.group(1))


def list_ids(body, name):
    match = re.search(
        rf"(?:^|[;\n])\s*{re.escape(name)}\s*=\s*\((.*?)\);",
        body,
        re.DOTALL,
    )
    return [] if match is None else OBJECT_ID.findall(match.group(1))


def resolve_file_references(project, file_references):
    groups = objects(project, "PBXGroup")
    projects = objects(project, "PBXProject")
    if len(projects) != 1:
        fail("Xcode 工程必须只有一个 PBXProject 对象")
    main_group = property_value(next(iter(projects.values())), "mainGroup")
    if main_group not in groups:
        fail("Xcode 工程的 mainGroup 无效")

    resolved = {}
    visiting = set()

    def walk(group_id, parent):
        if group_id in visiting:
            fail("Xcode PBXGroup 出现循环")
        visiting.add(group_id)
        body = groups[group_id]
        group_path = property_value(body, "path")
        base = parent
        if group_path:
            base = parent / PurePosixPath(group_path)
        for child in list_ids(body, "children"):
            if child in groups:
                walk(child, base)
            elif child in file_references:
                child_path = property_value(file_references[child], "path")
                source_tree = property_value(
                    file_references[child], "sourceTree"
                )
                if child_path:
                    if source_tree == "<group>":
                        relative = str(base / PurePosixPath(child_path))
                    elif source_tree == "SOURCE_ROOT":
                        relative = str(PurePosixPath(child_path))
                    else:
                        continue
                    resolved.setdefault(relative, []).append(child)
        visiting.remove(group_id)

    walk(main_group, PurePosixPath("."))
    return resolved


def phase_owners(project, phase_section):
    phases = objects(project, phase_section)
    targets = objects(project, "PBXNativeTarget")
    target_names = {}
    owners = {phase_id: set() for phase_id in phases}
    for target_id, body in targets.items():
        name = property_value(body, "name")
        if not name:
            fail(f"PBXNativeTarget {target_id} 缺少 name")
        if name in target_names:
            fail(f"PBXNativeTarget 名称重复：{name}")
        target_names[name] = target_id
        for phase_id in list_ids(body, "buildPhases"):
            if phase_id in owners:
                owners[phase_id].add(name)
    return phases, owners, target_names


def build_file_reference(body):
    reference = property_value(body, "fileRef")
    if reference is None:
        return None
    identifiers = OBJECT_ID.findall(reference)
    if len(identifiers) != 1:
        fail("PBXBuildFile 缺少有效的 fileRef")
    return identifiers[0]


def owners_for_reference(
    project, reference, build_files, phase_section
):
    build_ids = {
        identifier
        for identifier, body in build_files.items()
        if build_file_reference(body) == reference
    }
    phases, phase_owner_map, _ = phase_owners(project, phase_section)
    used_build_ids = set()
    owners = set()
    for phase_id, body in phases.items():
        members = set(list_ids(body, "files"))
        matches = build_ids & members
        if matches:
            used_build_ids.update(matches)
            owners.update(phase_owner_map[phase_id])
    return build_ids, used_build_ids, owners


def require_unique_reference(resolved, relative):
    references = resolved.get(relative, [])
    if len(references) != 1:
        fail(f"Xcode 工程必须唯一引用 {relative}")
    return references[0]


def verify_notices_file():
    notices = ROOT / NOTICES_RELATIVE
    try:
        metadata = notices.lstat()
    except OSError as error:
        fail(f"找不到声明正文：{error}")
    if not stat.S_ISREG(metadata.st_mode):
        fail("声明正文必须是常规文件，不能是目录、链接或特殊文件")

    stage = run_git("ls-files", "--stage", "--", str(NOTICES_RELATIVE))
    stage_lines = stage.decode("utf-8", errors="strict").splitlines()
    expected_suffix = "\t" + str(NOTICES_RELATIVE)
    if (
        len(stage_lines) != 1
        or not stage_lines[0].startswith("100644 ")
        or not stage_lines[0].endswith(expected_suffix)
    ):
        fail("声明正文必须以唯一的 Git 100644 文件受跟踪")

    tracked = run_git("ls-files", "-z").decode(
        "utf-8", errors="strict"
    ).split("\0")
    same_name = [
        path
        for path in tracked
        if path and PurePosixPath(path).name == NOTICES_NAME
    ]
    if same_name != [str(NOTICES_RELATIVE)]:
        fail("仓库中只能跟踪一份 THIRD-PARTY-NOTICES.txt")


def verify_project_resources(project, build_files, resolved):
    reference = require_unique_reference(
        resolved, str(NOTICES_RELATIVE)
    )
    build_ids, used_ids, owners = owners_for_reference(
        project, reference, build_files, "PBXResourcesBuildPhase"
    )
    if len(build_ids) != 2 or build_ids != used_ids:
        fail("声明正文必须通过恰好两个 Resources build file 打包")
    if owners != EXPECTED_RESOURCE_TARGETS:
        fail(
            "声明正文只能进入 iSH 与 iSHWatch，实际为："
            + ", ".join(sorted(owners))
        )

    _, _, target_names = phase_owners(
        project, "PBXResourcesBuildPhase"
    )
    missing = EXCLUDED_RESOURCE_TARGETS - set(target_names)
    if missing:
        fail("缺少必须验证的排除 target：" + ", ".join(sorted(missing)))

    for build_id in build_ids:
        if len(re.findall(rf"\b{build_id}\b", project)) != 2:
            fail(f"资源成员 {build_id} 只能出现在定义和目标阶段各一次")

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
        if NOTICES_NAME in content or str(NOTICES_RELATIVE) in content:
            fail(f"声明正文不能进入{relative}")

    shell_phases = objects(project, "PBXShellScriptBuildPhase")
    for body in shell_phases.values():
        name = property_value(body, "name") or ""
        if "RootFS" in name and NOTICES_NAME in body:
            fail("RootFS Xcode 阶段不能读取或写入声明正文")


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

    resource_lookup = (
        r'URLForResource:\s*@"THIRD-PARTY-NOTICES"\s*'
        r'withExtension:\s*@"txt"'
    )
    require_pattern(about, resource_lookup, "About 页面缺少声明资源能力检查")
    require_pattern(loader, resource_lookup, "iPhone 查看器缺少声明资源 loader")
    require_pattern(
        loader,
        r"stringWithContentsOfURL:\s*noticesURL.*?"
        r"encoding:\s*NSUTF8StringEncoding",
        "iPhone 查看器必须按 UTF-8 延迟读取声明正文",
    )
    if loader.count('@"third-party-notices-text"') != 1:
        fail("iPhone 声明正文必须有唯一的辅助功能 identifier")
    require_pattern(
        normalized(about),
        r"numberOfRowsInSection:.*?section == 2"
        r".*?!self\.hasThirdPartyNotices.*?rows--;",
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
        fail("iSH+Linux scheme 必须跳过不适用于该产品的声明 UI 测试")
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
    ) != "Alpine AArch64 Notices":
        fail("iPhone 声明入口必须明确限定 Alpine AArch64 范围")
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
    require_pattern(
        notices_view,
        r'Bundle\.main\.url\(\s*forResource:\s*"THIRD-PARTY-NOTICES"'
        r',\s*withExtension:\s*"txt"\s*\)',
        "Watch 查看器缺少声明资源 loader",
    )
    require_pattern(
        notices_view,
        r"private\s+static\s+let\s+loadState.*?"
        r"String\(contentsOf:\s*url,\s*encoding:\s*\.utf8\)",
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
    }
    for identifier in required_view_identifiers:
        if notices_view.count(f'"{identifier}"') != 1:
            fail(f"Watch 查看器 identifier 漂移：{identifier}")
    if content_view.count('"third-party-notices-button"') != 1:
        fail("Watch 声明入口 identifier 漂移")
    if "ThirdPartyNoticesView()" not in content_view:
        fail("Watch 声明入口没有展示固定查看器")
    if 'accessibilityLabel("Alpine AArch64 许可声明")' not in content_view:
        fail("Watch 声明入口没有明确限定 Alpine AArch64 范围")
    for identifier in (
        "third-party-notices-button",
        "third-party-notices-content",
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
    verify_notices_file()
    project = read_text("iSH.xcodeproj/project.pbxproj")
    build_files = objects(project, "PBXBuildFile")
    file_references = objects(project, "PBXFileReference")
    if not build_files or not file_references:
        fail("Xcode 工程缺少显式 build file 或 file reference")
    resolved = resolve_file_references(project, file_references)
    verify_project_resources(project, build_files, resolved)
    verify_iphone_contract(project, build_files, resolved)
    verify_watch_contract(project, build_files, resolved)
    print("Apple 双端 Alpine AArch64 许可声明接线测试通过")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
