#pragma once

#include <array>

// 界面上所有固定的中文文字，集中在这里：
// - 中文字体只含界面用到的字（tools/fonts/gen_fonts.sh 从这个文件收集），缺的字会显示成方框。
//   tests/test_ui.cpp 把 kAll 里每个字都在字体里查一遍
// - 以后做多语言，也是从这里替换
// 新加文字：写在下面，加进 kAll，再运行 tools/fonts/gen_fonts.sh 重新生成字体。
namespace rksec::ui::text {

inline constexpr const char *kTitle = "智能门禁";
inline constexpr const char *kCameraOk = "摄像头正常";
inline constexpr const char *kCameraLost = "摄像头未连接";
inline constexpr const char *kNoPreview = "等待摄像头画面";
inline constexpr const char *kEvents = "事件记录";
inline constexpr const char *kSettings = "设置";
inline constexpr const char *kAbout = "关于本机";
inline constexpr const char *kBack = "返回";
inline constexpr const char *kNoEvents = "暂无事件记录";
inline constexpr const char *kFps = "帧率";
inline constexpr const char *kDecode = "解码 ms";
inline constexpr const char *kDetect = "检测 ms";
inline constexpr const char *kLost = "丢帧";
inline constexpr const char *kUiFps = "界面";
inline constexpr const char *kCpu = "处理器";
inline constexpr const char *kDisplay = "显示";
inline constexpr const char *kCamera = "摄像头";
inline constexpr const char *kVersion = "版本";
inline constexpr const char *kTouch = "触摸";
inline constexpr const char *kTouchNone = "未配置";
inline constexpr const char *kPerson = "人";        // 检测框上的标签
inline constexpr const char *kStranger = "陌生人";  // 有脸但不在底库里（第 7c 步）

inline constexpr std::array kAll = {kTitle,    kCameraOk, kCameraLost, kNoPreview, kEvents, kSettings, kAbout, kBack,
                                    kNoEvents, kFps,      kDecode,     kDetect,    kLost,   kUiFps,    kCpu,   kDisplay,
                                    kCamera,   kVersion,  kTouch,      kTouchNone, kPerson, kStranger};

}  // namespace rksec::ui::text
