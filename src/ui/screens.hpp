#pragma once

#include <lvgl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "access/pass_store.hpp"
#include "media/jpeg_decoder.hpp"
#include "ui/router.hpp"

// 界面骨架（第 4b 步）：顶栏 + 四个页面（首页、事件记录、设置、关于本机）。
//
// 布局（1024x600）：
//   ┌──────────────────────── 顶栏 56 ───────────────────────────┐
//   │ 智能门禁                         ● 摄像头正常    14:03:22  │
//   ├────────────────────────────────────┬───────────────────────┤
//   │                                    │  [≡ 事件记录]  96 高   │
//   │      摄像头预览 640x360            │  [⚙ 设置]             │
//   │                                    │                       │
//   ├─────┬─────┬─────┬─────┬─────┬──────┤                       │
//   │帧率 │解码 │检测 │丢帧 │界面 │处理器│                       │
//   └─────┴─────┴─────┴─────┴─────┴──────┴───────────────────────┘
// 预览上叠加检测框（第 6 步）：框和标签是预先建好的对象，每次只改位置、文字和显示/隐藏。
// 其他页面左上角有“返回”按钮（64 高）。
//
// 页面切换由 Router 决定，这里只负责“显示哪个页面”：所有页面一开始都建好，切换时只是隐藏/显示，
// 没有创建、销毁对象的开销，也不会因为反复进出页面泄漏内存。
//
// 只能在界面线程里使用（LVGL 不是线程安全的）。
namespace rksec::ui {

// 首页和顶栏上显示的运行状态，界面线程每秒更新一次
struct UiStatus {
    bool camera_ok = false;
    double camera_fps = 0;
    double decode_p50_ms = 0;
    bool detecting = false;    // 没在检测时“检测”一栏显示 -
    double detect_p50_ms = 0;  // 推理 + 后处理
    std::uint64_t lost = 0;
    double ui_fps = 0;
    double cpu_percent = 0;  // 整个进程，100% = 一个核满载
};

// 预览上的一个检测框：预览图的像素坐标（左上角是预览图左上角），标签文字（会检查字体里有没有这些字）
struct OverlayBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string label;
    std::uint32_t color = 0x27AE60;  // 框和标签底色 0xRRGGBB，默认是 theme::kOk（第 7c 步：按身份换颜色）
};

// 设置、关于页面上的静态信息
struct DeviceInfo {
    std::string display;  // "1024x600 59.81Hz"
    std::string camera;   // "1280x720 MJPG"
    std::string touch;    // 触摸设备路径，空 = 没有配置
    std::string version;
};

class Screens {
  public:
    Screens(lv_display_t *disp, Router &router, DeviceInfo info);
    Screens(const Screens &) = delete;
    Screens &operator=(const Screens &) = delete;

    void update_status(const UiStatus &s);
    void show_access(const std::string &text);
    void show_passes(const std::vector<access::PassRow> &rows, const std::string &error);
    lv_obj_t *pass_button(std::size_t i) const { return pass_buttons_.at(i); }
    lv_obj_t *history_next() const { return history_next_; }
    lv_obj_t *history_close() const { return history_close_; }
    bool detail_visible() const;

    // 显示一帧预览。img 的像素必须在下一次调用之前一直有效（LatestImage 的 front 满足这一点）
    void show_preview(const media::Image &img);
    // 摄像头断开：不显示旧画面，显示提示（检测框一起清掉）
    void clear_preview();

    // 检测框：最多 kMaxBoxes 个，多的不画（按调用方给的顺序，调用方先按分数排好）。空 = 清掉所有框
    static constexpr std::size_t kMaxBoxes = 16;
    void show_boxes(const std::vector<OverlayBox> &boxes);
    std::size_t visible_boxes() const;  // 测试用

    // 测试用：页面上的按钮和页面本身
    lv_obj_t *nav_button(Page target) const;  // 首页的“事件记录”“设置”，设置页的“关于本机”
    lv_obj_t *back_button(Page page) const;
    bool page_visible(Page p) const;

  private:
    struct Target {
        Screens *self = nullptr;
        Page page = Page::home;
    };

    void build_top_bar(lv_obj_t *scr);
    void build_home(lv_obj_t *page);
    void build_events(lv_obj_t *page);
    void build_settings(lv_obj_t *page);
    void build_about(lv_obj_t *page);
    lv_obj_t *make_page(lv_obj_t *scr);
    lv_obj_t *make_nav_button(lv_obj_t *parent, const char *symbol, const char *label, Page target, int w);
    lv_obj_t *make_back_button(lv_obj_t *page, Page on_page, const char *title);
    lv_obj_t *make_info_row(lv_obj_t *parent, const char *name, const std::string &value, int y);
    void show_page(Page p);

    static void on_nav(lv_event_t *e);
    static void on_back(lv_event_t *e);
    static void on_pass(lv_event_t *e);
    static void on_history_page(lv_event_t *e);
    static void on_history_close(lv_event_t *e);
    void refresh_history();
    void show_pass_detail(std::int64_t id);

    Router &router_;
    DeviceInfo info_;
    lv_font_t cn_font_{};  // 中文 20 号，缺的字到人名用字里找（第 7d 步），再缺（图标）到 Montserrat 里找
    lv_font_t names_font_{};  // 人名用字 20 号（GB2312 一级汉字）
    lv_font_t title_font_{};  // 中文 28 号

    std::array<lv_obj_t *, 4> pages_{};
    std::array<lv_obj_t *, 4> nav_buttons_{};
    std::array<lv_obj_t *, 4> back_buttons_{};
    std::array<Target, 4> targets_{};

    lv_obj_t *access_card_ = nullptr;
    std::vector<access::PassRow> passes_;
    std::string history_error_;
    std::size_t history_page_ = 0;
    std::int64_t detail_id_ = 0;
    std::array<lv_obj_t *, 4> pass_buttons_{};
    std::array<lv_obj_t *, 4> pass_labels_{};
    lv_obj_t *history_hint_ = nullptr;
    lv_obj_t *history_next_ = nullptr;
    lv_obj_t *history_prev_ = nullptr;
    lv_obj_t *history_detail_ = nullptr;
    lv_obj_t *history_detail_text_ = nullptr;
    lv_obj_t *history_close_ = nullptr;
    lv_obj_t *access_label_ = nullptr;
    std::string access_text_;
    lv_obj_t *clock_ = nullptr;
    lv_obj_t *cam_dot_ = nullptr;
    lv_obj_t *cam_label_ = nullptr;
    lv_obj_t *preview_box_ = nullptr;
    lv_obj_t *preview_ = nullptr;
    std::array<lv_obj_t *, kMaxBoxes> boxes_{};
    std::array<lv_obj_t *, kMaxBoxes> box_labels_{};
    std::array<std::string, kMaxBoxes> box_texts_;  // 标签文字没变就不重新设置（设置文字会重新排版）
    std::array<std::uint32_t, kMaxBoxes> box_colors_{};
    lv_obj_t *preview_hint_ = nullptr;
    lv_image_dsc_t preview_dsc_{};
    std::array<lv_obj_t *, 6> stat_values_{};
    bool camera_ok_shown_ = false;
    bool status_initialized_ = false;
    bool box_glyph_warned_ = false;
};

}  // namespace rksec::ui
