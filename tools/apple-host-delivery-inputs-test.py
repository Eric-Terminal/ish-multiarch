#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
VALIDATOR = TOOLS / "apple-host-delivery-inputs.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "apple_host_delivery_inputs", VALIDATOR
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载宿主交付输入校验器")
LOCKS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LOCKS
SPEC.loader.exec_module(LOCKS)


class TestFailure(Exception):
    pass


def fail(message):
    raise TestFailure(message)


def run(arguments, cwd, expect_success=True, expected_error=None):
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output = (
        result.stdout.decode("utf-8", errors="replace")
        + result.stderr.decode("utf-8", errors="replace")
    )
    if expect_success and result.returncode != 0:
        fail(f"命令意外失败：{' '.join(arguments)}\n{output}")
    if not expect_success:
        if result.returncode == 0:
            fail(f"负例意外成功：{' '.join(arguments)}")
        if "错误：" not in output or "Traceback" in output:
            fail(f"负例没有返回简洁中文错误：\n{output}")
        if expected_error and expected_error not in output:
            fail(f"负例没有命中预期错误“{expected_error}”：\n{output}")
    return output


def run_validator(root, expect_success=True, expected_error=None):
    return run(
        [
            sys.executable,
            str(VALIDATOR),
            "check-locks",
            "--root",
            str(root),
        ],
        ROOT,
        expect_success,
        expected_error,
    )


def copy_relative(source_root, destination_root, relative):
    source = source_root / relative
    destination = destination_root / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination, follow_symlinks=False)


def write_utf8(path, content):
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(content)


def production_input_paths():
    dependencies = LOCKS.parse_dependencies(ROOT)
    licenses = LOCKS.parse_license_inputs(ROOT, dependencies)
    gitlinks = LOCKS.verify_gitlinks(ROOT, dependencies)
    libarchive_sources, _product_id = LOCKS.verify_libarchive(
        ROOT, dependencies, gitlinks
    )
    hterm_inputs = LOCKS.verify_hterm(ROOT, dependencies, gitlinks)
    paths = {
        ".gitmodules",
        "app/Linux.xcconfig",
        "app/WatchApp.xcconfig",
        "deps/config.h",
        "deps/libarchive.xcodeproj/project.pbxproj",
        "iSH.xcodeproj/project.pbxproj",
        "third_party/apple-host/README.md",
        "third_party/apple-host/dependencies.tsv",
        "third_party/apple-host/license-inputs.tsv",
        "third_party/apple-host/notice-fragments.tsv",
        "third_party/apple-host/target-inputs.tsv",
    }
    paths.update(libarchive_sources)
    paths.update(hterm_inputs)
    paths.update(item.path for item in licenses)
    paths.update(
        dependency.version_source
        for dependency in dependencies.values()
        if dependency.version_source != "gitlink"
    )
    return paths


def configure_repository(repository):
    run(["git", "init", "-q"], repository)
    run(["git", "config", "user.name", "宿主输入测试"], repository)
    run(
        ["git", "config", "user.email", "host-inputs@example.invalid"],
        repository,
    )
    run(["git", "config", "commit.gpgsign", "false"], repository)


def commit_repository(repository, message):
    run(["git", "add", "-A"], repository)
    run(
        ["git", "-c", "commit.gpgsign=false", "commit", "-q", "-m", message],
        repository,
    )
    return run(["git", "rev-parse", "HEAD"], repository).strip()


def rewrite_dependency_commits(root, commits):
    path = root / "third_party/apple-host/dependencies.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    header = lines[0]
    rewritten = []
    for line in lines[1:]:
        fields = line.split("\t")
        gitlink = fields[3]
        fields[4] = commits[gitlink]
        rewritten.append("\t".join(fields))
    write_utf8(path, header + "\n" + "\n".join(rewritten) + "\n")


def build_fixture(destination, paths):
    destination.mkdir()
    for relative in sorted(paths):
        copy_relative(ROOT, destination, relative)

    commits = {}
    for relative in ("deps/libapps", "deps/libarchive", "deps/linux"):
        repository = destination / relative
        configure_repository(repository)
        commits[relative] = commit_repository(
            repository, f"建立 {relative} 合成快照"
        )
    rewrite_dependency_commits(destination, commits)

    configure_repository(destination)
    run(["git", "add", "-A"], destination)
    for relative, commit in commits.items():
        index = run(
            ["git", "ls-files", "--stage", "--", relative],
            destination,
        )
        if not index.startswith(f"160000 {commit} 0\t{relative}\n"):
            fail(f"合成父仓库没有建立正确 gitlink：{relative}")
    return commits


