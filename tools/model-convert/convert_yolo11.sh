#!/usr/bin/env bash
# convert_yolo11.sh —— 把 YOLO11n 的 ONNX 转成 RK3588 的 INT8 RKNN 模型，并和登记的 sha256 对比。
#
# 用法（电脑上，先跑过 setup_env.sh）：
#   tools/model-convert/convert_yolo11.sh --zoo ~/rksdk/rknn_model_zoo
# 选项：
#   --venv 目录   转换环境（默认 ~/rksdk/venv）
#   --out 目录    输出目录（默认仓库的 build/models/，不进 git）
#
# 做了什么：
#   1. 检查 rknn_model_zoo 是锁定的 v2.3.2（commit bad6c73）
#   2. 没有 yolo11n.onnx 就用 model_zoo 的脚本下载；核对 sha256（models/models.sha256）
#   3. 用 model_zoo 的 convert.py 转换（INT8，校准集是 model_zoo 自带图片）
#   4. 输出文件名按 ADR-05 命名，算 sha256，和 models/models.sha256 登记的值对比
#
# 注意（D13）：toolkit2 2.3.2 每次转换，文件里有两小块（约 1300 字节）都不一样，像是压缩过的元数据，
# 所以 sha256 基本不会和登记值一致。实测这时推理结果和登记的模型逐字相同——权重没变。
# 因此 sha256 不一致不算失败，而是要接着用 verify_model.sh 在板子上比较推理结果。
# 另外，OMP_NUM_THREADS 这类线程设置会改变量化结果（实测单线程转出的模型框和置信度有小差别），
# 所以转换时把这些变量清掉，用和登记时一样的默认设置。
#
# 退出码：0 逐字节一致；3 字节不同、需要在板子上验证推理结果；1 失败；2 用法错误
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ZOO_COMMIT=bad6c7334531becaf90a561988519b7bec34d0ab
NAME=yolo11n_640_i8_rk3588_tk232.rknn
REGISTRY="$REPO/models/models.sha256"
VENV="$HOME/rksdk/venv"
OUT="$REPO/build/models"
ZOO=""

while [ $# -gt 0 ]; do
    case "$1" in
        --zoo) ZOO=${2:?--zoo 后面要跟目录}; shift ;;
        --venv) VENV=${2:?--venv 后面要跟目录}; shift ;;
        --out) OUT=${2:?--out 后面要跟目录}; shift ;;
        -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知选项: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$ZOO" ] || { echo "需要 --zoo（rknn_model_zoo v2.3.2 的目录）" >&2; exit 2; }
registered() { awk -v f="$1" '$2 == f {print $1}' "$REGISTRY"; }

echo "== 1. rknn_model_zoo"
have=$(git -C "$ZOO" rev-parse HEAD)
[ "$have" = "$ZOO_COMMIT" ] || { echo "  commit 是 $have，应为 $ZOO_COMMIT（v2.3.2）"; exit 1; }
echo "  v2.3.2（${have:0:7}）"

echo "== 2. 原始模型 yolo11n.onnx"
MODEL_DIR="$ZOO/examples/yolo11/model"
if [ ! -f "$MODEL_DIR/yolo11n.onnx" ]; then
    (cd "$MODEL_DIR" && bash download_model.sh) # 仓库里这个脚本没有执行权限（D05），用 bash 运行
fi
onnx_sha=$(sha256sum "$MODEL_DIR/yolo11n.onnx" | awk '{print $1}')
[ "$onnx_sha" = "$(registered yolo11n.onnx)" ] || { echo "  sha256 $onnx_sha 和登记值不一致"; exit 1; }
echo "  sha256 一致"

echo "== 3. 转换"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
echo "  CPU $(nproc) 核；清掉线程相关的环境变量，用默认设置（D13）"
start=$(date +%s)
(cd "$ZOO/examples/yolo11/python" &&
    env -u OMP_NUM_THREADS -u MKL_NUM_THREADS -u OPENBLAS_NUM_THREADS -u PYTHONHASHSEED \
        "$VENV/bin/python" convert.py ../model/yolo11n.onnx rk3588 i8 "$OUT/$NAME")
echo "  用时 $(($(date +%s) - start)) 秒，输出 $OUT/$NAME（$(du -h "$OUT/$NAME" | cut -f1)）"

echo "== 4. 和登记值对比"
got=$(sha256sum "$OUT/$NAME" | awk '{print $1}')
want=$(registered "$NAME")
echo "  本次  $got"
echo "  登记  ${want:-（没有登记）}"
if [ "$got" = "$want" ]; then
    echo "逐字节一致。"
else
    echo "字节不同。toolkit2 2.3.2 每次转换都会这样（D13），不代表模型变了。下一步在板子上比较推理结果："
    echo "  tools/model-convert/verify_model.sh $OUT/$NAME"
    exit 3
fi
