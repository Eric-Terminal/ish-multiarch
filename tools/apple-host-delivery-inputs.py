#!/usr/bin/env python3

from __future__ import annotations

import argparse
import configparser
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys

from apple_pbx import (
    OBJECT_ID, list_ids, normalized_reference_paths, objects, phase_owners,
    property_value, target_phase_items,
)
from apple_host_manifest import (
    EXPECTED_SCOPES,
    HEX40,
    HostInputError,
    decode_utf8,
    fail,
    parse_dependencies,
    parse_license_inputs,
    parse_notice_fragments,
    parse_targets,
    path_from_root,
    read_regular,
    validate_relative,
    validate_url,
)


ROOT = Path(__file__).resolve().parent.parent
GIT_READ_ONLY_ENV = os.environ.copy()
GIT_READ_ONLY_ENV["GIT_OPTIONAL_LOCKS"] = "0"
BUILD_FILE_POLICY = re.compile(
    r"(?:^|[;\n])\s*(?:settings|platformFilter|platformFilters)\s*="
)
FIRST_PARTY_FRAMEWORKS = {
    "iSH": {"libiSHApp.a", "libfakefs.a", "libish.a", "libish_emu.a"},
    "iSH+Linux": {
        "libiSHApp.a", "libiSHLinux.a", "libiSHLinuxUser.a",
        "libfakefs.a", "libish_emu.a",
    },
    "iSHFileProvider": {"libfakefs.a", "libish_emu.a"},
    "iSHWatch": set(),
}
REQUIRED_HTERM_PATHS = {
    "deps/libapps/hterm/concat/hterm.concat",
    "deps/libapps/hterm/concat/hterm_all.concat",
    "deps/libapps/hterm/concat/hterm_deps.concat",
    "deps/libapps/hterm/concat/hterm_resources.concat",
    "deps/libapps/hterm/images/close.svg",
    "deps/libapps/hterm/images/keyboard_arrow_down.svg",
    "deps/libapps/hterm/images/keyboard_arrow_up.svg",
    "deps/libapps/libdot/js/lib_colors.js",
    "deps/libapps/libdot/third_party/intl-segmenter/intl-segmenter.js",
    "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
}


@dataclass(frozen=True)
class ValidatedHostInputs:
    dependencies: dict
    license_inputs: tuple
    notice_fragments: tuple
    gitlinks: frozenset
    libarchive_sources: frozenset
    hterm_inputs: frozenset


def run_git(root, *arguments):
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=GIT_READ_ONLY_ENV,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        fail(f"Git 检查失败：{detail or '未知错误'}")
    return result.stdout


def parse_gitmodules(root):
    data = read_regular(root, ".gitmodules", "子模块配置")
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    try:
        parser.read_string(decode_utf8(data, "子模块配置"))
    except configparser.Error as error:
        fail(f"无法解析子模块配置：{error}")
    entries = {}
    for section_name in parser.sections():
        if not section_name.startswith('submodule "') or not section_name.endswith(
            '"'
        ):
            fail("子模块配置含未知 section")
        if set(parser[section_name]) - {"path", "url", "update", "shallow"}:
            fail(f"子模块配置含未知字段：{section_name}")
        path = parser[section_name].get("path")
        url = parser[section_name].get("url")
        if not path or not url or path in entries:
            fail(f"子模块配置缺少唯一路径或来源：{section_name}")
        validate_relative(path, "子模块路径")
        validate_url(url, "子模块来源")
        entries[path] = url
    return entries


def gitlink_index_entry(root, relative):
    data = run_git(root, "ls-files", "--stage", "--", relative)
    lines = decode_utf8(data, "Git 索引").splitlines()
    suffix = "\t" + relative
    if len(lines) != 1 or not lines[0].endswith(suffix):
        fail(f"Git 索引缺少唯一 gitlink：{relative}")
    fields = lines[0][:-len(suffix)].split()
    if (
        len(fields) != 3
        or fields[0] != "160000"
        or fields[2] != "0"
        or not HEX40.fullmatch(fields[1])
    ):
        fail(f"Git 索引中的 gitlink 模式或 stage 非法：{relative}")
    return fields[1]


@lru_cache(maxsize=None)
def indexed_files(repository):
    # 同一校验会反复查询三个索引，缓存可避免大量短命 Git 进程。
    data = run_git(repository, "ls-files", "--stage")
    entries = {}
    for line in decode_utf8(data, "Git 索引").splitlines():
        if "\t" not in line:
            fail("Git 索引记录格式非法")
        metadata, relative = line.split("\t", 1)
        fields = metadata.split()
        if len(fields) != 3 or fields[2] != "0":
            fail(f"Git 索引含非零 stage：{relative}")
        if relative in entries:
            fail(f"Git 索引含重复路径：{relative}")
        entries[relative] = fields[0]
    return entries