def worktree_digest(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if ".git" in relative.parts or not path.is_file():
            continue
        digest.update(str(relative).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def git_index_state(root):
    states = []
    for relative in (".", "deps/libapps", "deps/libarchive", "deps/linux"):
        index = root / relative / ".git/index"
        metadata = index.stat()
        states.append(
            (
                relative,
                metadata.st_mtime_ns,
                hashlib.sha256(index.read_bytes()).hexdigest(),
            )
        )
    return tuple(states)


def clone_case(base, parent, name):
    destination = parent / name
    shutil.copytree(base, destination, symlinks=True)
    return destination


def replace_once(path, old, new):
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        fail(f"测试夹具没有唯一待替换文本：{old}")
    write_utf8(path, text.replace(old, new, 1))


def case_component_gitlink_swap(root):
    path = root / "third_party/apple-host/dependencies.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    rows = {line.split("\t")[0]: line.split("\t") for line in lines[1:]}
    for index in (3, 4, 5):
        rows["hterm"][index], rows["linux"][index] = (
            rows["linux"][index],
            rows["hterm"][index],
        )
    rewritten = ["\t".join(rows[line.split("\t")[0]]) for line in lines[1:]]
    write_utf8(path, lines[0] + "\n" + "\n".join(rewritten) + "\n")


def case_target_contract_mismatch(root):
    path = root / "third_party/apple-host/target-inputs.tsv"
    replace_once(
        path,
        "iSH\tproduct\tvendored\tlibarchive\tstatic-library\tlibarchive.a\n",
        "iSH\tproduct\tvendored\tlinux-kernel\tstatic-library\tlibarchive.a\n",
    )


def append_sorted_target_row(root, row):
    path = root / "third_party/apple-host/target-inputs.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    write_utf8(path, lines[0] + "\n" + "\n".join(sorted(lines[1:] + [row])) + "\n")


def case_iphone_platform_flag(root):
    append_sorted_target_row(
        root,
        "iSH\tproduct\tplatform\tapple-sdk\tplatform-link\t-lunexpected",
    )


def case_duplicate_delivery_name(root):
    append_sorted_target_row(
        root,
        "iSH\tproduct\tplatform\tapple-sdk\tplatform-link\tlibarchive.a",
    )


def case_missing_watch_target(root):
    replace_once(
        root / "iSH.xcodeproj/project.pbxproj",
        "\t\t\tname = iSHWatch;\n",
        "\t\t\tname = iSHWatchRenamed;\n",
    )


def case_extra_watch_vendored_resource(root):
    path = root / "iSH.xcodeproj/project.pbxproj"
    marker = (
        "\t\t\t\tA1F000000000000000000002 "
        "/* THIRD-PARTY-NOTICES.txt in Resources */,\n"
    )
    extra = (
        "\t\t\t\tBB10E5C9248DBAAC009C7A74 "
        "/* libarchive.a in Resources */,\n"
    )
    replace_once(path, marker, marker + extra)


def case_libarchive_source_drift(root):
    replace_once(
        root / "deps/libarchive.xcodeproj/project.pbxproj",
        (
            "\t\t\t\tBB10E57E248DA6F4009C7A74 "
            "/* archive_entry.c in Sources */,\n"
        ),
        "",
    )


def case_libarchive_source_flags(root):
    path = root / "deps/libarchive.xcodeproj/project.pbxproj"
    original = (
        "\t\tBB10E57E248DA6F4009C7A74 "
        "/* archive_entry.c in Sources */ = {isa = PBXBuildFile; "
        "fileRef = BB10E3B5248DA6EF009C7A74 /* archive_entry.c */; };\n"
    )
    conditional = original.replace(
        "; };\n", '; settings = {COMPILER_FLAGS = "-DLOCK_DRIFT"; }; };\n'
    )
    replace_once(path, original, conditional)


def case_libarchive_product_drift(root):
    replace_once(
        root / "deps/libarchive.xcodeproj/project.pbxproj",
        (
            "\t\t\tproductReference = BB10E0D6248DA67B009C7A74 "
            "/* libarchive.a */;\n"
        ),
        (
            "\t\t\tproductReference = 000000000000000000000000 "
            "/* libarchive.a */;\n"
        ),
    )


def case_libarchive_target_isa_drift(root):
    replace_once(
        root / "deps/libarchive.xcodeproj/project.pbxproj",
        "\t\t\tisa = PBXNativeTarget;\n",
        "\t\t\tisa = PBXAggregateTarget;\n",
    )


def case_libarchive_proxy_drift(root):
    replace_once(
        root / "iSH.xcodeproj/project.pbxproj",
        (
            "\t\t\tremoteRef = BB10E5C7248DBAA1009C7A74 "
            "/* PBXContainerItemProxy */;\n"
        ),
        (
            "\t\t\tremoteRef = BB10E5CA248DBAB7009C7A74 "
            "/* PBXContainerItemProxy */;\n"
        ),
    )


def case_hterm_extra_output(root):
    path = root / "iSH.xcodeproj/project.pbxproj"
    output = (
        '\t\t\t\t"$(SRCROOT)/deps/libapps/hterm/dist/js/hterm_all.js",\n'
    )
    text = path.read_text(encoding="utf-8")
    if text.count(output) != 2:
        fail("合成工程没有两份待扩展的 hterm 输出")
    write_utf8(
        path,
        text.replace(output, output + '\t\t\t\t"$(TEMP_DIR)/extra.js",\n', 1),
    )


def case_hterm_disabled_command(root):
    path = root / "iSH.xcodeproj/project.pbxproj"
    original = (
        r'shellScript = "cd $SRCROOT/deps/libapps\n'
        r'./hterm/bin/mkdist\n";'
    )
    disabled = (
        r'shellScript = "cd $SRCROOT/deps/libapps\n'
        r'false # ./hterm/bin/mkdist\n";'
    )
    text = path.read_text(encoding="utf-8")
    if text.count(original) != 2:
        fail("合成工程没有两份待禁用的 hterm 生成命令")
    write_utf8(path, text.replace(original, disabled, 1))


def case_weak_libarchive_link(root):
    path = root / "iSH.xcodeproj/project.pbxproj"
    original = (
        "\t\tBB10E5C9248DBAAC009C7A74 "
        "/* libarchive.a in Frameworks */ = {isa = PBXBuildFile; "
        "fileRef = BB10E5C8248DBAA1009C7A74 /* libarchive.a */; };\n"
    )
    weakened = original.replace(
        "; };\n", "; settings = {ATTRIBUTES = (Weak, ); }; };\n"
    )
    replace_once(path, original, weakened)


def case_duplicate_watch_flag(root):
    path = root / "app/WatchApp.xcconfig"
    replace_once(path, " -lm -ldl ", " -lm -lm -ldl ")


def case_linux_product_drift(root):
    replace_once(
        root / "iSH.xcodeproj/project.pbxproj",
        (
            "\t\t\tproductReference = BBECF3B8269136E100DEC937 "
            "/* liblinux.a */;\n"
        ),
        (
            "\t\t\tproductReference = BBECF3A22691314C00DEC937 "
            "/* libiSHLinux.a */;\n"
        ),
    )


def case_linux_force_load_in_comment(root):
    path = root / "app/Linux.xcconfig"
    lines = path.read_text(encoding="utf-8").splitlines()
    matches = [line for line in lines if line.startswith("LINUX_APP_LDFLAGS = ")]
    if len(matches) != 1:
        fail("合成 Linux 配置没有唯一链接参数")
    original = matches[0]
    replacement = f"// {original}\nLINUX_APP_LDFLAGS = -Wl,-ld_classic"
    replace_once(path, original + "\n", replacement + "\n")


def case_gitlink_drift(root):
    path = root / "third_party/apple-host/dependencies.tsv"
    text = path.read_text(encoding="utf-8")
    old = next(
        line.split("\t")[4]
        for line in text.splitlines()[1:]
        if line.startswith("libarchive\t")
    )
    write_utf8(path, text.replace(old, "0" * 40))


def case_license_digest_drift(root):
    path = root / "third_party/apple-host/license-inputs.tsv"
    text = path.read_text(encoding="utf-8")
    row = next(
        line
        for line in text.splitlines()
        if line.startswith("hterm-bundle\thterm\tlicense\t")
    )
    fields = row.split("\t")
    fields[-1] = "0" * 64
    write_utf8(path, text.replace(row, "\t".join(fields)))


def case_notice_fragment_digest_drift(root):
    path = root / "third_party/apple-host/notice-fragments.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    fields = lines[1].split("\t")
    fields[-1] = "0" * 64
    lines[1] = "\t".join(fields)
    write_utf8(path, "\n".join(lines) + "\n")


def case_notice_fragment_range_drift(root):
    path = root / "third_party/apple-host/notice-fragments.tsv"
    replace_once(
        path,
        "archive_entry.c\t1618\t1650\t",
        "archive_entry.c\t1619\t1650\t",
    )


def case_iphone_loses_libarchive(root):
    project = root / "iSH.xcodeproj/project.pbxproj"
    line = (
        "\t\t\t\tBB10E5C9248DBAAC009C7A74 "
        "/* libarchive.a in Frameworks */,\n"
    )
    text = project.read_text(encoding="utf-8")
    if text.count(line) != 1:
        fail("无法在合成工程中唯一移除 iSH libarchive")
    write_utf8(project, text.replace(line, "", 1))


def case_watch_gains_libarchive(root):
    project = root / "iSH.xcodeproj/project.pbxproj"
    text = project.read_text(encoding="utf-8")
    marker = (
        "\t\tA1D000000000000000000004 /* Frameworks */ = {\n"
    )
    start = text.find(marker)
    if start < 0:
        fail("无法定位 Watch Frameworks phase")
    files = text.find("\t\t\tfiles = (\n", start)
    end = text.find("\t\t\t);\n", files)
    if files < 0 or end < 0:
        fail("Watch Frameworks phase 结构非法")
    insertion = (
        "\t\t\t\tBB10E5C9248DBAAC009C7A74 "
        "/* libarchive.a in Frameworks */,\n"
    )
    text = text[: end] + insertion + text[end:]
    write_utf8(project, text)


def case_hterm_nested_input_drift(root):
    concat = root / "deps/libapps/hterm/concat/hterm_deps.concat"
    line = "libdot/third_party/intl-segmenter/intl-segmenter.js\n"
    text = concat.read_text(encoding="utf-8")
    if text.count(line) != 1:
        fail("无法在合成 hterm 闭包中移除 intl-segmenter")
    write_utf8(concat, text.replace(line, "", 1))
    libapps = root / "deps/libapps"
    new_commit = commit_repository(libapps, "注入 hterm 闭包漂移")
    run(["git", "add", "deps/libapps"], root)
    dependencies = root / "third_party/apple-host/dependencies.tsv"
    locked = dependencies.read_text(encoding="utf-8")
    old_commits = {
        line.split("\t")[4]
        for line in locked.splitlines()[1:]
        if line.split("\t")[3] == "deps/libapps"
    }
    if len(old_commits) != 1:
        fail("合成依赖锁含多个 libapps 提交")
    write_utf8(
        dependencies, locked.replace(next(iter(old_commits)), new_commit)
    )


def case_linux_route_missing(root):
    path = root / "third_party/apple-host/target-inputs.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    filtered = [
        line
        for line in lines
        if "\tvendored\tlinux-kernel\tstatic-library\tliblinux.a"
        not in line
    ]
    if len(filtered) != len(lines) - 1:
        fail("无法从合成 target 路由中移除 Linux 输入")
    write_utf8(path, "\n".join(filtered) + "\n")


def case_required_license_missing(root):
    path = root / "third_party/apple-host/license-inputs.tsv"
    lines = path.read_text(encoding="utf-8").splitlines()
    filtered = [
        line
        for line in lines
        if not line.startswith("hterm-bundle\twcwidth\tlicense\t")
    ]
    if len(filtered) != len(lines) - 1:
        fail("无法从合成许可锁中移除 wcwidth")
    write_utf8(path, "\n".join(filtered) + "\n")


def case_hterm_output_path_drift(root):
    project = root / "iSH.xcodeproj/project.pbxproj"
    text = project.read_text(encoding="utf-8")
    correct = "$(SRCROOT)/deps/libapps/hterm/dist/js/hterm_all.js"
    if text.count(correct) != 2:
        fail("合成工程没有两份正确 hterm 输出路径")
    write_utf8(
        project,
        text.replace(
            correct, "$(SRCROOT)/deps/hterm/dist/js/hterm_all.js"
        ),
    )


def main():
    run_validator(ROOT)
    paths = production_input_paths()
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-host-inputs."
    ) as temporary:
        temporary_root = Path(temporary)
        base = temporary_root / "base"
        build_fixture(base, paths)
        probe = base / "deps/libarchive/COPYING"
        metadata = probe.stat()
        os.utime(
            probe,
            ns=(metadata.st_atime_ns, metadata.st_mtime_ns + 1_000_000_000),
        )
        before = worktree_digest(base)
        indexes_before = git_index_state(base)
        run_validator(base)
        after = worktree_digest(base)
        indexes_after = git_index_state(base)
        if before != after or indexes_before != indexes_after:
            fail("只读校验器改写了合成工作树或 Git 索引")

        generated = (
            base
            / "deps/libapps/hterm/dist/js/hterm_all.js"
        )
        generated.parent.mkdir(parents=True)
        write_utf8(generated, "// 合成未跟踪生成物\n")
        run_validator(base)

        cases = {
            "gitlink-drift": (
                case_gitlink_drift,
                "父仓库 gitlink 提交漂移",
            ),
            "component-gitlink-swap": (
                case_component_gitlink_swap,
                "hterm 的 gitlink 路径漂移",
            ),
            "target-contract-mismatch": (
                case_target_contract_mismatch,
                "iSH 的 vendored 输入合同漂移",
            ),
            "iphone-platform-flag": (
                case_iphone_platform_flag,
                "App 平台输入必须是 PBX 路径",
            ),
            "duplicate-delivery-name": (
                case_duplicate_delivery_name,
                "同一 target 不能重复声明同一交付名称",
            ),
            "license-digest-drift": (
                case_license_digest_drift,
                "宿主许可复核输入摘要漂移",
            ),
            "notice-fragment-digest-drift": (
                case_notice_fragment_digest_drift,
                "宿主声明片段摘要漂移",
            ),
            "notice-fragment-range-drift": (
                case_notice_fragment_range_drift,
                "宿主声明中段片段路径或范围集合漂移",
            ),
            "iphone-missing-libarchive": (
                case_iphone_loses_libarchive,
                "iSH 的 vendored 链接路由漂移",
            ),
            "watch-vendored-drift": (
                case_watch_gains_libarchive,
                "iSHWatch 的 vendored 链接路由漂移",
            ),
            "watch-target-missing": (
                case_missing_watch_target,
                "主工程缺少交付 target",
            ),
            "watch-extra-vendored-resource": (
                case_extra_watch_vendored_resource,
                "iSHWatch 的 vendored 资源路由漂移",
            ),
            "libarchive-source-drift": (
                case_libarchive_source_drift,
                "libarchive 编译源路径集合漂移",
            ),
            "libarchive-source-flags": (
                case_libarchive_source_flags,
                "libarchive 编译源不能携带单文件条件或 flags",
            ),
            "libarchive-product-drift": (
                case_libarchive_product_drift,
                "libarchive target 的静态库产品关系漂移",
            ),
            "libarchive-target-isa-drift": (
                case_libarchive_target_isa_drift,
                "PBXNativeTarget 中对象",
            ),
            "libarchive-proxy-drift": (
                case_libarchive_proxy_drift,
                "主工程没有把 libarchive 产品代理绑定到锁定子工程",
            ),
            "weak-libarchive-link": (
                case_weak_libarchive_link,
                "iSH 的 vendored 链接不能带条件或属性",
            ),
            "duplicate-watch-flag": (
                case_duplicate_watch_flag,
                "Watch App 的 OTHER_LDFLAGS 含重复项",
            ),
            "hterm-nested-drift": (
                case_hterm_nested_input_drift,
                "hterm 生成闭包缺少嵌套许可组件或 concat 输入",
            ),
            "linux-route-missing": (
                case_linux_route_missing,
                "iSH+Linux 的 vendored 链接路由漂移",
            ),
            "license-row-missing": (
                case_required_license_missing,
                "宿主许可复核输入路径集合漂移",
            ),
            "hterm-output-drift": (
                case_hterm_output_path_drift,
                "hterm 生成器、工作目录或声明输出路径漂移",
            ),
            "hterm-extra-output": (
                case_hterm_extra_output,
                "hterm 生成器、工作目录或声明输出路径漂移",
            ),
            "hterm-disabled-command": (
                case_hterm_disabled_command,
                "hterm 生成器、工作目录或声明输出路径漂移",
            ),
            "linux-product-drift": (
                case_linux_product_drift,
                "liblinux target 产品与 iSH+Linux 链接输入不一致",
            ),
            "linux-force-load-comment": (
                case_linux_force_load_in_comment,
                "iSH+Linux 的强制链接边界漂移",
            ),
        }
        for name, (mutate, expected_error) in cases.items():
            fixture = clone_case(base, temporary_root, name)
            mutate(fixture)
            run_validator(
                fixture,
                expect_success=False,
                expected_error=expected_error,
            )
    print("Apple 宿主交付输入锁合成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (TestFailure, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
