#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import sys
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

MANIFEST = "distribution/apple/release-profiles.tsv"
HEADER = (
    "profile\trole\ttarget\tscheme\tdebug_configuration"
    "\trelease_configuration\tkernel\trootfs"
)
PROFILE_NAME = re.compile(r"^[a-z][a-z0-9-]*$")
TARGET_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._-]*$")


class ProfileError(Exception):
    pass


@dataclass(frozen=True, order=True)
class ProfileRow:
    profile: str
    role: str
    target: str
    scheme: str
    debug_configuration: str
    release_configuration: str
    kernel: str
    rootfs: str


EXPECTED_ROWS = (
    ProfileRow(
        "core",
        "iphone-app",
        "iSH",
        "iSH",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
    ProfileRow(
        "core",
        "watch-app",
        "iSHWatch",
        "iSHWatch",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
    ProfileRow(
        "with-linux",
        "iphone-app",
        "iSH+Linux",
        "iSH+Linux",
        "DebugLinux",
        "ReleaseLinux",
        "linux",
        "online-root-tar",
    ),
    ProfileRow(
        "with-linux",
        "watch-app",
        "iSHWatch",
        "iSHWatch",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
)
EXPECTED_CONFIG_INCLUDES = {
    "app/ProjectDebug.xcconfig": (
        "XcodeDebug.xcconfig",
        "Project.xcconfig",
        "NotLinux.xcconfig",
    ),
    "app/ProjectRelease.xcconfig": (
        "XcodeRelease.xcconfig",
        "Project.xcconfig",
        "NotLinux.xcconfig",
    ),
    "app/ProjectDebugLinux.xcconfig": (
        "XcodeDebug.xcconfig",
        "Project.xcconfig",
        "Linux.xcconfig",
    ),
    "app/ProjectReleaseLinux.xcconfig": (
        "XcodeRelease.xcconfig",
        "Project.xcconfig",
        "Linux.xcconfig",
    ),
}
PROFILE_INPUTS = (
    "$(SRCROOT)/distribution/apple/release-profiles.tsv",
    "$(SRCROOT)/tools/apple-release-profiles.py",
)
PROFILE_PHASE = "校验发行候选 Profile"
ROOTFS_INPUT_LIST = (
    "$(SRCROOT)/tools/apple-aarch64-rootfs-inputs.xcfilelist"
)
EXPECTED_SKIPPED_TESTS = {
    "iSH": ("Screenshots",),
    "iSH+Linux": ("Screenshots", "ThirdPartyNoticesUITests"),
    "iSHWatch": (),
}
PBX = None


def fail(message: str) -> None:
    raise ProfileError(message)


def load_pbx() -> None:
    global PBX
    if PBX is None:
        import apple_pbx

        PBX = apple_pbx


def validate_relative(value: str, description: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or str(path) != value
        or any(part in ("", ".", "..") for part in path.parts)
        or "\\" in value
    ):
        fail(f"{description}不是安全的仓库相对路径：{value}")
    return path


def regular_path(root: Path, relative: str, description: str) -> Path:
    path = validate_relative(relative, description)
    current = root
    for part in path.parts:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{description}不存在：{relative}（{error}）")
        if stat.S_ISLNK(metadata.st_mode):
            fail(f"{description}不能经过符号链接：{relative}")
    if not stat.S_ISREG(current.lstat().st_mode):
        fail(f"{description}必须是普通文件：{relative}")
    return current


def read_bytes(root: Path, relative: str, description: str) -> bytes:
    path = regular_path(root, relative, description)
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"无法读取{description}：{relative}（{error}）")


def read_utf8(root: Path, relative: str, description: str) -> str:
    try:
        return read_bytes(root, relative, description).decode("utf-8")
    except UnicodeDecodeError:
        fail(f"{description}不是 UTF-8：{relative}")


def load_profiles(root: Path) -> tuple[ProfileRow, ...]:
    data = read_bytes(root, MANIFEST, "Apple 发行候选 profile 清单")
    if not data or not data.endswith(b"\n") or b"\r" in data:
        fail("Apple 发行候选 profile 清单必须非空、只用 LF 并以换行结束")
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        fail("Apple 发行候选 profile 清单不是 UTF-8")
    if lines[0] != HEADER:
        fail("Apple 发行候选 profile 清单表头漂移")
    rows = []
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 8 or any(not field for field in fields):
            fail("Apple 发行候选 profile 清单含空字段或列数错误")
        row = ProfileRow(*fields)
        if (
            PROFILE_NAME.fullmatch(row.profile) is None
            or row.role not in {"iphone-app", "watch-app"}
            or TARGET_NAME.fullmatch(row.target) is None
            or TARGET_NAME.fullmatch(row.scheme) is None
            or TARGET_NAME.fullmatch(row.debug_configuration) is None
            or TARGET_NAME.fullmatch(row.release_configuration) is None
            or row.kernel not in {"ish", "linux"}
            or row.rootfs not in {
                "fixed-alpine-seed",
                "online-root-tar",
            }
        ):
            fail("Apple 发行候选 profile 清单字段非法")
        rows.append(row)
    if rows != sorted(rows) or len(rows) != len(set(rows)):
        fail("Apple 发行候选 profile 清单必须按完整记录唯一排序")
    if tuple(rows) != EXPECTED_ROWS:
        fail("Apple 发行候选 profile 合同漂移")
    return tuple(rows)


def single(elements, description: str):
    if len(elements) != 1:
        fail(f"{description}必须唯一")
    return elements[0]


def expected_product_name(target: str) -> str:
    if target in {"iSHUITests", "iSHWatchUITests"}:
        return f"{target}.xctest"
    return "iSHWatch.app" if target == "iSHWatch" else "iSH.app"


def expected_ui_test_target(row: ProfileRow) -> str:
    return "iSHWatchUITests" if row.role == "watch-app" else "iSHUITests"


def validate_buildable_reference(
    reference,
    target: str,
    target_id: str,
    description: str,
) -> None:
    expected = {
        "BuildableIdentifier": "primary",
        "BlueprintIdentifier": target_id,
        "BuildableName": expected_product_name(target),
        "BlueprintName": target,
        "ReferencedContainer": "container:iSH.xcodeproj",
    }
    if any(reference.get(name) != value for name, value in expected.items()):
        fail(f"{description}的产品引用漂移")


def load_scheme(root: Path, scheme_name: str):
    relative = (
        "iSH.xcodeproj/xcshareddata/xcschemes/"
        f"{scheme_name}.xcscheme"
    )
    data = read_bytes(root, relative, f"{scheme_name} scheme")
    if b"<!DOCTYPE" in data or b"<!ENTITY" in data:
        fail(f"{scheme_name} scheme 含不支持的 XML 声明")
    try:
        scheme = ET.fromstring(data)
    except ET.ParseError as error:
        fail(f"{scheme_name} scheme 不是有效 XML：{error}")
    if scheme.tag != "Scheme":
        fail(f"{scheme_name} scheme 缺少 Scheme 根节点")
    return scheme


def validate_scheme(
    root: Path,
    row: ProfileRow,
    target_id: str,
    test_target_id: str,
) -> None:
    scheme = load_scheme(root, row.scheme)

    build_reference = single(
        scheme.findall(
            "./BuildAction/BuildActionEntries/"
            "BuildActionEntry/BuildableReference"
        ),
        f"{row.scheme} BuildAction 产品引用",
    )
    build_entry = single(
        scheme.findall("./BuildAction/BuildActionEntries/BuildActionEntry"),
        f"{row.scheme} BuildActionEntry",
    )
    if any(
        build_entry.get(attribute) != "YES"
        for attribute in (
            "buildForTesting",
            "buildForRunning",
            "buildForProfiling",
            "buildForArchiving",
            "buildForAnalyzing",
        )
    ):
        fail(f"{row.scheme} BuildAction 执行范围漂移")
    validate_buildable_reference(
        build_reference,
        row.target,
        target_id,
        f"{row.scheme} BuildAction",
    )

    expected_configurations = {
        "TestAction": row.debug_configuration,
        "LaunchAction": row.debug_configuration,
        "AnalyzeAction": row.debug_configuration,
        "ProfileAction": row.release_configuration,
        "ArchiveAction": row.release_configuration,
    }
    for action_name, configuration in expected_configurations.items():
        action = single(
            scheme.findall(f"./{action_name}"),
            f"{row.scheme} {action_name}",
        )
        if action.get("buildConfiguration") != configuration:
            fail(f"{row.scheme} {action_name} 的 configuration 漂移")

    launch_reference = single(
        scheme.findall(
            "./LaunchAction/BuildableProductRunnable/BuildableReference"
        ),
        f"{row.scheme} LaunchAction 产品引用",
    )
    validate_buildable_reference(
        launch_reference,
        row.target,
        target_id,
        f"{row.scheme} LaunchAction",
    )

    test_references = scheme.findall(
        "./TestAction/MacroExpansion/BuildableReference"
    )
    if len(test_references) > 1:
        fail(f"{row.scheme} TestAction 产品引用不能重复")
    if test_references:
        validate_buildable_reference(
            test_references[0],
            row.target,
            target_id,
            f"{row.scheme} TestAction",
        )

    testable = single(
        scheme.findall("./TestAction/Testables/TestableReference"),
        f"{row.scheme} TestAction 测试目标",
    )
    if testable.get("skipped") != "NO":
        fail(f"{row.scheme} TestAction 测试目标被跳过")
    test_target = expected_ui_test_target(row)
    testable_reference = single(
        testable.findall("./BuildableReference"),
        f"{row.scheme} TestAction 测试引用",
    )
    validate_buildable_reference(
        testable_reference,
        test_target,
        test_target_id,
        f"{row.scheme} TestAction 测试引用",
    )
    if testable.findall("./SelectedTests"):
        fail(f"{row.scheme} TestAction 不能缩小发行候选测试集合")
    skipped_containers = testable.findall("./SkippedTests")
    expected_skipped = EXPECTED_SKIPPED_TESTS[row.scheme]
    if len(skipped_containers) != (1 if expected_skipped else 0):
        fail(f"{row.scheme} TestAction 的跳过测试集合漂移")
    skipped = ()
    if skipped_containers:
        children = list(skipped_containers[0])
        if any(
            item.tag != "Test"
            or set(item.attrib) != {"Identifier"}
            or list(item)
            for item in children
        ):
            fail(f"{row.scheme} TestAction 的跳过测试集合漂移")
        skipped = tuple(
            item.get("Identifier")
            for item in children
        )
    if skipped != expected_skipped:
        fail(f"{row.scheme} TestAction 的跳过测试集合漂移")

    profile_references = scheme.findall(
        "./ProfileAction/BuildableProductRunnable/BuildableReference"
    )
    profile_references += scheme.findall(
        "./ProfileAction/MacroExpansion/BuildableReference"
    )
    profile_reference = single(
        profile_references,
        f"{row.scheme} ProfileAction 产品引用",
    )
    validate_buildable_reference(
        profile_reference,
        row.target,
        target_id,
        f"{row.scheme} ProfileAction",
    )


def validate_screenshots_scheme(
    root: Path,
    app_target_id: str,
    test_target_id: str,
) -> None:
    scheme = load_scheme(root, "Screenshots")
    entries = scheme.findall(
        "./BuildAction/BuildActionEntries/BuildActionEntry"
    )
    if len(entries) != 3:
        fail("Screenshots BuildAction 合同漂移")

    references = {}
    plan_entry = None
    for entry in entries:
        build_references = entry.findall("./BuildableReference")
        plan_references = entry.findall("./TestPlanReference")
        if len(build_references) == 1 and not plan_references:
            identifier = build_references[0].get("BlueprintIdentifier")
            if not identifier or identifier in references:
                fail("Screenshots BuildAction 产品引用漂移")
            references[identifier] = (entry, build_references[0])
        elif len(plan_references) == 1 and not build_references:
            if plan_entry is not None:
                fail("Screenshots BuildAction 测试计划重复")
            plan_entry = (entry, plan_references[0])
        else:
            fail("Screenshots BuildAction 合同漂移")

    expected_entries = {
        app_target_id: (
            "iSH",
            {
                "buildForTesting": "YES",
                "buildForRunning": "NO",
                "buildForProfiling": "NO",
                "buildForArchiving": "NO",
                "buildForAnalyzing": "NO",
            },
        ),
        test_target_id: (
            "iSHUITests",
            {
                "buildForTesting": "YES",
                "buildForRunning": "YES",
                "buildForProfiling": "NO",
                "buildForArchiving": "NO",
                "buildForAnalyzing": "NO",
            },
        ),
    }
    if set(references) != set(expected_entries):
        fail("Screenshots BuildAction 产品集合漂移")
    for identifier, (target, attributes) in expected_entries.items():
        entry, reference = references[identifier]
        if any(
            entry.get(name) != value
            for name, value in attributes.items()
        ):
            fail("Screenshots BuildAction 执行范围漂移")
        validate_buildable_reference(
            reference,
            target,
            identifier,
            "Screenshots BuildAction",
        )

    plan_path = "container:app/UITests/Screenshots.xctestplan"
    plan_attributes = {
        "buildForTesting": "YES",
        "buildForRunning": "NO",
        "buildForProfiling": "NO",
        "buildForArchiving": "NO",
        "buildForAnalyzing": "NO",
    }
    if (
        plan_entry is None
        or any(
            plan_entry[0].get(name) != value
            for name, value in plan_attributes.items()
        )
        or plan_entry[1].attrib != {"reference": plan_path}
    ):
        fail("Screenshots BuildAction 测试计划漂移")

    expected_configurations = {
        "TestAction": "Debug-ApplePleaseFixFB19282108",
        "LaunchAction": "Debug-ApplePleaseFixFB19282108",
        "AnalyzeAction": "Debug-ApplePleaseFixFB19282108",
        "ProfileAction": "Release",
        "ArchiveAction": "Release",
    }
    actions = {}
    for action_name, configuration in expected_configurations.items():
        action = single(
            scheme.findall(f"./{action_name}"),
            f"Screenshots {action_name}",
        )
        if action.get("buildConfiguration") != configuration:
            fail(
                f"Screenshots {action_name} 的 configuration 漂移"
            )
        actions[action_name] = action
    test_action = actions["TestAction"]
    test_plan = single(
        test_action.findall("./TestPlans/TestPlanReference"),
        "Screenshots TestAction 测试计划",
    )
    if test_plan.attrib != {"reference": plan_path, "default": "YES"}:
        fail("Screenshots TestAction 测试计划漂移")
    testable = single(
        test_action.findall("./Testables/TestableReference"),
        "Screenshots TestAction 测试目标",
    )
    if testable.get("skipped") != "NO":
        fail("Screenshots TestAction 测试目标被跳过")
    reference = single(
        testable.findall("./BuildableReference"),
        "Screenshots TestAction 测试引用",
    )
    validate_buildable_reference(
        reference,
        "iSHUITests",
        test_target_id,
        "Screenshots TestAction 测试引用",
    )


def target_map(project: str):
    targets = PBX.objects(project, "PBXNativeTarget")
    by_name = {}
    for identifier, body in targets.items():
        name = PBX.property_value(body, "name")
        if not name or name in by_name:
            fail("Xcode 工程 target 名称缺失或重复")
        by_name[name] = (identifier, body)
    return by_name


def referenced_identifier(
    body: str,
    name: str,
    objects: dict,
    description: str,
) -> str:
    identifiers = PBX.OBJECT_ID.findall(PBX.property_value(body, name) or "")
    if len(identifiers) != 1 or identifiers[0] not in objects:
        fail(f"{description}缺少有效的 {name}")
    return identifiers[0]


def product_reference_path(project: str, target_body: str) -> str:
    references = PBX.objects(project, "PBXFileReference")
    identifier = referenced_identifier(
        target_body,
        "productReference",
        references,
        "Xcode App target",
    )
    reference = references[identifier]
    path = PBX.property_value(reference, "path")
    if (
        PBX.property_value(reference, "sourceTree")
        != "BUILT_PRODUCTS_DIR"
        or path is None
    ):
        fail("Xcode App target 产品引用漂移")
    return path


def pbx_atom_list(
    body: str,
    name: str,
    description: str,
) -> tuple[str, ...]:
    match = re.search(
        rf"(?:^|[;{{\n])\s*{re.escape(name)}\s*=\s*\((.*?)\);",
        body,
        re.DOTALL,
    )
    if match is None:
        fail(f"{description}缺少 {name} 列表")
    content = match.group(1)
    item = re.compile(
        r'\s*("(?:\\.|[^"])*"|[A-Za-z0-9_.$()+/<>=*-]+)\s*,'
    )
    values = []
    position = 0
    while position < len(content):
        token = item.match(content, position)
        if token is None:
            if content[position:].strip():
                fail(f"{description}的 {name} 列表无法解析")
            break
        values.append(PBX.unquote(token.group(1)))
        position = token.end()
    return tuple(values)


def validate_file_provider_embedding(project: str) -> None:
    build_files = PBX.objects(project, "PBXBuildFile")
    paths = PBX.normalized_reference_paths(project)
    phases, owners, _targets = PBX.phase_owners(
        project, "PBXCopyFilesBuildPhase"
    )
    embeddings = {}
    for phase_id, phase in phases.items():
        build_ids = PBX.list_ids(phase, "files")
        for build_id in build_ids:
            build_file = build_files.get(build_id)
            if build_file is None:
                fail(f"Copy Files phase 引用了未知 build file：{build_id}")
            reference = PBX.build_file_reference(build_file)
            if reference is None or reference not in paths:
                fail(f"无法解析 Copy Files build file：{build_id}")
            if paths[reference] != "iSHFileProvider.appex":
                continue
            if len(owners[phase_id]) != 1:
                fail("FileProvider Copy Files phase 归属漂移")
            owner = next(iter(owners[phase_id]))
            embeddings.setdefault(owner, []).append(
                (phase, build_ids, build_id, build_file)
            )

    if set(embeddings) != {"iSH", "iSH+Linux"} or any(
        len(items) != 1 for items in embeddings.values()
    ):
        fail("FileProvider 没有被两个互斥 iPhone target 各嵌入一次")
    for owner, items in embeddings.items():
        phase, build_ids, build_id, build_file = items[0]
        if (
            build_ids != [build_id]
            or PBX.property_value(phase, "name")
            != "Embed Foundation Extensions"
            or PBX.property_value(phase, "dstPath") != ""
            or PBX.property_value(phase, "dstSubfolderSpec") != "13"
            or PBX.property_value(phase, "buildActionMask")
            != "2147483647"
            or PBX.property_value(
                phase, "runOnlyForDeploymentPostprocessing"
            )
            != "0"
        ):
            fail(f"{owner} 的 FileProvider Copy Files phase 漂移")
        if pbx_atom_list(
            build_file,
            "ATTRIBUTES",
            f"{owner} 的 FileProvider build file",
        ) != ("RemoveHeadersOnCopy",):
            fail(f"{owner} 的 FileProvider 嵌入属性漂移")


def validate_project_targets(
    root: Path,
    rows: tuple[ProfileRow, ...],
) -> tuple[str, dict]:
    project = read_utf8(
        root,
        "iSH.xcodeproj/project.pbxproj",
        "Xcode 工程",
    )
    definitions = re.findall(
        r"^\t\t([A-F0-9]{24})(?: /\*.*\*/)? = \{",
        project,
        re.MULTILINE,
    )
    if len(definitions) != len(set(definitions)):
        fail("Xcode 工程对象 ID 跨 section 重复")
    targets = target_map(project)
    expected_targets = {row.target for row in rows}
    if expected_targets != {"iSH", "iSH+Linux", "iSHWatch"}:
        fail("发行候选 profile 混入非产品 target")
    identifiers = {}
    for target in sorted(expected_targets):
        if target not in targets:
            fail(f"Xcode 工程缺少发行候选 target：{target}")
        identifier, body = targets[target]
        if (
            PBX.property_value(body, "productType")
            != "com.apple.product-type.application"
        ):
            fail(f"{target} 不是 application target")
        expected_name = "iSHWatch" if target == "iSHWatch" else "iSH"
        if PBX.property_value(body, "productName") != expected_name:
            fail(f"{target} 的 ProductName 漂移")
        if product_reference_path(project, body) != expected_product_name(
            target
        ):
            fail(f"{target} 的 App 产品路径漂移")
        identifiers[target] = identifier

    for target in ("iSHUITests", "iSHWatchUITests"):
        if target not in targets:
            fail(f"Xcode 工程缺少发行候选 UI 测试 target：{target}")
        identifier, body = targets[target]
        if (
            PBX.property_value(body, "productType")
            != "com.apple.product-type.bundle.ui-testing"
            or PBX.property_value(body, "productName") != target
            or product_reference_path(project, body)
            != expected_product_name(target)
        ):
            fail(f"{target} 的 UI 测试产品身份漂移")
        identifiers[target] = identifier

    if PBX.list_ids(targets["iSHUITests"][1], "dependencies"):
        fail("iSHUITests 不能静态依赖某一个互斥 iPhone App")

    validate_file_provider_embedding(project)
    return project, identifiers


def config_includes(text: str) -> tuple[str, ...]:
    lines = text.splitlines()
    matches = []
    for line in lines:
        match = re.fullmatch(r'#include "([^"]+)"', line)
        if match is None:
            fail("产品 xcconfig 含非 include 内容或格式漂移")
        matches.append(match.group(1))
    return tuple(matches)


def unique_setting(text: str, name: str, description: str) -> str:
    matches = re.findall(
        rf"^{re.escape(name)}[ \t]*=[ \t]*(.*)$",
        text,
        re.MULTILINE,
    )
    if len(matches) != 1:
        fail(f"{description}缺少唯一 {name}")
    return matches[0].strip()


def inline_setting_keys(body: str, name: str) -> tuple[str, ...]:
    return tuple(
        re.findall(
            rf'(?:^|[;\n])\s*"?('
            rf"{re.escape(name)}(?:\[[^\"\]\n]+\])?"
            rf')"?\s*=',
            body,
        )
    )


def configuration_paths(
    project: str,
    configuration_list_id: str,
) -> dict[str, str]:
    lists = PBX.objects(project, "XCConfigurationList")
    configurations = PBX.objects(project, "XCBuildConfiguration")
    references = PBX.objects(project, "PBXFileReference")
    reference_paths = PBX.normalized_reference_paths(project)
    if configuration_list_id not in lists:
        fail("Xcode configuration list 引用无效")
    result = {}
    for identifier in PBX.list_ids(
        lists[configuration_list_id], "buildConfigurations"
    ):
        if identifier not in configurations:
            fail("Xcode configuration list 含未知 configuration")
        body = configurations[identifier]
        name = PBX.property_value(body, "name")
        if not name or name in result:
            fail("Xcode configuration 名称缺失或重复")
        reference_id = referenced_identifier(
            body,
            "baseConfigurationReference",
            references,
            f"Xcode {name} configuration",
        )
        path = reference_paths.get(reference_id)
        if path is None:
            fail(f"Xcode {name} configuration 的 xcconfig 路径无效")
        result[name] = path
    return result


def validate_configuration_graph(project: str) -> tuple[str, ...]:
    expected_project = {
        "Debug-ApplePleaseFixFB19282108": "app/ProjectDebug.xcconfig",
        "DebugLinux": "app/ProjectDebugLinux.xcconfig",
        "Release": "app/ProjectRelease.xcconfig",
        "ReleaseLinux": "app/ProjectReleaseLinux.xcconfig",
    }
    projects = PBX.objects(project, "PBXProject")
    if len(projects) != 1:
        fail("Xcode 工程必须只有一个 PBXProject")
    configuration_lists = PBX.objects(project, "XCConfigurationList")
    project_body = next(iter(projects.values()))
    project_list = referenced_identifier(
        project_body,
        "buildConfigurationList",
        configuration_lists,
        "PBXProject",
    )
    if configuration_paths(project, project_list) != expected_project:
        fail("Xcode project configuration 与产品 xcconfig 映射漂移")

    base_paths = set(expected_project.values())
    targets = target_map(project)
    all_configurations = set(expected_project)
    target_lists = {}
    for target, expected_path in (
        ("iSH", "app/App.xcconfig"),
        ("iSH+Linux", "app/App.xcconfig"),
        ("iSHWatch", "app/WatchApp.xcconfig"),
        ("iSHUITests", "app/iOS.xcconfig"),
        ("iSHWatchUITests", "app/WatchUITests.xcconfig"),
    ):
        _target_id, target_body = targets[target]
        target_list = referenced_identifier(
            target_body,
            "buildConfigurationList",
            configuration_lists,
            f"{target} target",
        )
        target_lists[target] = target_list
        paths = configuration_paths(project, target_list)
        if (
            set(paths) != all_configurations
            or set(paths.values()) != {expected_path}
        ):
            fail(f"{target} target configuration 映射漂移")
        base_paths.update(paths.values())

    configurations = PBX.objects(project, "XCBuildConfiguration")
    actual_test_targets = {}
    for identifier in PBX.list_ids(
        configuration_lists[target_lists["iSHUITests"]],
        "buildConfigurations",
    ):
        body = configurations[identifier]
        name = PBX.property_value(body, "name")
        if inline_setting_keys(body, "TEST_TARGET_NAME") != (
            "TEST_TARGET_NAME",
        ):
            fail("iSHUITests 含条件化或重复的被测 App 覆盖")
        actual_test_targets[name] = PBX.property_value(
            body, "TEST_TARGET_NAME"
        )
    expected_test_targets = {
        "Debug-ApplePleaseFixFB19282108": "iSH",
        "DebugLinux": "iSH+Linux",
        "Release": "iSH",
        "ReleaseLinux": "iSH+Linux",
    }
    if actual_test_targets != expected_test_targets:
        fail("iSHUITests 的 configuration 与被测 App 映射漂移")

    for identifier in PBX.list_ids(
        configuration_lists[target_lists["iSHWatchUITests"]],
        "buildConfigurations",
    ):
        body = configurations[identifier]
        if inline_setting_keys(body, "TEST_TARGET_NAME"):
            fail("iSHWatchUITests 不能覆盖 xcconfig 的被测 App")
    return tuple(sorted(base_paths))


def validate_xcconfig_tree(
    root: Path,
    base_paths: tuple[str, ...],
) -> None:
    visited = set()
    visiting = set()

    def walk(relative: str) -> None:
        if relative in visiting:
            fail(f"xcconfig include 出现循环：{relative}")
        if relative in visited:
            return
        visiting.add(relative)
        text = read_utf8(root, relative, f"{relative} xcconfig")
        for number, line in enumerate(text.splitlines(), start=1):
            if not line.lstrip().startswith("#include"):
                continue
            match = re.fullmatch(
                r'[ \t]*#include[ \t]+"([^"\r\n]+)"'
                r"[ \t]*(?://.*)?",
                line,
            )
            if match is None:
                fail(f"{relative}:{number} 的 xcconfig include 格式非法")
            candidate = PurePosixPath(relative).parent / PurePosixPath(
                match.group(1)
            )
            included = posixpath.normpath(str(candidate))
            validate_relative(included, "xcconfig include")
            walk(included)
        visiting.remove(relative)
        visited.add(relative)

    for relative in base_paths:
        walk(relative)


def executable_shell_lines(
    text: str,
    *,
    escaped_newlines: bool = False,
) -> tuple[str, ...]:
    lines = text.split(r"\n") if escaped_newlines else text.splitlines()
    return tuple(
        stripped
        for line in lines
        if (stripped := line.strip()) and not stripped.startswith("#")
    )


def require_ordered_commands(
    commands: tuple[str, ...],
    expected: tuple[str, ...],
    description: str,
) -> None:
    position = -1
    for command in expected:
        try:
            next_position = commands.index(command, position + 1)
        except ValueError:
            fail(f"{description} 缺少可执行命令：{command}")
        position = next_position


def validate_configurations(root: Path, project: str) -> None:
    base_paths = validate_configuration_graph(project)
    validate_xcconfig_tree(root, base_paths)
    for relative, expected in EXPECTED_CONFIG_INCLUDES.items():
        actual = config_includes(
            read_utf8(root, relative, f"{relative} 产品配置")
        )
        if actual != expected:
            fail(f"{relative} 的 include 顺序漂移")

    ordinary = read_utf8(
        root, "app/NotLinux.xcconfig", "普通 iSH 产品配置"
    )
    if unique_setting(
        ordinary, "ISH_KERNEL", "普通 iSH 产品配置"
    ) != "ish":
        fail("普通 iSH 产品配置没有显式选择 ish 内核")
    if unique_setting(
        ordinary, "NINJA_TARGETS", "普通 iSH 产品配置"
    ).split() != ["libish.a", "libish_emu.a", "libfakefs.a"]:
        fail("普通 iSH 产品配置的 Ninja 输入漂移")

    watch_tests = read_utf8(
        root, "app/WatchUITests.xcconfig", "Watch UI 测试配置"
    )
    if unique_setting(
        watch_tests, "TEST_TARGET_NAME", "Watch UI 测试配置"
    ) != "iSHWatch":
        fail("Watch UI 测试配置的被测 App 漂移")

    linux = read_utf8(root, "app/Linux.xcconfig", "Linux 产品配置")
    if unique_setting(linux, "ISH_KERNEL", "Linux 产品配置") != "linux":
        fail("Linux 产品配置没有显式选择 linux 内核")
    if unique_setting(
        linux, "NINJA_TARGETS", "Linux 产品配置"
    ).split() != ["deps/liblinux.a", "libfakefs.a", "libish_emu.a"]:
        fail("Linux 产品配置的 Ninja 输入漂移")
    if unique_setting(
        linux, "GCC_PREPROCESSOR_DEFINITIONS", "Linux 产品配置"
    ) != "ISH_LINUX=1":
        fail("Linux 产品配置的 ISH_LINUX 宏漂移")
    force_loads = re.findall(
        r"(?:^|\s)-force_load\s+(\S+)",
        unique_setting(
            linux, "LINUX_APP_LDFLAGS", "Linux 产品配置"
        ),
    )
    if force_loads != [
        "$(BUILT_PRODUCTS_DIR)/liblinux.a",
        "$(BUILT_PRODUCTS_DIR)/libiSHLinux.a",
    ]:
        fail("Linux 产品配置的强制链接边界漂移")

    meson_script = read_utf8(
        root, "app/xcode-meson.sh", "Xcode Meson 配置脚本"
    )
    meson_commands = executable_shell_lines(meson_script)
    kernel_assignments = tuple(
        command
        for command in meson_commands
        if re.match(r"^kernel=", command)
    )
    if kernel_assignments != ("kernel=$ISH_KERNEL",):
        fail("Xcode Meson 配置仍允许隐式内核选择")
    require_ordered_commands(
        meson_commands,
        (
            "if [[ ${ISH_KERNEL:-} != ish && "
            "${ISH_KERNEL:-} != linux ]]; then",
            "exit 1",
            "fi",
            "kernel=$ISH_KERNEL",
        ),
        "Xcode Meson 配置脚本",
    )


def gate_command(
    profile: str,
    role: str,
    rootfs: str,
) -> str:
    return (
        'python3 -B "$SRCROOT/tools/apple-release-profiles.py" '
        "check-build "
        f"--profile {profile} --role {role} "
        '--target "$TARGET_NAME" '
        '--configuration "$CONFIGURATION" '
        '--kernel "$ISH_KERNEL" '
        f"--rootfs {rootfs}"
    )


def pbx_string_list(
    body: str,
    name: str,
    description: str,
) -> tuple[str, ...]:
    match = re.search(
        rf"(?:^|[;\n])\s*{re.escape(name)}\s*=\s*\((.*?)\);",
        body,
        re.DOTALL,
    )
    if match is None:
        fail(f"{description}缺少 {name} 列表")
    content = match.group(1)
    item = re.compile(r'\s*("(?:\\.|[^"])*")\s*,')
    values = []
    position = 0
    while position < len(content):
        token = item.match(content, position)
        if token is None:
            if content[position:].strip():
                fail(f"{description}的 {name} 列表无法解析")
            break
        values.append(PBX.unquote(token.group(1)))
        position = token.end()
    return tuple(values)


def owned_phase(
    project: str,
    target: str,
    name: str,
) -> tuple[str, str]:
    phases, owners, _targets = PBX.phase_owners(
        project, "PBXShellScriptBuildPhase"
    )
    matches = [
        (identifier, body)
        for identifier, body in phases.items()
        if owners[identifier] == {target}
        and PBX.property_value(body, "name") == name
    ]
    if len(matches) != 1:
        fail(f"{target} 缺少唯一的 {name} phase")
    return matches[0]


def validate_build_gate(
    project: str,
    target: str,
    profile: str,
    role: str,
    rootfs: str,
) -> None:
    phase_id, phase = owned_phase(project, target, PROFILE_PHASE)
    _target_id, target_body = target_map(project)[target]
    if PBX.list_ids(target_body, "buildPhases")[0] != phase_id:
        fail(f"{target} 的发行候选 profile 门禁不是首个 App phase")
    if (
        PBX.property_value(phase, "alwaysOutOfDate") != "1"
        or PBX.property_value(phase, "buildActionMask") != "2147483647"
        or PBX.property_value(
            phase, "runOnlyForDeploymentPostprocessing"
        )
        != "0"
        or PBX.property_value(phase, "shellPath") != "/bin/sh"
        or pbx_string_list(phase, "inputPaths", target)
        != PROFILE_INPUTS
    ):
        fail(f"{target} 的发行候选 profile 门禁执行合同漂移")
    script = PBX.property_value(phase, "shellScript") or ""
    expected_script = (
        "set -eu\\n"
        + gate_command(profile, role, rootfs)
        + "\\n"
    )
    if script != expected_script:
        fail(f"{target} 的发行候选 profile 门禁脚本漂移")


def validate_rootfs_wiring(root: Path, project: str) -> None:
    _core_id, core = owned_phase(project, "iSH", "生成 AArch64 RootFS")
    if pbx_string_list(
        core,
        "inputFileListPaths",
        "普通 iSH rootfs phase",
    ) != (ROOTFS_INPUT_LIST,):
        fail("普通 iSH rootfs phase 输入清单漂移")
    core_script = PBX.property_value(core, "shellScript") or ""
    require_ordered_commands(
        executable_shell_lines(core_script, escaped_newlines=True),
        (
            "set -euo pipefail",
            '"$SRCROOT/tools/apple-aarch64-rootfs.sh" "$seed"',
        ),
        "普通 iSH rootfs phase",
    )

    _linux_id, linux = owned_phase(project, "iSH+Linux", "Download Root")
    linux_script = PBX.property_value(linux, "shellScript") or ""
    require_ordered_commands(
        executable_shell_lines(linux_script, escaped_newlines=True),
        (
            'curl -L "https://$ROOTFS_URL" -o '
            '"$BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/root.tar.gz"',
        ),
        "iSH+Linux rootfs phase",
    )

    _watch_id, watch = owned_phase(
        project, "iSHWatch", "生成 AArch64 RootFS"
    )
    if pbx_string_list(
        watch,
        "inputFileListPaths",
        "Watch rootfs phase",
    ) != (ROOTFS_INPUT_LIST,):
        fail("Watch rootfs phase 输入清单漂移")
    if pbx_string_list(
        watch,
        "inputPaths",
        "Watch rootfs phase",
    ) != ("$(SRCROOT)/tools/apple-watch-rootfs-phase.sh",):
        fail("Watch rootfs phase 包装脚本输入漂移")
    watch_script = PBX.property_value(watch, "shellScript") or ""
    require_ordered_commands(
        executable_shell_lines(watch_script, escaped_newlines=True),
        ('/bin/bash "$SRCROOT/tools/apple-watch-rootfs-phase.sh"',),
        "Watch rootfs phase",
    )
    watch_wrapper = read_utf8(
        root,
        "tools/apple-watch-rootfs-phase.sh",
        "Watch rootfs phase 包装脚本",
    )
    require_ordered_commands(
        executable_shell_lines(watch_wrapper),
        (
            "set -euo pipefail",
            '"$SRCROOT/tools/apple-aarch64-rootfs.sh" "$seed"',
        ),
        "Watch rootfs phase 包装脚本",
    )


def check_locks(root: Path) -> tuple[ProfileRow, ...]:
    load_pbx()
    rows = load_profiles(root)
    project, target_ids = validate_project_targets(root, rows)
    contracts = {}
    for row in rows:
        contract = (
            row.target,
            row.debug_configuration,
            row.release_configuration,
        )
        previous = contracts.setdefault(row.scheme, contract)
        if previous != contract:
            fail(f"{row.scheme} 在不同 profile 中声明了不同构建合同")
    for scheme, (target, _debug, _release) in contracts.items():
        row = next(item for item in rows if item.scheme == scheme)
        test_target = expected_ui_test_target(row)
        validate_scheme(
            root,
            row,
            target_ids[target],
            target_ids[test_target],
        )
    validate_screenshots_scheme(
        root,
        target_ids["iSH"],
        target_ids["iSHUITests"],
    )
    validate_configurations(root, project)
    validate_build_gate(
        project,
        "iSH",
        "core",
        "iphone-app",
        "fixed-alpine-seed",
    )
    validate_build_gate(
        project,
        "iSH+Linux",
        "with-linux",
        "iphone-app",
        "online-root-tar",
    )
    validate_build_gate(
        project,
        "iSHWatch",
        "core",
        "watch-app",
        "fixed-alpine-seed",
    )
    validate_rootfs_wiring(root, project)
    return rows


def check_build(arguments) -> None:
    rows = load_profiles(arguments.root)
    matches = [
        row
        for row in rows
        if row.profile == arguments.profile and row.role == arguments.role
    ]
    if len(matches) != 1:
        fail("构建没有匹配唯一发行候选 profile 角色")
    row = matches[0]
    if (
        arguments.target != row.target
        or arguments.configuration
        not in {row.debug_configuration, row.release_configuration}
        or arguments.kernel != row.kernel
        or arguments.rootfs != row.rootfs
    ):
        fail(
            "Xcode target、configuration、kernel 或 rootfs "
            "与发行候选 profile 不一致"
        )


class ChineseArgumentParser(argparse.ArgumentParser):
    def error(self, _message):
        self.exit(2, "错误：命令参数不完整或非法。\n")


def add_root_argument(parser) -> None:
    parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="待校验仓库根目录",
    )


def make_parser() -> argparse.ArgumentParser:
    parser = ChineseArgumentParser(
        description="校验 Apple core/with-linux 发行候选 profile"
    )
    commands = parser.add_subparsers(
        dest="command",
        required=True,
        parser_class=ChineseArgumentParser,
    )
    check = commands.add_parser(
        "check-locks",
        help="验证候选清单与 Xcode wiring",
    )
    add_root_argument(check)
    show = commands.add_parser(
        "show",
        help="显示显式指定的候选 profile",
    )
    show.add_argument("--profile", required=True)
    add_root_argument(show)
    build = commands.add_parser(
        "check-build",
        help="验证当前 Xcode 产品组合",
    )
    for name in (
        "profile",
        "role",
        "target",
        "configuration",
        "kernel",
        "rootfs",
    ):
        build.add_argument(f"--{name}", required=True)
    add_root_argument(build)
    return parser


def main() -> int:
    arguments = make_parser().parse_args()
    try:
        try:
            arguments.root = arguments.root.resolve(strict=True)
        except (OSError, RuntimeError):
            fail("待校验仓库根目录无法解析")
        if arguments.command == "check-locks":
            check_locks(arguments.root)
            print("Apple 发行候选 profile 与 Xcode wiring 校验通过")
            return 0
        if arguments.command == "show":
            rows = check_locks(arguments.root)
            selected = [
                row for row in rows if row.profile == arguments.profile
            ]
            if not selected:
                fail(f"未知 Apple 发行候选 profile：{arguments.profile}")
            print(HEADER)
            for row in selected:
                print("\t".join(row.__dict__.values()))
            return 0
        check_build(arguments)
        print(
            "Apple 构建 profile 匹配："
            f"{arguments.profile}/{arguments.role}"
        )
        return 0
    except (ProfileError, ValueError, OSError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