def ensure_tracked_file(root, relative, gitlinks):
    relative_path = PurePosixPath(relative)
    for gitlink in sorted(gitlinks, key=len, reverse=True):
        gitlink_path = PurePosixPath(gitlink)
        try:
            child = relative_path.relative_to(gitlink_path)
        except ValueError:
            continue
        mode = indexed_files(root / gitlink).get(str(child))
        if mode not in {"100644", "100755"}:
            fail(f"子模块没有唯一跟踪输入：{relative}")
        return
    mode = indexed_files(root).get(relative)
    if mode not in {"100644", "100755"}:
        fail(f"仓库没有唯一跟踪输入：{relative}")


def verify_gitlinks(root, dependencies):
    by_path = {}
    for dependency in dependencies.values():
        identity = (dependency.gitlink_commit, dependency.source_url)
        previous = by_path.setdefault(dependency.gitlink_path, identity)
        if previous != identity:
            fail(f"{dependency.gitlink_path} 对应多个提交或来源")
    modules = parse_gitmodules(root)
    if modules != {path: identity[1] for path, identity in by_path.items()}:
        fail(".gitmodules 的路径或来源与宿主组件锁不一致")
    for relative, (commit, _url) in by_path.items():
        if gitlink_index_entry(root, relative) != commit:
            fail(f"父仓库 gitlink 提交漂移：{relative}")
        directory = root / relative
        try:
            metadata = directory.lstat()
        except OSError as error:
            fail(f"子模块目录不存在：{relative}（{error}）")
        if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            fail(f"子模块必须是实目录：{relative}")
        head = decode_utf8(
            run_git(directory, "rev-parse", "--verify", "HEAD"),
            "子模块 HEAD",
        ).strip()
        if head != commit:
            fail(f"子模块 HEAD 与 gitlink 不一致：{relative}")
        # hterm 的未跟踪生成物不改变锁；已跟踪源码漂移必须失败。
        dirty = run_git(directory, "status", "--porcelain=v1", "--untracked-files=no")
        if dirty:
            fail(f"子模块含已跟踪修改：{relative}")
    return set(by_path)


def read_utf8(root, relative, description):
    return decode_utf8(read_regular(root, relative, description), description)


def verify_versions(root, dependencies, gitlinks):
    expected_versions = {}
    for component in ("hterm", "libdot"):
        source = dependencies[component].version_source
        ensure_tracked_file(root, source, gitlinks)
        try:
            document = json.loads(read_utf8(root, source, "版本来源"))
            version = document["version"]
        except (json.JSONDecodeError, KeyError, TypeError):
            fail(f"{component} 的 package.json 版本格式非法")
        expected_versions[component] = version
    configure_source = dependencies["libarchive"].version_source
    ensure_tracked_file(root, configure_source, gitlinks)
    configure = read_utf8(root, configure_source, "libarchive 版本来源")
    versions = re.findall(
        r"^m4_define\(\[LIBARCHIVE_VERSION_S\],\[([0-9.]+)\]\)$",
        configure,
        re.MULTILINE,
    )
    if len(versions) != 1:
        fail("libarchive configure.ac 缺少唯一版本")
    expected_versions["libarchive"] = versions[0]
    metadata_source = dependencies["wcwidth"].version_source
    ensure_tracked_file(root, metadata_source, gitlinks)
    metadata = read_utf8(root, metadata_source, "wcwidth 版本来源")
    versions = re.findall(
        r'^\s*version:\s*"([^"]+)"\s*$',
        metadata,
        re.MULTILINE,
    )
    if len(versions) != 1:
        fail("wcwidth METADATA 缺少唯一版本")
    expected_versions["wcwidth"] = versions[0]
    expected_versions["intl-segmenter"] = "snapshot"
    expected_versions["linux"] = "snapshot"
    for component, expected in expected_versions.items():
        if dependencies[component].version != expected:
            fail(f"{component} 的锁定版本漂移")
    config = read_utf8(root, "deps/config.h", "libarchive Apple 配置")
    package_versions = re.findall(
        r'^#define PACKAGE_VERSION "([^"]+)"$',
        config,
        re.MULTILINE,
    )
    if package_versions != [dependencies["libarchive"].version]:
        fail("Apple libarchive 配置版本与上游版本锁不一致")
    ensure_tracked_file(root, "deps/config.h", gitlinks)


