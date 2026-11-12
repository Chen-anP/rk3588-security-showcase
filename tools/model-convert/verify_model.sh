#!/usr/bin/env bash
# verify_model.sh —— 在板子上比较两个模型的推理结果：已登记的模型 vs 新转换的同名模型。
#
# 为什么需要它（D13）：toolkit2 2.3.2 每次转换出的文件都有约 1300 字节不同，sha256 不能用来判断
# “这次转换和登记的一样”。改为看行为：同一个 demo、同一张图，检测结果（类别、框、置信度）逐字相同才算等价。
#
# 用法（电脑上）：
#   tools/model-convert/verify_model.sh build/models/yolo11n_640_i8_rk3588_tk232.rknn
# 选项：
#   --host 主机       ssh 主机名（默认 opi）
#   --demo-dir 目录   板子上 rknn_yolo11_demo 所在目录，相对板子上的家目录或绝对路径
#                    （默认 rksdk/rknn_model_zoo/install/rk3588_linux_aarch64/rknn_yolo11_demo）
#
# 局限：只用 demo 自带的一张图（bus.jpg）。第 5 步有了自己的回归测试图集以后，换成多张图比较。
# 退出码：0 等价；1 不等价或出错；2 用法错误
set -euo pipefail

HOST=opi
DEMO=rksdk/rknn_model_zoo/install/rk3588_linux_aarch64/rknn_yolo11_demo # ssh 登录后在家目录，相对路径即可
CAND=""

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST=${2:?--host 后面要跟主机名}; shift ;;
        --demo-dir) DEMO=${2:?--demo-dir 后面要跟目录}; shift ;;
        -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "未知选项: $1" >&2; exit 2 ;;
        *) CAND=$1 ;;
    esac
    shift
done
[ -n "$CAND" ] || { echo "要验证哪个模型？" >&2; exit 2; }
[ -f "$CAND" ] || { echo "找不到 $CAND" >&2; exit 1; }
NAME=$(basename "$CAND")
TMP="/tmp/rksec-verify-$NAME"

# 在板子上跑 demo，只留检测结果那几行（“类别 @ (x1 y1 x2 y2) 置信度”）
detect() {
    # 命令里的变量在电脑这边展开后整串交给板子执行，这是有意的
    # shellcheck disable=SC2029
    ssh "$HOST" "cd $DEMO && LD_LIBRARY_PATH=./lib ./rknn_yolo11_demo $1 model/bus.jpg" 2>&1 | grep "@" || true
}

echo "== 1. 板子上的登记模型完好"
# shellcheck disable=SC2029
ssh "$HOST" "cd ~/models && sha256sum -c models.sha256 --ignore-missing" | grep -F "$NAME" ||
    { echo "  板子上 ~/models/$NAME 没有或校验不通过"; exit 1; }

echo "== 2. 拷新模型到板子 $TMP（不放进 ~/models）"
scp -q "$CAND" "$HOST:$TMP"

echo "== 3. 同一张图分别推理"
ref=$(detect "\$HOME/models/$NAME") # \$HOME 留到板子上展开
new=$(detect "$TMP")
# shellcheck disable=SC2029
ssh "$HOST" "rm -f $TMP"
echo "  登记模型："
printf '%s\n' "$ref" | sed 's/^/    /'
echo "  新模型："
printf '%s\n' "$new" | sed 's/^/    /'

echo "== 4. 比较"
if [ -z "$ref" ]; then
    echo "  登记模型没有任何检测结果：demo 没跑起来？检查 --demo-dir"
    exit 1
fi
if [ "$ref" = "$new" ]; then
    echo "等价：$(printf '%s\n' "$ref" | wc -l) 个检测结果逐字相同（类别、框、置信度）。"
    echo "部署仍然用登记的那个文件（sha256 不变），新转换的只用来证明：现在还能转出行为一样的模型。"
else
    echo "不等价："
    diff <(printf '%s\n' "$ref") <(printf '%s\n' "$new") | sed 's/^/    /' || true
    exit 1
fi
