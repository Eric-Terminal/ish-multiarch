#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
source "$ROOT/tools/reproducible-build-env.sh"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/ish-source-epoch.XXXXXX")
cleanup() {
    /usr/bin/find "$test_root" -depth -delete
}
trap cleanup EXIT
empty_source="$test_root/empty"
outer_repository="$test_root/outer"
nested_source="$outer_repository/source"
source_repository="$test_root/source"
fixture_home="$test_root/home"
fixture_config="$test_root/config"
mkdir -p "$empty_source" "$fixture_home" "$fixture_config"

SOURCE_DATE_EPOCH=123456789
ZERO_AR_DATE=9
ish_reproducible_build_environment "$empty_source"
[[ "$SOURCE_DATE_EPOCH" == 123456789 && "$ZERO_AR_DATE" == 1 ]]

if (SOURCE_DATE_EPOCH=invalid
        ish_reproducible_build_environment "$empty_source" >/dev/null 2>&1); then
    echo "错误：非法 SOURCE_DATE_EPOCH 应被拒绝。" >&2
    exit 1
fi

(
    PATH=/nonexistent
    unset SOURCE_DATE_EPOCH ZERO_AR_DATE
    ish_reproducible_build_environment "$empty_source"
    [[ "$SOURCE_DATE_EPOCH" == 0 && "$ZERO_AR_DATE" == 1 ]]
)

git_executable=$(command -v git || true)
if [[ -n "$git_executable" ]]; then
    git_command=(env -i
        "GIT_CONFIG_NOSYSTEM=1"
        "GIT_CONFIG_GLOBAL=/dev/null"
        "HOME=$fixture_home"
        "XDG_CONFIG_HOME=$fixture_config"
        "PATH=${PATH:-/usr/bin:/bin}"
        "LC_ALL=C")
    if [[ -n ${DEVELOPER_DIR:-} ]]; then
        git_command+=("DEVELOPER_DIR=$DEVELOPER_DIR")
    fi
    git_command+=("$git_executable" --no-replace-objects)

    mkdir -p "$source_repository" "$nested_source"
    "${git_command[@]}" -C "$source_repository" init -q
    "${git_command[@]}" -C "$outer_repository" init -q

    commit_fixture() {
        local repository=$1
        local commit_date=$2
        local -a commit_command

        commit_command=(env -i
            "GIT_CONFIG_NOSYSTEM=1"
            "GIT_CONFIG_GLOBAL=/dev/null"
            "HOME=$fixture_home"
            "XDG_CONFIG_HOME=$fixture_config"
            "PATH=${PATH:-/usr/bin:/bin}"
            "LC_ALL=C"
            "GIT_AUTHOR_DATE=$commit_date"
            "GIT_COMMITTER_DATE=$commit_date")
        if [[ -n ${DEVELOPER_DIR:-} ]]; then
            commit_command+=("DEVELOPER_DIR=$DEVELOPER_DIR")
        fi
        commit_command+=("$git_executable" --no-replace-objects)
        "${commit_command[@]}" -C "$repository" \
            -c user.name=fixture -c user.email=fixture@example.invalid \
            -c commit.gpgsign=false -c core.hooksPath=/dev/null \
            commit --allow-empty -qm fixture
    }
    commit_fixture "$source_repository" "2001-09-09T01:46:40Z"
    commit_fixture "$outer_repository" "2005-03-18T01:58:31Z"

    expected_epoch=$("${git_command[@]}" -C "$source_repository" \
        log -1 --format=%ct HEAD)
    outer_epoch=$("${git_command[@]}" -C "$outer_repository" \
        log -1 --format=%ct HEAD)
    [[ "$expected_epoch" != "$outer_epoch" ]]
    unset SOURCE_DATE_EPOCH ZERO_AR_DATE
    ish_reproducible_build_environment "$source_repository"
    [[ "$SOURCE_DATE_EPOCH" == "$expected_epoch" &&
            "$ZERO_AR_DATE" == 1 ]]

    unset SOURCE_DATE_EPOCH ZERO_AR_DATE
    ish_reproducible_build_environment "$nested_source"
    [[ "$SOURCE_DATE_EPOCH" == 0 && "$ZERO_AR_DATE" == 1 ]]

    unset SOURCE_DATE_EPOCH ZERO_AR_DATE
    export GIT_DIR="$outer_repository/.git"
    export GIT_WORK_TREE="$source_repository"
    ish_reproducible_build_environment "$source_repository"
    unset GIT_DIR GIT_WORK_TREE
    [[ "$SOURCE_DATE_EPOCH" == "$expected_epoch" &&
            "$ZERO_AR_DATE" == 1 ]]
fi