def digest_paths(paths):
    data = "".join(f"{path}\n" for path in sorted(paths)).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def verify_libarchive(root, dependencies, gitlinks):
    relative = "deps/libarchive.xcodeproj/project.pbxproj"
    project = read_utf8(root, relative, "libarchive Xcode 工程")
    ensure_tracked_file(root, relative, gitlinks)
    targets = objects(project, "PBXNativeTarget")
    target_by_name = {}
    for target_id, body in targets.items():
        name = property_value(body, "name")
        if name is None or name in target_by_name:
            fail("libarchive Xcode 工程的 target 名称缺失或重复")
        target_by_name[name] = (target_id, body)
    expected_names = {"libarchive", "libarchive-watchOS"}
    if set(target_by_name) != expected_names:
        fail("libarchive Xcode 工程必须恰有手机与 Watch 两个独立 target")

    file_references = objects(project, "PBXFileReference")
    reference_paths = target_phase_items(project, "PBXSourcesBuildPhase")
    dependency = dependencies["libarchive"]
    source_phase_objects = objects(project, "PBXSourcesBuildPhase")
    all_phase_ids = {}
    all_build_ids = {}
    sources_by_target = {}
    product_ids = {}
    expected_products = {
        "libarchive": ("archive", "libarchive.a"),
        "libarchive-watchOS": ("archive-watchOS", "libarchive-watchOS.a"),
    }
    phase_objects = {}
    for section_name in (
        "PBXSourcesBuildPhase",
        "PBXFrameworksBuildPhase",
        "PBXHeadersBuildPhase",
        "PBXResourcesBuildPhase",
        "PBXCopyFilesBuildPhase",
        "PBXShellScriptBuildPhase",
    ):
        if f"/* Begin {section_name} section */" in project:
            phase_objects.update(objects(project, section_name))

    for name in sorted(expected_names):
        _target_id, target = target_by_name[name]
        if (
            property_value(target, "productType")
            != "com.apple.product-type.library.static"
        ):
            fail(f"{name} target 必须生成静态库")

        target_phase_ids = list_ids(target, "buildPhases")
        if (
            not target_phase_ids
            or len(target_phase_ids) != len(set(target_phase_ids))
            or any(phase_id not in phase_objects for phase_id in target_phase_ids)
        ):
            fail(f"{name} target 的 build phase 关系漂移")
        target_build_ids = []
        for phase_id in target_phase_ids:
            target_build_ids.extend(list_ids(phase_objects[phase_id], "files"))
        if len(target_build_ids) != len(set(target_build_ids)):
            fail(f"{name} target 的 PBXBuildFile 输入重复")
        all_phase_ids[name] = set(target_phase_ids)
        all_build_ids[name] = set(target_build_ids)

        phase_ids = [
            phase_id
            for phase_id in target_phase_ids
            if phase_id in source_phase_objects
        ]
        if len(phase_ids) != 1:
            fail(f"{name} target 必须恰有一个 Sources phase")

        source_items = reference_paths.get(name)
        if source_items is None:
            fail(f"{name} target 缺少 Sources phase")
        source_paths = []
        build_ids = set()
        for path, build_id, _reference, build_body in source_items:
            if BUILD_FILE_POLICY.search(build_body):
                fail(f"{name} 编译源不能携带单文件条件或 flags")
            full_path = f"deps/{path}"
            if not full_path.endswith(".c"):
                fail(f"{name} Sources 含非 C 输入：{full_path}")
            path_from_root(root, full_path, f"{name} 编译源")
            ensure_tracked_file(root, full_path, gitlinks)
            source_paths.append(full_path)
            build_ids.add(build_id)
        if (
            len(source_paths) != len(set(source_paths))
            or len(build_ids) != len(source_paths)
        ):
            fail(f"{name} Sources 含重复输入")
        if (
            len(source_paths) != dependency.input_count
            or digest_paths(source_paths) != dependency.input_sha256
        ):
            fail(f"{name} 编译源路径集合漂移")
        sources_by_target[name] = set(source_paths)

        product_reference = property_value(target, "productReference") or ""
        target_product_ids = OBJECT_ID.findall(product_reference)
        product = (
            file_references.get(target_product_ids[0])
            if len(target_product_ids) == 1
            else None
        )
        product_name, product_path = expected_products[name]
        if (
            property_value(target, "productName") != product_name
            or product is None
            or property_value(product, "path") != product_path
            or property_value(product, "sourceTree") != "BUILT_PRODUCTS_DIR"
            or property_value(product, "explicitFileType") != "archive.ar"
        ):
            fail(f"{name} target 的静态库产品关系漂移")
        product_ids[name] = target_product_ids[0]

    if all_phase_ids["libarchive"] & all_phase_ids["libarchive-watchOS"]:
        fail("手机与 Watch libarchive 的 build phase ID 必须完全独立")
    if all_build_ids["libarchive"] & all_build_ids["libarchive-watchOS"]:
        fail("手机与 Watch libarchive 的 PBXBuildFile 必须相互独立")
    if product_ids["libarchive"] == product_ids["libarchive-watchOS"]:
        fail("手机与 Watch libarchive 必须生成独立产品")
    if sources_by_target["libarchive"] != sources_by_target["libarchive-watchOS"]:
        fail("手机与 Watch libarchive 的编译源路径集合不一致")

    configuration_lists = objects(project, "XCConfigurationList")
    configurations = objects(project, "XCBuildConfiguration")
    watch_target = target_by_name["libarchive-watchOS"][1]
    list_ids_from_target = OBJECT_ID.findall(
        property_value(watch_target, "buildConfigurationList") or ""
    )
    configuration_list = (
        configuration_lists.get(list_ids_from_target[0])
        if len(list_ids_from_target) == 1
        else None
    )
    configuration_ids = (
        list_ids(configuration_list, "buildConfigurations")
        if configuration_list is not None
        else []
    )
    watch_configurations = [
        configurations.get(identifier) for identifier in configuration_ids
    ]
    configuration_names = [
        property_value(configuration, "name")
        for configuration in watch_configurations
        if configuration is not None
    ]
    if (
        any(configuration is None for configuration in watch_configurations)
        or len(configuration_ids) != len(set(configuration_ids))
        or set(configuration_names) != {"Debug", "Release"}
        or len(configuration_names) != 2
    ):
        fail("libarchive-watchOS 必须恰有 Debug 与 Release 配置")
    for configuration in watch_configurations:
        supported = (
            property_value(configuration, "SUPPORTED_PLATFORMS") or ""
        ).split()
        excluded = (
            property_value(configuration, "EXCLUDED_SOURCE_FILE_NAMES") or ""
        ).split()
        if (
            property_value(configuration, "PRODUCT_NAME") != "archive-watchOS"
            or property_value(configuration, "SDKROOT") != "watchos"
            or len(supported) != 2
            or set(supported) != {"watchos", "watchsimulator"}
            or property_value(configuration, "TARGETED_DEVICE_FAMILY") != "4"
            or property_value(configuration, "WATCHOS_DEPLOYMENT_TARGET")
            != "10.0"
            or "filter_fork_posix.c" not in excluded
        ):
            fail("libarchive-watchOS 的平台与源码排除配置漂移")

    return sources_by_target["libarchive"], product_ids


