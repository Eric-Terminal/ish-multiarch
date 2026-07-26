from pathlib import PurePosixPath
import posixpath
import re


OBJECT_ID = re.compile(r"\b[A-F0-9]{24}\b")


def fail(message):
    raise ValueError(message)


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
        if property_value(body, "isa") != section_name:
            fail(f"{section_name} 中对象 {identifier} 的 isa 漂移")
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


def normalized_reference_paths(project):
    file_references = objects(project, "PBXFileReference")
    references = dict(file_references)
    for section_name in (
        "PBXReferenceProxy",
        "PBXVariantGroup",
        "XCVersionGroup",
    ):
        begin = f"/* Begin {section_name} section */"
        end = f"/* End {section_name} section */"
        if begin not in project and end not in project:
            continue
        additional = objects(project, section_name)
        duplicates = set(references) & set(additional)
        if duplicates:
            fail(f"Xcode 工程跨 section 重复对象：{sorted(duplicates)[0]}")
        references.update(additional)
    resolved = resolve_file_references(project, references)
    paths = {}
    for relative, identifiers in resolved.items():
        normalized = posixpath.normpath(relative)
        if normalized == ".." or normalized.startswith("../"):
            fail("Xcode 工程引用逃逸出源码根目录")
        for identifier in identifiers:
            previous = paths.setdefault(identifier, normalized)
            if previous != normalized:
                fail(f"Xcode 文件引用 {identifier} 对应多个路径")
    for identifier, body in references.items():
        if identifier in paths:
            continue
        value = property_value(body, "path") or property_value(body, "name")
        if value:
            paths[identifier] = value
    return paths


def target_phase_items(project, phase_section):
    build_files = objects(project, "PBXBuildFile")
    paths = normalized_reference_paths(project)
    phases, owners, _ = phase_owners(project, phase_section)
    items = {}
    for phase_id, body in phases.items():
        for target in owners[phase_id]:
            target_items = items.setdefault(target, [])
            for build_id in list_ids(body, "files"):
                build_body = build_files.get(build_id)
                if build_body is None:
                    fail(f"{phase_section} 引用了未知 build file：{build_id}")
                reference = build_file_reference(build_body)
                if reference is None or reference not in paths:
                    fail(f"无法解析 build file {build_id} 的文件路径")
                target_items.append(
                    (paths[reference], build_id, reference, build_body)
                )
    return items
