# 实测环境

| 项目 | 环境 |
| --- | --- |
| 开发板 | Orange Pi 5 Ultra，RK3588，16 GB RAM |
| 系统 | Orange Pi OS Ubuntu Jammy 22.04，6.1.99-rockchip-rk3588 |
| 推理 | librknnrt 2.3.2，NPU 驱动 0.9.8 |
| 采集 | USB UVC，MJPG 1280×720，支持 DMA-BUF |
| 界面 | LVGL v9.2.2，通过 DRM/KMS 输出 |
| 网络 | GStreamer RTSP，libmosquitto 2.0.11 |

编译与模型部署入口见 [README](../README.md) 和 [模型清单](../models/MODELS.md)。短测配置见 [性能记录](bench.md)。