def logical_concat_lines(text, relative):
    physical = text.splitlines()
    logical = []
    index = 0
    while index < len(physical):
        line = physical[index].strip()
        while line.endswith("\\"):
            line = line[:-1]
            index += 1
            if index >= len(physical):
                fail(f"hterm concat 续行没有后继：{relative}")
            line += physical[index].strip()
        logical.append(line)
        index += 1
    return logical


def verify_hterm(root, dependencies, gitlinks):
    libapps_root = PurePosixPath("deps/libapps")
    start = PurePosixPath(
        "deps/libapps/hterm/concat/hterm_all.concat"
    )
    inputs = set()
    visited = set()
    visiting = set()

    def resolve_from_libapps(value, description):
        candidate = libapps_root / PurePosixPath(value)
        normalized = PurePosixPath(os.path.normpath(str(candidate)))
        if normalized == libapps_root:
            fail(f"{description}不能指向 libapps 目录")
        try:
            normalized.relative_to(libapps_root)
        except ValueError:
            fail(f"{description}逃逸出 deps/libapps：{value}")
        validate_relative(str(normalized), description)
        return normalized

    def resolve_from_concat(parent, value, description):
        candidate = parent / PurePosixPath(value)
        normalized = PurePosixPath(os.path.normpath(str(candidate)))
        try:
            normalized.relative_to(libapps_root)
        except ValueError:
            fail(f"{description}逃逸出 deps/libapps：{value}")
        validate_relative(str(normalized), description)
        return normalized

    def include_file(relative, description):
        value = str(relative)
        path_from_root(root, value, description)
        ensure_tracked_file(root, value, gitlinks)
        inputs.add(value)

    def visit(relative):
        value = str(relative)
        if value in visiting:
            fail(f"hterm concat 出现 include 循环：{value}")
        if value in visited:
            return
        visiting.add(value)
        include_file(relative, "hterm concat")
        text = read_utf8(root, value, "hterm concat")
        parent = relative.parent
        for line in logical_concat_lines(text, value):
            if not line or line.startswith("#"):
                continue
            if line.startswith("@include "):
                argument = line.split(" ", 1)[1]
                visit(resolve_from_libapps(argument, "hterm include"))
                continue
            if line.startswith("@file "):
                argument = line.split(" ", 1)[1]
                include_file(
                    resolve_from_libapps(argument, "hterm 文件输入"),
                    "hterm 文件输入",
                )
                continue
            if line.startswith("@resource "):
                try:
                    _name, _mime, resource = line.split(" ", 3)[1:]
                    mode, argument = resource.split(" ", 1)
                except ValueError:
                    fail(f"hterm resource 格式非法：{line}")
                if mode == "<":
                    if argument.startswith("."):
                        resource_path = resolve_from_concat(
                            parent, argument, "hterm 文件资源"
                        )
                    else:
                        resource_path = resolve_from_libapps(
                            argument, "hterm 文件资源"
                        )
                    include_file(resource_path, "hterm 文件资源")
                elif mode == "changelog":
                    arguments = argument.split()
                    if len(arguments) == 1:
                        source = "../doc/ChangeLog.md"
                    elif len(arguments) == 2:
                        source = arguments[1]
                    else:
                        fail(f"hterm changelog resource 格式非法：{line}")
                    include_file(
                        resolve_from_concat(
                            parent, source, "hterm ChangeLog"
                        ),
                        "hterm ChangeLog",
                    )
                elif mode in {"date", "git-rev"}:
                    # 时间没有文件输入；提交身份已经由 libapps gitlink 固定。
                    pass
                elif mode == "head":
                    arguments = argument.split()
                    if len(arguments) != 2:
                        fail(f"hterm head resource 格式非法：{line}")
                    include_file(
                        resolve_from_concat(
                            parent, arguments[1], "hterm head 输入"
                        ),
                        "hterm head 输入",
                    )
                elif mode == "grep":
                    arguments = argument.split()
                    if len(arguments) != 2:
                        fail(f"hterm grep resource 格式非法：{line}")
                    include_file(
                        resolve_from_concat(
                            parent, arguments[1], "hterm grep 输入"
                        ),
                        "hterm grep 输入",
                    )
                else:
                    fail(f"hterm resource 使用未知模式：{mode}")
                continue
            if line.startswith("@"):
                fail(f"hterm concat 使用未知指令：{line}")
            include_file(
                resolve_from_libapps(line, "hterm 普通文件输入"),
                "hterm 普通文件输入",
            )
        visiting.remove(value)
        visited.add(value)

    visit(start)
    if not REQUIRED_HTERM_PATHS <= inputs:
        fail("hterm 生成闭包缺少嵌套许可组件或 concat 输入")
    contracts = {
        (
            dependency.input_count,
            dependency.input_sha256,
            dependency.delivery_kind,
            dependency.delivery_name,
        )
        for dependency in dependencies.values()
        if dependency.delivery_unit == "hterm-bundle"
    }
    if contracts != {
        (
            len(inputs),
            digest_paths(inputs),
            "generated-resource",
            "hterm_all.js",
        )
    }:
        fail("hterm 生成输入路径集合漂移")
    return inputs


