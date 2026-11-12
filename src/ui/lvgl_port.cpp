#include "ui/lvgl_port.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#include "core/log.hpp"

namespace rksec::ui {

namespace {

std::uint32_t tick_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void log_cb(lv_log_level_t level, const char *buf)
{
    std::string s{buf};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    if (level >= LV_LOG_LEVEL_ERROR) {
        LOG_E("lvgl", "%s", s.c_str());
    } else {
        LOG_W("lvgl", "%s", s.c_str());
    }
}

}  // namespace

void lvgl_init()
{
    lv_init();
    lv_tick_set_cb(&tick_ms);
    lv_log_register_print_cb(&log_cb);
}

void lvgl_deinit()
{
    lv_deinit();
}

RamDisplay::RamDisplay(int width, int height, Present present) : present_(std::move(present))
{
    disp_ = lv_display_create(width, height);
    lv_display_set_color_format(disp_, LV_COLOR_FORMAT_XRGB8888);
    stride_ = lv_draw_buf_width_to_stride(static_cast<std::uint32_t>(width), LV_COLOR_FORMAT_XRGB8888);
    buf_.assign(stride_ * static_cast<std::size_t>(height), 0);
    lv_display_set_buffers(disp_, buf_.data(), nullptr, static_cast<std::uint32_t>(buf_.size()),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp_, &RamDisplay::flush_cb);
    lv_display_set_user_data(disp_, this);
}

RamDisplay::~RamDisplay()
{
    if (disp_ != nullptr) {
        lv_display_delete(disp_);
    }
}

void RamDisplay::flush_cb(lv_display_t *disp, const lv_area_t * /*area*/, std::uint8_t * /*px_map*/)
{
    auto *self = static_cast<RamDisplay *>(lv_display_get_user_data(disp));
    // DIRECT 模式下每个变化的区域调用一次；整屏缓冲区已经是完整的画面，最后一个区域时整屏交出去
    if (lv_display_flush_is_last(disp) && self->present_) {
        if (self->present_(self->buf_.data(), self->stride_)) {
            ++self->frames_;
        } else {
            ++self->errors_;
        }
    }
    lv_display_flush_ready(disp);
}

}  // namespace rksec::ui
