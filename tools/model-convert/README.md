# 模型转换环境

在开发机上转换，板子只部署 `.rknn`（ADR-02）。

| 项目 | 值 |
|---|---|
| 系统 | WSL2 Ubuntu 22.04（x86_64） |
| Python | 3.10，虚拟环境 |
| rknn-toolkit2 | 2.3.2，whl：`rknn_toolkit2-2.3.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl` |
| whl sha256 | 6cb783ddf293ac509f39bf9127acf6a5492bbb67e4b4b4ac33a7c6d2cefb4f3c |
| 仓库 | airockchip/rknn-toolkit2 v2.3.2（commit 42aa1d4），airockchip/rknn_model_zoo v2.3.2（commit bad6c73） |

## 用法

四个脚本，都在电脑（WSL）上运行，可以重复运行：

```bash
# 1. 建转换环境：核对 whl 的 sha256，按 requirements.lock 装精确版本，最后逐行对比 pip freeze
tools/model-convert/setup_env.sh --whl ~/rksdk/rknn-toolkit2/rknn-toolkit2/packages/x86_64/rknn_toolkit2-2.3.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl

# 2. 转换：检查 model_zoo 版本和 onnx 的 sha256，转换后和 models/models.sha256 对比
tools/model-convert/convert_yolo11.sh --zoo ~/rksdk/rknn_model_zoo

# 2b. 字节不同时（toolkit2 2.3.2 基本每次都这样，D13），在板子上比较新旧模型的推理结果
tools/model-convert/verify_model.sh build/models/yolo11n_640_i8_rk3588_tk232.rknn

# 3. 部署：只部署登记过且校验一致的模型，拷到板子 ~/models/ 后在板子上再校验一次
tools/model-convert/deploy_model.sh build/models/yolo11n_640_i8_rk3588_tk232.rknn
```

安装顺序有讲究：先 `pip install -r requirements.lock`，再 `pip install --no-deps` 装 toolkit2 的 whl。
反过来的话，pip 会按 toolkit2 自己的要求 `onnx>=1.16.1` 装到不兼容的 1.22.0（D06）。

`requirements.lock` 由 `pip freeze` 生成（2026-10-27），去掉了 rknn-toolkit2 那一行（原来是本机绝对路径）。

**转换不能逐字节复现（D13）**：toolkit2 2.3.2 每次转换，文件里约 1300 字节不同（两小块像压缩元数据的区域），
但推理结果和登记的模型逐字相同。所以判断“转换没变”看推理结果，不看 sha256；部署永远用登记的那个文件。
`OMP_NUM_THREADS` 等线程变量会改变量化结果，转换脚本会清掉它们。

模型的登记值在 `models/models.sha256`（机器读）和 `models/MODELS.md`（人读），两边要一致。