def target_routes(targets, target, kinds):
    return {
        item.delivery_name
        for item in targets
        if item.target == target and item.delivery_kind in kinds
    }


def verify_main_project(
    root, targets, dependencies, gitlinks, libarchive_product_ids
):
    relative = "iSH.xcodeproj/project.pbxproj"
    project = read_utf8(root, relative, "主 Xcode 工程")
    ensure_tracked_file(root, relative, gitlinks)
    native_targets = objects(project, "PBXNativeTarget")
    target_names = [property_value(body, "name") for body in native_targets.values()]
    if (
        any(name is None for name in target_names)
        or len(target_names) != len(set(target_names))
        or not set(EXPECTED_SCOPES) <= set(target_names)
    ):
        fail("主工程缺少交付 target，或 target 名称重复")

    framework_items = target_phase_items(
        project, "PBXFrameworksBuildPhase"
    )
    resources = target_phase_items(project, "PBXResourcesBuildPhase")
    source_items = target_phase_items(project, "PBXSourcesBuildPhase")
    copy_items = target_phase_items(project, "PBXCopyFilesBuildPhase")
    for description, phase_items in (
        ("Frameworks", framework_items),
        ("Resources", resources),
        ("Sources", source_items),
        ("CopyFiles", copy_items),
    ):
        for target, items in phase_items.items():
            paths = [item[0] for item in items]
            if len(paths) != len(set(paths)):
                fail(f"{target} 的 {description} phase 含重复路径")
    vendored_routes = [
        item for item in targets if item.input_scope == "vendored"
    ]
    vendored_names = {
        dependency.delivery_name for dependency in dependencies.values()
    } | {item.delivery_name for item in vendored_routes}
    gitlink_prefixes = tuple(f"{path}/" for path in sorted(gitlinks))

    def verify_vendored_phase(items, kinds, description):
        expected_targets = {
            item.target for item in vendored_routes if item.delivery_kind in kinds
        }
        for target in set(items) | expected_targets:
            candidates = [
                item
                for item in items.get(target, [])
                if item[0] in gitlinks
                or PurePosixPath(item[0]).name in vendored_names
                or item[0].startswith(gitlink_prefixes)
            ]
            if any(BUILD_FILE_POLICY.search(item[3]) for item in candidates):
                fail(f"{target} 的 vendored {description}不能带条件或属性")
            actual = {item[0] for item in candidates}
            expected = target_routes(targets, target, kinds)
            if actual != expected:
                fail(f"{target} 的 vendored {description}路由漂移")

    # 已知 gitlink 的直接输入只能沿清单声明的 phase 进入产品。
    verify_vendored_phase(framework_items, {"static-library"}, "链接")
    verify_vendored_phase(resources, {"generated-resource"}, "资源")
    verify_vendored_phase(source_items, set(), "编译源")
    verify_vendored_phase(copy_items, set(), "复制")

    archive_products = {
        "iSH": ("libarchive.a", "libarchive"),
        "iSH+Linux": ("libarchive.a", "libarchive"),
        "iSHWatch": ("libarchive-watchOS.a", "libarchive-watchOS"),
    }
    archive_items = {
        target: [
            item
            for item in framework_items.get(target, [])
            if item[0] == product_path
        ]
        for target, (product_path, _remote_info) in archive_products.items()
    }
    if any(len(items) != 1 for items in archive_items.values()):
        fail("三个 Apple App target 必须唯一链接各自的 libarchive 产品")
    original_references = {
        archive_items[target][0][2] for target in ("iSH", "iSH+Linux")
    }
    watch_reference = archive_items["iSHWatch"][0][2]
    if len(original_references) != 1 or watch_reference in original_references:
        fail("手机与 Watch App 必须引用各自独立的 libarchive 产品代理")

    reference_proxies = objects(project, "PBXReferenceProxy")
    container_proxies = objects(project, "PBXContainerItemProxy")
    reference_paths = normalized_reference_paths(project)

    def verify_archive_proxy(reference_id, product_path, remote_info):
        archive_proxy = reference_proxies.get(reference_id)
        if (
            archive_proxy is None
            or property_value(archive_proxy, "path") != product_path
            or property_value(archive_proxy, "sourceTree")
            != "BUILT_PRODUCTS_DIR"
            or property_value(archive_proxy, "fileType") != "archive.ar"
        ):
            fail(f"主工程的 {product_path} 产品代理漂移")
        remote_ids = OBJECT_ID.findall(
            property_value(archive_proxy, "remoteRef") or ""
        )
        remote_proxy = (
            container_proxies.get(remote_ids[0])
            if len(remote_ids) == 1
            else None
        )
        portal_ids = (
            OBJECT_ID.findall(
                property_value(remote_proxy, "containerPortal") or ""
            )
            if remote_proxy is not None
            else []
        )
        if (
            remote_proxy is None
            or property_value(remote_proxy, "proxyType") != "2"
            or property_value(remote_proxy, "remoteGlobalIDString")
            != libarchive_product_ids[remote_info]
            or property_value(remote_proxy, "remoteInfo") != remote_info
            or len(portal_ids) != 1
            or reference_paths.get(portal_ids[0])
            != "deps/libarchive.xcodeproj"
        ):
            fail(f"主工程没有把 {product_path} 绑定到锁定子工程产品")

    verify_archive_proxy(
        next(iter(original_references)), "libarchive.a", "libarchive"
    )
    verify_archive_proxy(
        watch_reference, "libarchive-watchOS.a", "libarchive-watchOS"
    )

    linux_targets = [
        body
        for body in native_targets.values()
        if property_value(body, "name") == "liblinux"
    ]
    if len(linux_targets) != 1:
        fail("主工程必须有唯一 liblinux target")
    linux_target = linux_targets[0]
    linux_product_ids = OBJECT_ID.findall(
        property_value(linux_target, "productReference") or ""
    )
    linux_links = [
        item
        for item in framework_items.get("iSH+Linux", [])
        if item[0] == "liblinux.a"
    ]
    file_references = objects(project, "PBXFileReference")
    linux_product = (
        file_references.get(linux_product_ids[0])
        if len(linux_product_ids) == 1
        else None
    )
    if (
        property_value(linux_target, "productType")
        != "com.apple.product-type.library.static"
        or linux_product is None
        or property_value(linux_product, "path") != "liblinux.a"
        or property_value(linux_product, "sourceTree") != "BUILT_PRODUCTS_DIR"
        or property_value(linux_product, "explicitFileType") != "archive.ar"
        or len(linux_links) != 1
        or linux_links[0][2] != linux_product_ids[0]
    ):
        fail("liblinux target 产品与 iSH+Linux 链接输入不一致")

    for target, first_party in FIRST_PARTY_FRAMEWORKS.items():
        items = framework_items.get(target, [])
        actual = {item[0] for item in items}
        external = actual - first_party
        if any(
            BUILD_FILE_POLICY.search(item[3])
            for item in items
            if item[0] in external
        ):
            fail(f"{target} 的外部链接不能带条件或属性")
        expected = target_routes(
            targets, target, {"platform-link", "static-library"}
        )
        expected = {item for item in expected if not item.startswith("-")}
        if external != expected:
            fail(f"{target} 的外部静态链接或 Apple SDK 边界漂移")
        if actual & first_party != first_party:
            fail(f"{target} 的第一方核心链接集合漂移")

    hterm_path = "deps/libapps/hterm/dist/js/hterm_all.js"
    hterm_owners = {
        target
        for target, items in resources.items()
        if hterm_path in {item[0] for item in items}
    }
    expected_hterm_owners = {
        item.target
        for item in targets
        if item.delivery_kind == "generated-resource"
        and item.delivery_name == hterm_path
    }
    if hterm_owners != expected_hterm_owners or hterm_owners != {
        "iSH",
        "iSH+Linux",
    }:
        fail("hterm_all.js 的 Resources target 归属漂移")

    phases, owners, _ = phase_owners(
        project, "PBXShellScriptBuildPhase"
    )
    compile_phases = []
    for phase_id, body in phases.items():
        if property_value(body, "name") == "Compile JavaScript":
            compile_phases.append((body, owners[phase_id]))
    if len(compile_phases) != 2:
        fail("主工程必须有两份 Compile JavaScript phase")
    compile_owners = set()
    correct_output = "$(SRCROOT)/deps/libapps/hterm/dist/js/hterm_all.js"
    correct_script = r"cd $SRCROOT/deps/libapps\n./hterm/bin/mkdist\n"
    for body, phase_owner in compile_phases:
        if len(phase_owner) != 1:
            fail("Compile JavaScript phase 必须只属于一个 target")
        compile_owners.update(phase_owner)
        output_match = re.search(
            r"(?:^|[;\n])\s*outputPaths\s*=\s*\((.*?)\);",
            body,
            re.DOTALL,
        )
        output_body = output_match.group(1) if output_match else ""
        output_tokens = re.findall(r'"((?:\\.|[^"])*)"', output_body)
        output_remainder = re.sub(r'"(?:\\.|[^"])*"', "", output_body)
        if (
            property_value(body, "shellPath") != "/bin/sh -e"
            or property_value(body, "shellScript") != correct_script
            or output_tokens != [correct_output]
            or re.sub(r"[\s,]", "", output_remainder)
        ):
            fail("hterm 生成器、工作目录或声明输出路径漂移")
    if compile_owners != hterm_owners:
        fail("hterm 生成 phase 与 Resources target 不一致")
    if "$(SRCROOT)/deps/hterm/dist/js/hterm_all.js" in project:
        fail("主工程仍声明旧 hterm 输出路径")

    extension_owners = {
        target
        for target, items in copy_items.items()
        if "iSHFileProvider.appex" in {item[0] for item in items}
    }
    if extension_owners != {"iSH", "iSH+Linux"}:
        fail("FileProvider 嵌入产品归属漂移")

    watch = read_utf8(root, "app/WatchApp.xcconfig", "Watch App 配置")
    ensure_tracked_file(root, "app/WatchApp.xcconfig", gitlinks)
    lines = re.findall(
        r"^OTHER_LDFLAGS\s*=\s*(.+)$", watch, re.MULTILINE
    )
    if len(lines) != 1:
        fail("Watch App 配置缺少唯一 OTHER_LDFLAGS")
    actual_watch_flags = set(lines[0].split())
    if len(lines[0].split()) != len(actual_watch_flags):
        fail("Watch App 的 OTHER_LDFLAGS 含重复项")
    expected_platform_flags = target_routes(
        targets, "iSHWatch", {"platform-link"}
    )
    expected_watch_flags = expected_platform_flags | {
        "$(inherited)",
        "-Wl,-fatal_warnings",
        "-lish",
        "-lish_emu",
        "-lfakefs",
    }
    if actual_watch_flags != expected_watch_flags:
        fail("Watch App 的第一方库或 Apple SDK 链接边界漂移")
    header_search_paths = re.findall(
        r"^HEADER_SEARCH_PATHS\s*=\s*(.+)$", watch, re.MULTILINE
    )
    if len(header_search_paths) != 1 or header_search_paths[0].split() != [
        "$(inherited)",
        "$(SRCROOT)",
        "$(SRCROOT)/deps/libarchive/libarchive",
    ]:
        fail("Watch fakefs 的 libarchive 头文件搜索边界漂移")

    linux = read_utf8(root, "app/Linux.xcconfig", "Linux 产品配置")
    ensure_tracked_file(root, "app/Linux.xcconfig", gitlinks)
    def unique_xcconfig_value(name):
        matches = re.findall(
            rf"^{re.escape(name)}[ \t]*=[ \t]*(.*)$",
            linux,
            re.MULTILINE,
        )
        if len(matches) != 1:
            fail(f"Linux 产品配置缺少唯一 {name}")
        return matches[0].strip()

    if unique_xcconfig_value("ISH_KERNEL") != "linux":
        fail("iSH+Linux 的内核选择漂移")
    if unique_xcconfig_value("NINJA_TARGETS").split() != [
        "deps/liblinux.a",
        "libfakefs.a",
        "libish_emu.a",
    ]:
        fail("iSH+Linux 的内核或 Ninja 输入边界漂移")
    linux_ldflags = unique_xcconfig_value("LINUX_APP_LDFLAGS")
    if any(marker in linux_ldflags for marker in ("//", "/*", "*/", "#")):
        fail("Linux App 的链接参数不能包含注释")
    force_loads = re.findall(r"(?:^|\s)-force_load\s+(\S+)", linux_ldflags)
    if force_loads != [
        "$(BUILT_PRODUCTS_DIR)/liblinux.a",
        "$(BUILT_PRODUCTS_DIR)/libiSHLinux.a",
    ]:
        fail("iSH+Linux 的强制链接边界漂移")

    app = read_utf8(root, "app/App.xcconfig", "Apple App 配置")
    ensure_tracked_file(root, "app/App.xcconfig", gitlinks)
    link_configuration = "\n".join((project, app, linux))
    if re.search(r"(?:^|[\s,])-all_load(?=[\s,]|$)", link_configuration):
        fail("Apple App 不能强制加载整个 libarchive")
    if re.search(
        r"(?:^|[\s,])-force_load(?:\s+|,)\S*libarchive\.a(?=[\s,]|$)",
        link_configuration,
    ):
        fail("Apple App 不能强制加载整个 libarchive")


