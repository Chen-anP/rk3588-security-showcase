# 模型清单

模型文件不进 git（体积大、二进制、会频繁更新），只在这里登记**来源、转换方法和 sha256**。
拷到板子后必须校验：`cd ~/models && sha256sum -c models.sha256 --ignore-missing`。

同目录的 `models.sha256` 是机器读的登记值（转换、部署脚本用它核对），和下表必须一致；改一边就要改另一边。

## 命名规则

`模型_输入尺寸_量化_芯片_工具版本.rknn`，例如 `yolo11n_640_i8_rk3588_tk232.rknn`（ADR-05）。

## 清单

| 文件 | 用途 | 来源 | 转换 | sha256 |
|---|---|---|---|---|
| yolo11n.onnx | 人员检测（原始模型） | rknn_model_zoo v2.3.2 `examples/yolo11/model/download_model.sh` | — | 62a3751662e06c678debb54b113c211d918f245ea6d3aea0b09fc418b8fc7705 |
| yolo11n_640_i8_rk3588_tk232.rknn | 人员检测（NPU） | 由上一行转换 | `python convert.py ../model/yolo11n.onnx rk3588 i8 …`，校准集：model_zoo 自带图片，环境见 `tools/model-convert/` | cdeb014e7c6d5e32c3fa8cc986e1aa2f1916a3f8583adbfda32e35cc1d15577a |
| RetinaFace_mobile320.onnx | 人脸检测（原始模型，MobileNet0.25，320 输入） | rknn_model_zoo v2.3.2 `examples/RetinaFace/model/download_model.sh` | — | 1061ac88e7c833cf058c3e8eb50367dc5e3daadcc14967b5dede8ac4409b86fa |
| retinaface_mobile320_i8_rk3588_tk232.rknn | 人脸检测（NPU） | 由上一行转换 | `tools/model-convert/convert_retinaface.sh`（model_zoo 的 `convert.py … rk3588 i8`，校准集：model_zoo 自带的 test.jpg） | 26595703cada1bc24f60ade8bb6029b6d245eaefc7e22191c3bcf06398450d5a |
| w600k_mbf.onnx | 人脸识别（原始模型，MobileFaceNet，WebFace600K 训练，112 输入，512 维特征） | insightface v0.7 release 的 `buffalo_s.zip`（zip 的 sha256 d85a87f503f691807cd8bb97128bdf7a0660326cd9cd02657127fa978bab8b5e），解压取 `w600k_mbf.onnx` | — | 9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f |
| mobilefacenet_112_fp16_rk3588_tk232.rknn | 人脸识别（NPU） | 由上一行转换 | `tools/model-convert/convert_mobilefacenet.py`（FP16，不量化；均值、方差 127.5 写进模型；输入固定成 [1,3,112,112]） | 78a3f37802fc5809cab9bfae184247363b0596b239504c06bfa8f3fcbd9c9563 |

## 板子上的存放位置

`/usr/share/rk3588-security/models/`（可按板上部署位置调整）。
程序从配置文件的 `detect.model` 读路径（`config/rk3588-security.conf`），换位置只改配置。

## 模型输入输出（程序加载时检查，不符合就报错）

- 输入 1 个：640×640×3，uint8（RGB），NHWC；归一化在模型里
- 输出 9 个（3 个尺度 × 框、类别、分数和），NCHW，int8 非对称量化：
  `[1,64,80,80] [1,80,80,80] [1,1,80,80]`、`[1,64,40,40] …`、`[1,64,20,20] …`
- 换模型（比如自己训练的、类别不同的）时，输出格式必须一样，后处理才能用

RetinaFace（第 7b 步）：

- 输入 1 个：320×320×3，NHWC；模型里的输入是 INT8（均值 104/117/123 的归一化折算进了输入量化），程序按 uint8 RGB 交给运行库，运行库换算
- 输出 3 个，3 维：`[1,4200,4]` 框偏移（INT8）、`[1,4200,2]` 背景/人脸概率（**FP16**）、`[1,4200,10]` 5 个关键点偏移（INT8）。混着 FP16，所以用“输出转成 float”模式加载
- 均值 104/117/123 是 BGR 顺序（原始模型用 BGR 训练），官方 demo 却喂 RGB；我们和官方一致用 RGB

MobileFaceNet（第 7c 步）：

- 输入 1 个：112×112×3，uint8 RGB，按 5 个关键点对齐好的人脸（insightface 的参考点）；(像素 − 127.5) / 127.5 在模型里
- 输出 1 个：512 个 float（FP16 模型，按 float 取出），程序再归一化成长度 1
- 转换时 rknn-toolkit2 2.3.2 打印一行 `E RKNN: Unkown op target: 0`，模型照常生成。是否影响结果用
  `tools/model-convert/check_mobilefacenet.py` 确认：同一张对齐好的脸，板子 NPU 和电脑 onnxruntime 的特征余弦相似度应 ≥ 0.99
- 为什么 FP16：INT8 需要几百张对齐好的人脸做校准集，校准集不合适时识别率会悄悄下降；模型小，FP16 也只要几毫秒

## 许可

| 模型 | 许可 | 说明 |
|---|---|---|
| YOLO11（ultralytics，经 rknn_model_zoo） | AGPL-3.0 | 商用需要遵守 AGPL 或买商业许可 |
| RetinaFace（rknn_model_zoo 转发） | MIT（原始 Pytorch_Retinaface） | |
| MobileFaceNet w600k_mbf（insightface buffalo_s） | **只允许非商业研究使用** | insightface 预训练模型的许可：训练数据（WebFace）只允许学术研究。本项目是学习、演示用途；要商用必须换成能商用的识别模型或自己训练 |
