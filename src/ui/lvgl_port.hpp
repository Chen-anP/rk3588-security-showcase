#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// LVGL 和本程序之间的接口：初始化、时钟、日志，以及“画在内存里的显示”。
//
// 渲染方式（LV_DISPLAY_RENDER_MODE_DIRECT，单块整屏缓冲区）：
// - LVGL 只重画变化的区域，直接画在一块整屏大小的普通内存里，没变的地方保持上一帧的内容
// - 一帧的所有区域画完（flush_is_last）后，调用 present 把整屏交给显示输出（DrmDisplay::present）
// 显示和测试共用这一套：测试时 present 什么都不做，或者把画面存下来看。
namespace rksec::ui {

// 整个进程调用一次：lv_init、毫秒时钟、把 LVGL 的日志转到本程序的日志
void lvgl_init();
void lvgl_deinit();

class RamDisplay {
  public:
    using Present = std::function<bool(const std::uint8_t *pixels, std::size_t stride)>;

    RamDisplay(int width, int height, Present present);
    ~RamDisplay();
    RamDisplay(const RamDisplay &) = delete;
    RamDisplay &operator=(const RamDisplay &) = delete;

    lv_display_t *display() const { return disp_; }
    const std::uint8_t *pixels() const { return buf_.data(); }
    std::size_t stride() const { return stride_; }

    std::uint64_t frames() const { return frames_; }          // 交给 present 的帧数
    std::uint64_t present_errors() const { return errors_; }  // present 返回 false 的次数

  private:
    static void flush_cb(lv_display_t *disp, const lv_area_t *area, std::uint8_t *px_map);

    lv_display_t *disp_ = nullptr;
    std::vector<std::uint8_t> buf_;
    std::size_t stride_ = 0;
    Present present_;
    std::uint64_t frames_ = 0;
    std::uint64_t errors_ = 0;
};

}  // namespace rksec::ui