def verify_fakefs_archive_surface(root, gitlinks):
    relative = "tools/fakefs.c"
    source = read_utf8(root, relative, "fakefs 归档实现")
    ensure_tracked_file(root, relative, gitlinks)
    selectors = set(
        re.findall(
            r"\b(archive_(?:read_support_(?:filter|format)|"
            r"write_(?:add_filter|set_format))_[A-Za-z0-9_]+)\s*\(",
            source,
        )
    )
    expected = {
        "archive_read_support_filter_gzip",
        "archive_read_support_format_tar",
        "archive_write_add_filter_gzip",
        "archive_write_set_format_pax",
    }
    if selectors != expected or re.search(r"\bblake2[A-Za-z0-9_]*\s*\(", source):
        fail("fakefs 的 libarchive 格式或过滤器边界漂移")


def verify_license_inputs(
    root, inputs, gitlinks, libarchive_sources, hterm_inputs
):
    for item in inputs:
        data = read_regular(root, item.path, "宿主许可复核输入")
        ensure_tracked_file(root, item.path, gitlinks)
        if len(data) != item.size:
            fail(f"宿主许可复核输入大小漂移：{item.path}")
        if hashlib.sha256(data).hexdigest() != item.sha256:
            fail(f"宿主许可复核输入摘要漂移：{item.path}")
        if (
            item.delivery_unit == "libarchive"
            and item.role == "inline-notice"
            and item.path.endswith(".c")
            and item.path not in libarchive_sources
        ):
            fail(f"libarchive 声明输入没有进入编译源：{item.path}")
        if (
            item.delivery_unit == "hterm-bundle"
            and item.role == "inline-notice"
            and item.path not in hterm_inputs
        ):
            fail(f"hterm 声明输入没有进入生成闭包：{item.path}")


