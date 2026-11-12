#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "display/drm_select.hpp"

// DRM/KMS 显示输出：把一整屏 XRGB8888 画面显示到 HDMI 屏上，换帧不撕裂。
//
// 做法（第 4b 步）：
// - 按驱动名选显卡（drm_select.hpp），打开后程序成为 DRM master（同一时间只能有一个程序控制显示）
// - 申请两块 dumb buffer（内核分配、能 mmap 的显存），轮流当“正在显示的”和“正在准备的”
// - present(画面)：等上一次翻页完成 → 把画面拷进后台那块 → 请求在下一次垂直消隐时翻页（page flip）
//   翻页在消隐期间完成，屏幕不会出现半张新半张旧（撕裂）；拷贝前等翻页完成，不会写到正在显示的那块
// - 关闭时把显示恢复成打开前的样子（控制台重新出现），程序崩溃时内核也会收回 master
//
// 为什么界面不直接画在 dumb buffer 上：显存映射通常不经过 CPU 缓存（write-combine），
// 写入很快，但读出非常慢；界面库画半透明效果要读目标像素。所以界面画在普通内存里，再整块拷过去（顺序写，很快）。
//
// 线程：只在一个线程（界面线程）里使用。
namespace rksec::display {

class DrmDisplay {
  public:
    struct Options {
        std::string device = "auto";     // "auto"：按驱动名 rockchip 选；或者 /dev/dri/cardN
        std::string connector = "auto";  // "auto"：第一个已连接的输出口；或者 "HDMI-A-1"
    };

    DrmDisplay();
    ~DrmDisplay();
    DrmDisplay(const DrmDisplay &) = delete;
    DrmDisplay &operator=(const DrmDisplay &) = delete;

    // 打开设备、选输出口和模式、申请缓冲区。失败返回 false，err 里是原因（给人看的）
    bool open(const Options &opts, std::string &err);

    int width() const;
    int height() const;
    const ModeInfo &mode() const;
    std::string summary() const;  // 选了哪张卡、哪个输出口、什么模式，写日志用

    // 显示一整屏画面。pixels 是 width×height 的 XRGB8888，每行 stride 字节。
    // 会等上一次翻页完成（最多一帧时间），所以调用频率自然不超过屏幕刷新率。
    bool present(const std::uint8_t *pixels, std::size_t stride, std::string &err);

    // 等最后一次翻页完成（退出前调用，避免关闭时还有翻页在进行）
    void wait_idle();

    struct Stats {
        std::uint64_t presented = 0;   // 成功提交的帧
        std::uint64_t flip_waits = 0;  // 提交时上一次翻页真的还没完成、要等的次数
        std::uint64_t flip_timeouts = 0;
    };
    Stats stats() const;

    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rksec::display
