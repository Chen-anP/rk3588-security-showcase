#!/usr/bin/env bash
# check_code.sh —— 提交前自查：格式、静态检查、shell 脚本
#   tools/check_code.sh          只检查，有问题返回非 0（CI 用）
#   tools/check_code.sh --fix    自动按 .clang-format 重排代码
set -euo pipefail
cd "$(dirname "$0")/.."

FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

mapfile -t CXX_FILES < <(git ls-files '*.cpp' '*.hpp')
mapfile -t SH_FILES < <(git ls-files '*.sh')
FAIL=0

echo "== clang-format（$(clang-format --version | grep -o 'version [0-9.]*')）"
if [ "$FIX" -eq 1 ]; then
    clang-format -i "${CXX_FILES[@]}"
    echo "已重排 ${#CXX_FILES[@]} 个文件"
elif ! clang-format --dry-run --Werror "${CXX_FILES[@]}"; then
    echo "格式不对：运行 tools/check_code.sh --fix"
    FAIL=1
fi

echo "== clang-tidy"
DB=build/ci
[ -f "$DB/compile_commands.json" ] || DB=build/debug
if [ -f "$DB/compile_commands.json" ]; then
    # 只检查这次编译了的文件：没有 MPP 的机器（CI）上，mpp_jpeg_decoder.cpp 不编译，
    # 编译数据库里没有它，clang-tidy 不知道它的头文件在哪，会报“找不到头文件”
    SRC=()
    for f in $(git ls-files 'src/*.cpp'); do
        if grep -qF "/$f\"" "$DB/compile_commands.json"; then
            SRC+=("$f")
        else
            echo "跳过（这次没有编译）：$f"
        fi
    done
    clang-tidy -p "$DB" --quiet "${SRC[@]}" || FAIL=1
else
    echo "跳过：先编译一次（cmake --preset debug && cmake --build --preset debug）"
fi

echo "== shellcheck"
if command -v shellcheck >/dev/null; then
    shellcheck "${SH_FILES[@]}" || FAIL=1
else
    echo "跳过：没有安装 shellcheck"
fi

echo "== 仓库里不能有私钥"
if git grep -nE 'BEGIN (RSA |EC |OPENSSH |DSA )?PRIVATE KEY' -- . ':!tools/check_code.sh'; then
    echo "发现私钥！"
    FAIL=1
fi

[ "$FAIL" -eq 0 ] && echo "全部通过" || echo "有检查没通过"
exit "$FAIL"
