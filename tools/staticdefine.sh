#!/bin/bash -e
set -o pipefail

compile_commands=$1
input=$2
output=$3
dep=$4
staticdefine_header="$(cd "$(dirname "$0")" && pwd -P)/staticdefine.h"

# compile_commands.json 的 command 字段包含 shell quoting；先按 shell 语法还原为
# 参数数组，再直接启动编译器，避免源码路径中的空格被二次拆分。
python3 - "$compile_commands" "$input" "$output" "$dep" \
        "$staticdefine_header" <<'PY' |
import json
import shlex
import subprocess
import sys

compile_commands, input_path, output_path, dep_path, header_path = sys.argv[1:]
with open(compile_commands) as stream:
    commands = json.load(stream)
for command in commands:
    if command['file'].endswith('asbestos/asbestos.c'):
        break
else:
    raise SystemExit('找不到 asbestos/asbestos.c 的编译命令')

arguments = shlex.split(command['command'])
arguments = arguments[:-9] + ['-MD', '-MQ', output_path, '-MF', dep_path]
arguments += [input_path, '-include', header_path, '-S', '-o', '-']
raise SystemExit(subprocess.run(arguments, check=False).returncode)
PY
sed -ne 's:^[[:space:]]*\.ascii[[:space:]]*"\(.*\)".*:\1:;
         /^->/{s:->#\(.*\):/* \1 */:;
         s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:;
         s:->::; p;}' > $output
# sed magic was copied from the linux kernel build system