def verify_notice_fragments(root, fragments, gitlinks):
    for fragment in fragments:
        data = read_regular(root, fragment.path, "宿主声明片段来源")
        ensure_tracked_file(root, fragment.path, gitlinks)
        if b"\r" in data:
            fail(f"宿主声明片段来源必须只使用 LF：{fragment.path}")
        lines = data.splitlines(keepends=True)
        if fragment.end_line > len(lines):
            fail(f"宿主声明片段行号超出来源文件：{fragment.path}")
        selected = b"".join(
            lines[fragment.start_line - 1 : fragment.end_line]
        )
        if len(selected) != fragment.size:
            fail(f"宿主声明片段大小漂移：{fragment.path}")
        if hashlib.sha256(selected).hexdigest() != fragment.sha256:
            fail(f"宿主声明片段摘要漂移：{fragment.path}")


def verify_linux_contract(dependencies):
    dependency = dependencies["linux"]
    paths = {"deps/linux"}
    if (
        dependency.delivery_kind != "static-library"
        or dependency.delivery_name != "liblinux.a"
        or dependency.input_count != len(paths)
        or dependency.input_sha256 != digest_paths(paths)
    ):
        fail("Linux gitlink 交付合同漂移")


def check_locks(root):
    dependencies = parse_dependencies(root)
    targets = parse_targets(root, dependencies)
    inputs = parse_license_inputs(root, dependencies)
    fragments = parse_notice_fragments(root, inputs)
    gitlinks = verify_gitlinks(root, dependencies)
    verify_versions(root, dependencies, gitlinks)
    libarchive_sources, libarchive_product_ids = verify_libarchive(
        root, dependencies, gitlinks
    )
    hterm_inputs = verify_hterm(root, dependencies, gitlinks)
    verify_linux_contract(dependencies)
    verify_main_project(
        root, targets, dependencies, gitlinks, libarchive_product_ids
    )
    verify_fakefs_archive_surface(root, gitlinks)
    verify_license_inputs(
        root, inputs, gitlinks, libarchive_sources, hterm_inputs
    )
    verify_notice_fragments(root, fragments, gitlinks)
    return ValidatedHostInputs(
        dependencies,
        tuple(inputs),
        tuple(fragments),
        frozenset(gitlinks),
        frozenset(libarchive_sources),
        frozenset(hterm_inputs),
    )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="校验 Apple 宿主交付输入、许可复核点与 target 路由锁。"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    check_parser = subparsers.add_parser(
        "check-locks", help="只读校验当前宿主交付输入锁"
    )
    check_parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="待校验仓库根目录，默认使用脚本所在仓库",
    )
    arguments = parser.parse_args(argv)
    if arguments.command == "check-locks":
        root = arguments.root.resolve()
        check_locks(root)
        print("Apple 宿主交付输入锁校验通过")
        return 0
    fail("未知命令")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (HostInputError, ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
