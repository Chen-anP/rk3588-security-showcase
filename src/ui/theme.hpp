#pragma once

#include <cstdint>

// 界面尺寸和颜色，集中在这里，换屏或调整风格只改这一个文件。
//
// 屏幕：7 寸 1024x600，约 170 dpi（1 mm ≈ 6.7 像素）。
// 手指按的东西至少 9～10 mm 见方 → 至少 64 像素；按钮之间留空，防止按错相邻的。
namespace rksec::ui::theme {

constexpr int kScreenW = 1024;
constexpr int kScreenH = 600;

constexpr int kPad = 16;         // 外边距、块与块之间的间距
constexpr int kTopBarH = 56;     // 顶栏
constexpr int kTouchMin = 64;    // 可点击元素的最小高度（手指）
constexpr int kNavButtonH = 96;  // 首页右侧的大按钮
constexpr int kRadius = 12;
constexpr int kBoxBorder = 2;  // 检测框的线宽

// 预览区：摄像头 1280x720 解码时缩小一半，正好 640x360，一个像素对一个像素，不用再缩放
constexpr int kPreviewW = 640;
constexpr int kPreviewH = 360;

// 颜色（0xRRGGBB）
constexpr std::uint32_t kBg = 0x101418;
constexpr std::uint32_t kPanel = 0x1C232B;
constexpr std::uint32_t kPanelPressed = 0x2A3440;
constexpr std::uint32_t kAccent = 0x2F80ED;
constexpr std::uint32_t kOk = 0x27AE60;
constexpr std::uint32_t kWarn = 0xF2994A;
constexpr std::uint32_t kError = 0xEB5757;
constexpr std::uint32_t kText = 0xE6E9ED;
constexpr std::uint32_t kTextDim = 0x8A94A0;

}  // namespace rksec::ui::theme
