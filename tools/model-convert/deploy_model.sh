#!/usr/bin/env bash
# deploy_model.sh —— 把转换好的模型拷到板子，并在板子上校验 sha256。
#
# 用法（电脑上）：
#   tools/model-convert/deploy_model.sh build/models/yolo11n_640_i8_rk3588_tk232.rknn
# 选项：
#   --host 主机   ssh 主机名（默认 opi，即 ~/.ssh/config 里配的板子）
#
# 只部署在 models/models.sha256 里登记过、而且校验值一致的模型：没登记的模型不知道从哪来，不能上板。
# 退出码：0 成功；1 失败；2 用法错误
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REGISTRY="$HERE/../../models/models.sha256"
HOST=opi
FILES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST=${2:?--host 后面要跟主机名}; shift ;;
        -h | --help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "未知选项: $1" >&2; exit 2 ;;
        *) FILES+=("$1") ;;
    esac
    shift
done
[ "${#FILES[@]}" -gt 0 ] || { echo "要部署哪个模型？" >&2; exit 2; }

echo "== 1. 本地核对登记"
for f in "${FILES[@]}"; do
    name=$(basename "$f")
    want=$(awk -v n="$name" '$2 == n {print $1}' "$REGISTRY")
    got=$(sha256sum "$f" | awk '{print $1}')
    [ -n "$want" ] || { echo "  $name 没有在 models/models.sha256 登记，拒绝部署"; exit 1; }
    [ "$got" = "$want" ] || { echo "  $name sha256 $got 和登记值 $want 不一致，拒绝部署"; exit 1; }
    echo "  $name 一致"
done

echo "== 2. 拷到 $HOST:~/models/"
ssh "$HOST" "mkdir -p ~/models"
scp -q "${FILES[@]}" "$REGISTRY" "$HOST:models/"

echo "== 3. 板子上校验"
ssh "$HOST" "cd ~/models && sha256sum -c models.sha256 --ignore-missing"
echo "完成"
