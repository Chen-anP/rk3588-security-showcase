# 第三方来源

- `src/ui/fonts/rksec_font_*.c` 是从 Noto Sans CJK SC 生成的 LVGL 位图字体。源文件来自 Ubuntu 24.04 的 `fonts-noto-cjk 1:20230817+repack1-3`，锁定散列见 `tools/fonts/fonts.sha256`。参见 [OFL 1.1](licenses/NotoSansCJK-OFL.txt)、[软件包版权说明](licenses/NotoSansCJK-Ubuntu-copyright.txt)和[字体上游](https://github.com/notofonts/noto-cjk)。
- LVGL 在构建时按锁定版本获取。MPP、RGA、RKNN、GStreamer、libmosquitto 等由部署环境提供，遵循各自条款。
- [模型清单](models/MODELS.md)列出来源、转换方式和许可信息。本仓库发布源码、说明和合成截图。模型权重由使用者按相应条款获取。
