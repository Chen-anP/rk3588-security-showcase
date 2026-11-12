#!/usr/bin/env python3
"""check_mobilefacenet.py —— 对照板子 NPU 和电脑上 onnxruntime 算出来的人脸特征（第 7c 步）。

转换模型（FP16）、板子上的预处理（RGB、均值方差）有没有出错，看最后的识别率很难分辨；
直接拿同一张对齐好的 112×112 人脸，两边各算一次特征，余弦相似度应该接近 1（≥ 0.99）。

用法：
    # 板子上：--face-embed 存对齐后的脸（*.aligned.ppm）和特征（*.emb.txt）
    rk3588-security -c … --face-embed a.jpg b.jpg --save /tmp/emb
    # 拷到电脑上
    scp -r orangepi@板子:/tmp/emb /tmp/
    ~/rksdk/venv/bin/python tools/model-convert/check_mobilefacenet.py w600k_mbf.onnx /tmp/emb/*.aligned.ppm

onnxruntime 这边的预处理按 insightface 的 ArcFaceONNX：RGB，(像素 − 127.5) / 127.5，NCHW。
全部 ≥ 0.99 返回 0，否则返回 1。
"""
import argparse
import sys

import numpy as np


def read_ppm(path):
    """P6 格式（--face-embed 存的）：文字头 + RGB 字节"""
    with open(path, "rb") as f:
        data = f.read()
    fields = []
    pos = 0
    while len(fields) < 4:
        while data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end : end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1  # 头后面的一个空白字符
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path} 不是 8 位 P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data[pos : pos + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("onnx", help="w600k_mbf.onnx")
    ap.add_argument("ppm", nargs="+", help="板子 --face-embed --save 存的 *.aligned.ppm（同名的 .emb.txt 放在旁边）")
    args = ap.parse_args()

    import onnxruntime as ort

    sess = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name
    worst = 1.0
    onnx_embs = []
    for ppm in args.ppm:
        img = read_ppm(ppm)
        if img.shape != (112, 112, 3):
            sys.exit(f"{ppm} 应该是 112×112，实际 {img.shape[1]}×{img.shape[0]}")
        x = ((img.astype(np.float32) - 127.5) / 127.5).transpose(2, 0, 1)[None]
        e = sess.run(None, {name: x})[0][0]
        e = e / np.linalg.norm(e)
        onnx_embs.append(e)
        txt = ppm[: -len(".aligned.ppm")] + ".emb.txt" if ppm.endswith(".aligned.ppm") else ppm + ".emb.txt"
        board = np.loadtxt(txt, dtype=np.float32)
        board = board / np.linalg.norm(board)
        cos = float(np.dot(e, board))
        worst = min(worst, cos)
        print(f"{ppm}\n    板子 NPU ↔ onnxruntime 余弦相似度 {cos:.4f}{'' if cos >= 0.99 else '  ← 偏低'}")
    if len(onnx_embs) >= 2:
        print("onnxruntime 算的两两相似度（和板子 --face-embed 打印的对照）：")
        for i in range(len(onnx_embs)):
            for j in range(i + 1, len(onnx_embs)):
                print(f"    [{i + 1}] ↔ [{j + 1}]  {float(np.dot(onnx_embs[i], onnx_embs[j])):.3f}")
    print(f"最低 {worst:.4f}：{'通过' if worst >= 0.99 else '不通过（检查转换参数、预处理的颜色顺序和均值方差）'}")
    return 0 if worst >= 0.99 else 1


if __name__ == "__main__":
    sys.exit(main())
