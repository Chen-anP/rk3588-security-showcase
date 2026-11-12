#!/usr/bin/env bash
# setup_env.sh —— 在电脑上（WSL2 Ubuntu 22.04，x86_64）建模型转换环境，版本全部锁定（ADR-02、ADR-05）。
#
# 用法：
#   tools/model-convert/setup_env.sh --whl 路径/rknn_toolkit2-2.3.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
# 选项：
#   --venv 目录   虚拟环境放哪（默认 ~/rksdk/venv）；已存在就在原环境上补齐，可以重复运行
#
# 做了什么：
#   1. 检查 x86_64 + Python 3.10（toolkit2 的 whl 是 cp310 的）
#   2. 核对 whl 的 sha256
#   3. 先按 requirements.lock 装精确版本，再用 --no-deps 装 toolkit2
#      （反过来的话，pip 会按 toolkit2 自己的要求 onnx>=1.16.1 装到不兼容的新版本，见 D06）
#   4. 导入测试；pip freeze 和锁定文件逐行对比，有任何不同就失败
# 退出码：0 成功；1 失败；2 用法错误
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WHL_SHA256=6cb783ddf293ac509f39bf9127acf6a5492bbb67e4b4b4ac33a7c6d2cefb4f3c
VENV="$HOME/rksdk/venv"
WHL=""

while [ $# -gt 0 ]; do
    case "$1" in
        --whl) WHL=${2:?--whl 后面要跟文件}; shift ;;
        --venv) VENV=${2:?--venv 后面要跟目录}; shift ;;
        -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知选项: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$WHL" ] || { echo "需要 --whl（在 rknn-toolkit2 仓库 v2.3.2 的 rknn-toolkit2/packages/x86_64/ 下）" >&2; exit 2; }

echo "== 1. 平台"
[ "$(uname -m)" = x86_64 ] || { echo "需要 x86_64，当前 $(uname -m)（板子上不转换模型，ADR-02）"; exit 1; }
py=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
[ "$py" = 3.10 ] || { echo "需要 Python 3.10，当前 $py"; exit 1; }
echo "  x86_64，Python $py"

echo "== 2. 核对 whl"
have=$(sha256sum "$WHL" | awk '{print $1}')
[ "$have" = "$WHL_SHA256" ] || { echo "  sha256 不一致：$have（应为 $WHL_SHA256）"; exit 1; }
echo "  sha256 一致"

echo "== 3. 安装（$VENV）"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install --disable-pip-version-check -q -r "$HERE/requirements.lock"
"$VENV/bin/pip" install --disable-pip-version-check -q --no-deps "$WHL"

echo "== 4. 验证"
"$VENV/bin/python" -c 'from rknn.api import RKNN; print("  导入 rknn.api 成功")'
# pip freeze 里 toolkit2 那一行是本地 whl 路径，不进锁定文件，比较时去掉
DIFF=$(mktemp)
trap 'rm -f "$DIFF"' EXIT
if diff <(sort "$HERE/requirements.lock") \
    <("$VENV/bin/pip" freeze --disable-pip-version-check | grep -vi '^rknn' | sort) >"$DIFF"; then
    echo "  pip freeze 与 requirements.lock 完全一致"
else
    echo "  pip freeze 与 requirements.lock 不一致（< 锁定文件，> 实际环境）："
    sed 's/^/    /' "$DIFF"
    exit 1
fi
echo "完成。使用：source $VENV/bin/activate"
