#!/usr/bin/env python3
"""convert_mobilefacenet.py —— 把人脸识别模型 MobileFaceNet（insightface buffalo_s 里的 w600k_mbf.onnx）
转成 RK3588 的 RKNN 模型（第 7c 步）。

用法（电脑上，在 setup_env.sh 装好的环境里）：
    ~/rksdk/venv/bin/python tools/model-convert/convert_mobilefacenet.py w600k_mbf.onnx \\
        --out build/models/mobilefacenet_112_fp16_rk3588_tk232.rknn

为什么默认 FP16 而不是 INT8：
  INT8 量化要一批“有代表性”的输入做校准——对人脸识别来说，就是几百张按 5 个关键点对齐好的 112×112 人脸。
  校准集不合适，特征向量会整体偏掉，识别率下降，而且不容易发现。模型很小（约 13 MB），FP16 在 NPU 上
  也只要几毫秒，所以先用 FP16 保证精度；以后有了对齐好的人脸集再试 INT8（--dtype i8 --dataset 列表文件）。

预处理（和 insightface 的 ArcFaceONNX 一样）：输入 RGB，(像素 − 127.5) / 127.5。这一步写进模型（mean/std），
板子上直接喂 uint8 RGB。

原始模型的输入是 [None, 3, 112, 112]（批大小不固定），RKNN 要固定成 1。
"""
import argparse
import hashlib
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("onnx", help="w600k_mbf.onnx")
    ap.add_argument("--out", required=True, help="输出的 .rknn 路径")
    ap.add_argument("--dtype", choices=["fp16", "i8"], default="fp16")
    ap.add_argument("--dataset", help="--dtype i8 时的校准集列表文件（每行一张对齐好的 112×112 人脸图片路径）")
    args = ap.parse_args()
    if args.dtype == "i8" and not args.dataset:
        ap.error("--dtype i8 需要 --dataset")

    import onnx  # 只用来读输入名和形状
    from rknn.api import RKNN

    model = onnx.load(args.onnx)
    inp = model.graph.input[0]
    dims = [d.dim_value if d.dim_value > 0 else d.dim_param for d in inp.type.tensor_type.shape.dim]
    print(f"== ONNX  {args.onnx}  sha256 {sha256(args.onnx)}")
    print(f"   输入 {inp.name} {dims}；输出 {[o.name for o in model.graph.output]}")

    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[127.5, 127.5, 127.5]], std_values=[[127.5, 127.5, 127.5]], target_platform="rk3588")
    if rknn.load_onnx(model=args.onnx, inputs=[inp.name], input_size_list=[[1, 3, 112, 112]]) != 0:
        sys.exit("加载 ONNX 失败")
    if rknn.build(do_quantization=args.dtype == "i8", dataset=args.dataset) != 0:
        sys.exit("转换失败")
    if rknn.export_rknn(args.out) != 0:
        sys.exit("导出失败")
    print(f"== RKNN  {args.out}  sha256 {sha256(args.out)}")


if __name__ == "__main__":
    main()
