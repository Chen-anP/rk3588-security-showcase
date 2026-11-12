#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ini.hpp"
#include "core/log.hpp"

// 程序配置：从 INI 读取并逐项校验。
// 原则：配置写错就报错退出，不悄悄用默认值；一次报出全部错误（带行号）；拼错的键名也算错误。
namespace rksec::config {

struct Camera {
    std::string device;      // 必填；用 /dev/v4l/by-id/ 固定路径（D04）
    int width = 1280;        // 160～1920
    int height = 720;        // 120～1080
    int fps = 30;            // 请求帧率 1～60；实际帧率随光线变化（D03）
    int power_line_hz = 50;  // 防闪烁：50 / 60 / 0（关闭）
    int buffers = 4;         // 驱动缓冲区个数 2～16（第 3 步）
};

// 显示和触摸（第 4 步）
struct Display {
    int idle_timeout_seconds = 20;  // 0 保持屏幕常亮

    std::string device = "auto";     // "auto"（按驱动名选 rockchip 显示设备）或 /dev/dri/cardN
    std::string connector = "auto";  // "auto"（第一个连了屏幕的输出口）或输出口名，比如 HDMI-A-1
    std::string touch;               // 触摸屏的 /dev/input/by-id/ 路径；空 = 不用触摸
};

// 人员检测（第 5d、6 步）
struct Detect {
    bool enabled = true;  // false：只有预览，不检测
    // 模型文件（不进 git，登记在 models/MODELS.md）。默认值是第 11 步打包安装后的位置
    std::string model = "/usr/share/rk3588-security/models/yolo11n_640_i8_rk3588_tk232.rknn";
    int workers = 1;                             // 同时推理的 NPU 上下文个数 1～3，每个绑一个 NPU 核
    float score_threshold = 0.25F;               // 0.05～0.95
    float nms_threshold = 0.45F;                 // 0.1～0.9
    std::vector<std::string> classes{"person"};  // 要的类别（COCO 英文名），逗号分隔
};

// 人脸（第 7 步）
struct Face {
    // 人脸检测模型（RetinaFace，rknn_model_zoo）。默认值是第 11 步打包安装后的位置
    std::string detect_model = "/usr/share/rk3588-security/models/retinaface_mobile320_i8_rk3588_tk232.rknn";
    float detect_threshold = 0.5F;  // 人脸概率阈值 0.1～0.99
    // 人脸识别（第 7c 步）。false：只检测人，不认人
    bool recognize = true;
    // 识别模型（insightface MobileFaceNet w600k_mbf，FP16）
    std::string recognize_model = "/usr/share/rk3588-security/models/mobilefacenet_112_fp16_rk3588_tk232.rknn";
    float match_threshold = 0.4F;  // 相似度达到多少算认出来 0.1～0.95（要用实际照片校准，见第 7c 步记录）
    // 人脸库（SQLite，第 7d 步）：--enroll 登记，启动时读进内存。没有这个文件就新建一个空的
    std::string database = "/var/lib/rk3588-security/faces.db";
};

// 视频编码（第 8a 步）：给录像和 RTSP 用的 H.264 码流
struct Encode {
    bool enabled = true;      // false：不编码（没有录像、没有 RTSP）
    int bitrate_kbps = 4000;  // 目标码率（CBR），200～20000
    int fps = 25;             // 码率控制按这个帧率算每帧预算 5～60
    int gop = 50;  // 每隔多少帧一个 IDR 关键帧 5～300（录像只能从关键帧剪、RTSP 新客户端等关键帧）
};

// 事件录像（第 8b 步）：陌生人出现时录一段 MP4，记进事件表（和人脸库同一个数据库文件 face.database）
struct Record {
    bool enabled = true;  // false：不录（也要 encode.enabled 和 face.recognize 都开着才会录）
    std::string dir = "/var/lib/rk3588-security/events";  // 录像目录（权限 0700）
    int pre_seconds = 5;  // 预录：陌生人出现前多少秒 1～30（实际从更早的关键帧开始，多 0～1 个 GOP）
    int post_seconds = 10;  // 陌生人消失多久后结束 1～120
    int max_seconds = 300;  // 一个文件最长多久，超过切成下一个文件 30～3600
    int quota_mb = 2048;    // 录像总大小上限，超过删最旧的 64～1000000
};

// RTSP 实时流（第 8c 步）
struct Rtsp {
    bool enabled = true;              // false：不开（要 encode.enabled 才能开）
    std::string address = "0.0.0.0";  // 监听地址（IPv4）
    int port = 8554;                  // 1024～65535
    std::string path = "/live";       // rtsp://板子:端口/路径
    // 账号文件：一行“用户名:密码”，权限必须 600；没有这个文件就不开 RTSP（不提供不要密码的流）
    std::string credentials = "/etc/rk3588-security/rtsp_credentials";
};

struct Log {
    log::Level level = log::Level::info;
};

struct Pir {
    std::string chip = "/dev/gpiochip1";
    int line = 7;
    int active_level = 1;
};

struct Access {
    bool enabled = false;
};

struct Mqtt {
    bool enabled = false;
    std::string host = "localhost";
    int port = 8883;
    std::string device_id;
    std::string credentials;
    std::string ca_file;
};

struct AppConfig {
    Mqtt mqtt;
    Access access;
    Pir pir;
    Camera camera;
    Display display;
    Detect detect;
    Face face;
    Encode encode;
    Record record;
    Rtsp rtsp;
    Log log;
};

// 解析并校验。有任何错误返回 nullopt，errors 里是全部问题
std::optional<AppConfig> load(std::string_view text, std::vector<ini::Error> &errors);

// 读文件再 load；文件打不开也记成一条错误
std::optional<AppConfig> load_file(const std::string &path, std::vector<ini::Error> &errors);

// 生效配置的可读摘要，启动时写进日志
std::string summary(const AppConfig &cfg);

}  // namespace rksec::config
