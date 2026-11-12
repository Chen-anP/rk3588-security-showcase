#include "ui/screens.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include "core/log.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace rksec::ui {

namespace {

std::size_t idx(Page p)
{
    return static_cast<std::size_t>(p);
}

lv_color_t color(std::uint32_t rgb)
{
    return lv_color_hex(rgb);
}

// 去掉默认主题给容器加的边框、内边距、滚动条，做成一个纯粹的定位用的盒子
void plain_box(lv_obj_t *o)
{
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    plain_box(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color(theme::kPanel), 0);
    lv_obj_set_style_radius(o, theme::kRadius, 0);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color(rgb), 0);
    lv_label_set_text(l, text);
    return l;
}

constexpr int kContentY = theme::kTopBarH;
constexpr int kContentH = theme::kScreenH - theme::kTopBarH;
constexpr int kRightX = theme::kPad + theme::kPreviewW + theme::kPad;  // 右侧一栏的起点
constexpr int kRightW = theme::kScreenW - kRightX - theme::kPad;
constexpr int kStatCount = 6;
constexpr int kStatY = theme::kPad + theme::kPreviewH + theme::kPad;
constexpr int kStatH = kContentH - kStatY - theme::kPad;
constexpr int kStatW = (theme::kPreviewW - (kStatCount - 1) * theme::kPad) / kStatCount;

}  // namespace

Screens::Screens(lv_display_t *disp, Router &router, DeviceInfo info) : router_(router), info_(std::move(info))
{
    names_font_ = rksec_font_names_20;
    names_font_.fallback = &lv_font_montserrat_20;
    cn_font_ = rksec_font_cn_20;
    cn_font_.fallback = &names_font_;
    title_font_ = rksec_font_cn_28;
    title_font_.fallback = &lv_font_montserrat_28;

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, color(theme::kBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(scr);
    for (auto &p : pages_) {
        p = make_page(scr);
    }
    build_home(pages_[idx(Page::home)]);
    build_events(pages_[idx(Page::events)]);
    build_settings(pages_[idx(Page::settings)]);
    build_about(pages_[idx(Page::about)]);

    router_.on_change([this](Page from, Page to) {
        LOG_D("ui", "页面 %s → %s", page_name(from), page_name(to));
        show_page(to);
    });
    show_page(router_.current());
    clear_preview();

    // 运行时拼出来的文字（不在 text.hpp 里，测试管不到）：缺字就写日志，不悄悄显示方框（D17）
    for (const std::string *s : {&info_.display, &info_.camera, &info_.touch, &info_.version}) {
        if (const std::string m = missing_glyphs(*s); !m.empty()) {
            LOG_W("ui",
                  "界面字体里没有“%s”（在“%s”里），会显示成方框：加进 tools/fonts/extra_chars.txt 后运行 "
                  "tools/fonts/gen_fonts.sh",
                  m.c_str(), s->c_str());
        }
    }
}

lv_obj_t *Screens::make_page(lv_obj_t *scr)
{
    lv_obj_t *p = lv_obj_create(scr);
    plain_box(p);
    lv_obj_set_pos(p, 0, kContentY);
    lv_obj_set_size(p, theme::kScreenW, kContentH);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    return p;
}

void Screens::build_top_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    plain_box(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, theme::kScreenW, theme::kTopBarH);
    lv_obj_set_style_bg_color(bar, color(theme::kPanel), 0);

    lv_obj_t *title = label(bar, &title_font_, theme::kText, text::kTitle);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, theme::kPad, 0);

    clock_ = label(bar, &lv_font_montserrat_20, theme::kText, "--:--:--");
    lv_obj_align(clock_, LV_ALIGN_RIGHT_MID, -theme::kPad, 0);

    cam_label_ = label(bar, &cn_font_, theme::kTextDim, text::kCameraLost);
    lv_obj_align(cam_label_, LV_ALIGN_RIGHT_MID, -140, 0);

    cam_dot_ = lv_obj_create(bar);
    plain_box(cam_dot_);
    lv_obj_set_size(cam_dot_, 12, 12);
    lv_obj_set_style_radius(cam_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cam_dot_, color(theme::kError), 0);
    lv_obj_align_to(cam_dot_, cam_label_, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

void Screens::build_home(lv_obj_t *page)
{
    // 预览区：黑底，图片居中；没有画面时显示提示
    lv_obj_t *box = panel(page, theme::kPad, theme::kPad, theme::kPreviewW, theme::kPreviewH);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    preview_box_ = box;
    preview_ = lv_image_create(box);
    lv_obj_center(preview_);
    preview_hint_ = label(box, &cn_font_, theme::kTextDim, text::kNoPreview);
    lv_obj_center(preview_hint_);

    // 检测框：后创建的画在上面，所以在图片之后建
    for (std::size_t i = 0; i < kMaxBoxes; ++i) {
        lv_obj_t *b = lv_obj_create(box);
        plain_box(b);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(b, theme::kBoxBorder, 0);
        lv_obj_set_style_border_color(b, color(theme::kOk), 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *l = label(b, &cn_font_, theme::kBg, "");
        lv_obj_set_style_bg_color(l, color(theme::kOk), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(l, 4, 0);
        lv_obj_set_pos(l, 0, 0);
        boxes_[i] = b;
        box_labels_[i] = l;
        box_colors_[i] = theme::kOk;
    }

    access_card_ = panel(page, kRightX, 2 * (theme::kNavButtonH + theme::kPad) + theme::kPad, kRightW, 136);
    access_label_ = label(access_card_, &cn_font_, theme::kText, "门禁未启用");
    lv_obj_set_pos(access_label_, 16, 16);
    lv_obj_set_width(access_label_, kRightW - 32);

    // 状态小块：数值（大号数字）+ 名称
    const std::array<const char *, kStatCount> names = {text::kFps,  text::kDecode, text::kDetect,
                                                        text::kLost, text::kUiFps,  text::kCpu};
    for (int i = 0; i < kStatCount; ++i) {
        lv_obj_t *tile = panel(page, theme::kPad + i * (kStatW + theme::kPad), kStatY, kStatW, kStatH);
        lv_obj_t *v = label(tile, &lv_font_montserrat_28, theme::kText, "-");
        lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_t *n = label(tile, &cn_font_, theme::kTextDim, names[static_cast<std::size_t>(i)]);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -20);
        stat_values_[static_cast<std::size_t>(i)] = v;
    }

    lv_obj_t *events = make_nav_button(page, LV_SYMBOL_LIST, "通行记录", Page::events, kRightW);
    lv_obj_set_pos(events, kRightX, theme::kPad);
    lv_obj_t *settings = make_nav_button(page, LV_SYMBOL_SETTINGS, text::kSettings, Page::settings, kRightW);
    lv_obj_set_pos(settings, kRightX, theme::kPad + theme::kNavButtonH + theme::kPad);
}

void Screens::build_events(lv_obj_t *page)
{
    make_back_button(page, Page::events, "通行记录");
    history_hint_ = label(page, &cn_font_, theme::kTextDim, "正在读取记录");
    lv_obj_set_pos(history_hint_, 400, 30);
    lv_obj_set_width(history_hint_, 608);
    lv_label_set_long_mode(history_hint_, LV_LABEL_LONG_DOT);
    for (std::size_t i = 0; i < pass_buttons_.size(); ++i) {
        auto *b = lv_button_create(page);
        lv_obj_set_pos(b, 16, 96 + static_cast<int>(i) * 80);
        lv_obj_set_size(b, 992, 68);
        auto *l = label(b, &cn_font_, theme::kText, "");
        lv_obj_set_width(l, 960);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_center(l);
        pass_buttons_[i] = b;
        pass_labels_[i] = l;
        lv_obj_add_event_cb(b, &Screens::on_pass, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    history_prev_ = lv_button_create(page);
    lv_obj_set_pos(history_prev_, 16, 432);
    lv_obj_set_size(history_prev_, 160, 64);
    lv_obj_center(label(history_prev_, &cn_font_, theme::kText, "上一页"));
    history_next_ = lv_button_create(page);
    lv_obj_set_pos(history_next_, 848, 432);
    lv_obj_set_size(history_next_, 160, 64);
    lv_obj_center(label(history_next_, &cn_font_, theme::kText, "下一页"));
    for (auto *b : {history_prev_, history_next_})
        lv_obj_add_event_cb(b, &Screens::on_history_page, LV_EVENT_CLICKED, this);
    history_detail_ = panel(page, 16, 96, 992, 416);
    history_detail_text_ = label(history_detail_, &cn_font_, theme::kText, "");
    lv_obj_set_pos(history_detail_text_, 20, 16);
    lv_obj_set_width(history_detail_text_, 952);
    history_close_ = lv_button_create(history_detail_);
    lv_obj_set_pos(history_close_, 20, 332);
    lv_obj_set_size(history_close_, 180, 64);
    lv_obj_center(label(history_close_, &cn_font_, theme::kText, "返回列表"));
    lv_obj_add_event_cb(history_close_, &Screens::on_history_close, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(history_detail_, LV_OBJ_FLAG_HIDDEN);
}

void Screens::build_settings(lv_obj_t *page)
{
    make_back_button(page, Page::settings, text::kSettings);
    int y = theme::kPad + theme::kTouchMin + theme::kPad;
    make_info_row(page, text::kDisplay, info_.display, y);
    y += theme::kTouchMin;
    make_info_row(page, text::kCamera, info_.camera, y);
    y += theme::kTouchMin;
    make_info_row(page, text::kTouch, info_.touch.empty() ? text::kTouchNone : info_.touch, y);
    y += theme::kTouchMin + theme::kPad;
    lv_obj_t *about = make_nav_button(page, LV_SYMBOL_RIGHT, text::kAbout, Page::about, kRightW);
    lv_obj_set_pos(about, theme::kPad, y);
}

void Screens::build_about(lv_obj_t *page)
{
    make_back_button(page, Page::about, text::kAbout);
    make_info_row(page, text::kVersion, info_.version, theme::kPad + theme::kTouchMin + theme::kPad);
}

lv_obj_t *Screens::make_nav_button(lv_obj_t *parent, const char *symbol, const char *label_text, Page target, int w)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, theme::kNavButtonH);
    lv_obj_set_style_radius(b, theme::kRadius, 0);
    lv_obj_set_style_bg_color(b, color(theme::kPanel), 0);
    lv_obj_set_style_bg_color(b, color(theme::kPanelPressed), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);

    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%s   %s", symbol, label_text);
    lv_obj_t *l = label(b, &cn_font_, theme::kText, buf.data());
    lv_obj_align(l, LV_ALIGN_LEFT_MID, theme::kPad, 0);

    Target &t = targets_[idx(target)];
    t = Target{this, target};
    lv_obj_add_event_cb(b, &Screens::on_nav, LV_EVENT_CLICKED, &t);
    nav_buttons_[idx(target)] = b;
    return b;
}

lv_obj_t *Screens::make_back_button(lv_obj_t *page, Page on_page, const char *title)
{
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_size(b, 160, theme::kTouchMin);
    lv_obj_set_pos(b, theme::kPad, theme::kPad);
    lv_obj_set_style_radius(b, theme::kRadius, 0);
    lv_obj_set_style_bg_color(b, color(theme::kPanel), 0);
    lv_obj_set_style_bg_color(b, color(theme::kPanelPressed), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%s  %s", LV_SYMBOL_LEFT, text::kBack);
    lv_obj_t *l = label(b, &cn_font_, theme::kText, buf.data());
    lv_obj_center(l);
    lv_obj_add_event_cb(b, &Screens::on_back, LV_EVENT_CLICKED, this);
    back_buttons_[idx(on_page)] = b;

    lv_obj_t *t = label(page, on_page == Page::events ? &cn_font_ : &title_font_, theme::kText, title);
    lv_obj_align_to(t, b, LV_ALIGN_OUT_RIGHT_MID, theme::kPad * 2, 0);
    return b;
}

lv_obj_t *Screens::make_info_row(lv_obj_t *parent, const char *name, const std::string &value, int y)
{
    lv_obj_t *n = label(parent, &cn_font_, theme::kTextDim, name);
    lv_obj_set_pos(n, theme::kPad * 2, y + 18);
    lv_obj_t *v = label(parent, &cn_font_, theme::kText, value.c_str());
    lv_obj_set_pos(v, 200, y + 18);
    return v;
}

void Screens::show_page(Page p)
{
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (i == idx(p)) {
            lv_obj_remove_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void Screens::on_nav(lv_event_t *e)
{
    auto *t = static_cast<Target *>(lv_event_get_user_data(e));
    t->self->router_.go(t->page);
}

void Screens::on_back(lv_event_t *e)
{
    auto *self = static_cast<Screens *>(lv_event_get_user_data(e));
    self->router_.back();
}

void Screens::show_access(const std::string &text)
{
    if (access_text_ == text) {
        return;
    }
    access_text_ = text;
    lv_label_set_text(access_label_, missing_name_glyphs(text).empty() ? text.c_str() : "Access status unavailable");
    const auto rgb = text.find("失败") != std::string::npos || text.find("故障") != std::string::npos ? theme::kError
                     : text == "模拟开门"                                                             ? theme::kOk
                     : text == "陌生人"                                                               ? theme::kWarn
                                                                                                      : theme::kAccent;
    lv_obj_set_style_border_width(access_card_, 3, 0);
    lv_obj_set_style_border_color(access_card_, color(rgb), 0);
}

namespace {
std::string pass_time(std::int64_t ms)
{
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    if (!localtime_r(&t, &tm))
        return "-";
    std::array<char, 32> b{};
    std::strftime(b.data(), b.size(), "%Y-%m-%d %H:%M:%S", &tm);
    return b.data();
}
std::string safe_name(const std::string &name)
{
    if (name.size() > 192 || name.find_first_of("\r\n\t") != std::string::npos || !missing_name_glyphs(name).empty())
        return "姓名无法显示";
    return name;
}
}  // namespace
bool Screens::detail_visible() const
{
    return !lv_obj_has_flag(history_detail_, LV_OBJ_FLAG_HIDDEN);
}
void Screens::show_passes(const std::vector<access::PassRow> &rows, const std::string &error)
{
    // Bound even unexpected caller input. Updates never retain image data.
    passes_.assign(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(std::min(rows.size(), std::size_t{50})));
    history_error_ = error;
    if (!error.empty())
        passes_.clear();
    history_page_ = std::min(history_page_, passes_.empty() ? std::size_t{0} : (passes_.size() - 1) / 4);
    refresh_history();
    if (detail_id_)
        show_pass_detail(detail_id_);
}
void Screens::refresh_history()
{
    std::string hint =
        history_error_.empty()
            ? passes_.empty() ? "暂无通行记录" : "最近50条 / 本地时间 / " + std::to_string(history_page_ + 1) + "页"
            : "记录读取失败，请稍后重试";
    lv_label_set_text(history_hint_, hint.c_str());
    for (std::size_t i = 0; i < 4; ++i) {
        auto n = history_page_ * 4 + i;
        if (n >= passes_.size()) {
            lv_obj_add_flag(pass_buttons_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(pass_buttons_[i], LV_OBJ_FLAG_HIDDEN);
        const auto &r = passes_[n];
        auto text = pass_time(r.occurred_ms) + "   " + safe_name(r.person_name) + "   #" + std::to_string(r.id);
        lv_label_set_text(pass_labels_[i], text.c_str());
    }
    if (history_page_ == 0)
        lv_obj_add_state(history_prev_, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(history_prev_, LV_STATE_DISABLED);
    if ((history_page_ + 1) * 4 >= passes_.size())
        lv_obj_add_state(history_next_, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(history_next_, LV_STATE_DISABLED);
}
void Screens::show_pass_detail(std::int64_t id)
{
    auto it = std::find_if(passes_.begin(), passes_.end(), [id](const auto &r) { return r.id == id; });
    if (it == passes_.end()) {
        detail_id_ = 0;
        lv_obj_add_flag(history_detail_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    detail_id_ = id;
    std::ostringstream text;
    text << "通行记录 #" << id << "\n\n姓名：" << safe_name(it->person_name) << "\n时间：" << pass_time(it->occurred_ms)
         << "\n分数：" << std::fixed << std::setprecision(3) << it->score << "\n人员："
         << (it->person_id ? "已登记" : "已删除（历史记录）") << "\n快照：" << (it->has_snapshot ? "已保存" : "无")
         << "\n模拟授权记录";
    lv_label_set_text(history_detail_text_, text.str().c_str());
    lv_obj_remove_flag(history_detail_, LV_OBJ_FLAG_HIDDEN);
}
void Screens::on_pass(lv_event_t *e)
{
    auto *s = static_cast<Screens *>(lv_event_get_user_data(e));
    auto *target = lv_event_get_target_obj(e);
    for (std::size_t i = 0; i < 4; ++i)
        if (s->pass_buttons_[i] == target) {
            auto n = s->history_page_ * 4 + i;
            if (n < s->passes_.size())
                s->show_pass_detail(s->passes_[n].id);
            break;
        }
}
void Screens::on_history_page(lv_event_t *e)
{
    auto *s = static_cast<Screens *>(lv_event_get_user_data(e));
    if (lv_event_get_target_obj(e) == s->history_next_) {
        if ((s->history_page_ + 1) * 4 < s->passes_.size())
            ++s->history_page_;
    } else if (s->history_page_)
        --s->history_page_;
    s->refresh_history();
}
void Screens::on_history_close(lv_event_t *e)
{
    auto *s = static_cast<Screens *>(lv_event_get_user_data(e));
    s->detail_id_ = 0;
    lv_obj_add_flag(s->history_detail_, LV_OBJ_FLAG_HIDDEN);
}

void Screens::update_status(const UiStatus &s)
{
    // 时钟
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    lv_label_set_text(clock_, buf.data());

    // 摄像头状态只在变化时改（改文字会触发重新布局和重画）
    if (!status_initialized_ || s.camera_ok != camera_ok_shown_) {
        lv_label_set_text(cam_label_, s.camera_ok ? text::kCameraOk : text::kCameraLost);
        lv_obj_set_style_text_color(cam_label_, color(s.camera_ok ? theme::kText : theme::kError), 0);
        lv_obj_set_style_bg_color(cam_dot_, color(s.camera_ok ? theme::kOk : theme::kError), 0);
        lv_obj_align_to(cam_dot_, cam_label_, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        camera_ok_shown_ = s.camera_ok;
        status_initialized_ = true;
    }

    std::snprintf(buf.data(), buf.size(), "%.1f", s.camera_fps);
    lv_label_set_text(stat_values_[0], buf.data());
    std::snprintf(buf.data(), buf.size(), "%.1f", s.decode_p50_ms);
    lv_label_set_text(stat_values_[1], buf.data());
    if (s.detecting) {
        std::snprintf(buf.data(), buf.size(), "%.1f", s.detect_p50_ms);
        lv_label_set_text(stat_values_[2], buf.data());
    } else {
        lv_label_set_text(stat_values_[2], "-");
    }
    std::snprintf(buf.data(), buf.size(), "%llu", static_cast<unsigned long long>(s.lost));
    lv_label_set_text(stat_values_[3], buf.data());
    std::snprintf(buf.data(), buf.size(), "%.0f", s.ui_fps);
    lv_label_set_text(stat_values_[4], buf.data());
    std::snprintf(buf.data(), buf.size(), "%.0f%%", s.cpu_percent);
    lv_label_set_text(stat_values_[5], buf.data());
}

void Screens::show_preview(const media::Image &img)
{
    if (img.width <= 0 || img.height <= 0) {
        return;
    }
    // 图片描述指向 LatestImage 里的像素，不拷贝。先让 LVGL 忘掉上一张（同一个描述结构，内容变了）
    lv_image_cache_drop(&preview_dsc_);
    preview_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_dsc_.header.cf = LV_COLOR_FORMAT_XRGB8888;
    preview_dsc_.header.w = static_cast<std::uint16_t>(img.width);
    preview_dsc_.header.h = static_cast<std::uint16_t>(img.height);
    preview_dsc_.header.stride = static_cast<std::uint16_t>(img.stride);
    preview_dsc_.data_size = static_cast<std::uint32_t>(img.pixels.size());
    preview_dsc_.data = img.pixels.data();
    lv_image_set_src(preview_, &preview_dsc_);
    lv_obj_center(preview_);
    lv_obj_invalidate(preview_);
    lv_obj_remove_flag(preview_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preview_hint_, LV_OBJ_FLAG_HIDDEN);
}

void Screens::clear_preview()
{
    lv_obj_add_flag(preview_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_hint_, LV_OBJ_FLAG_HIDDEN);
    show_boxes({});
}

void Screens::show_boxes(const std::vector<OverlayBox> &boxes)
{
    for (std::size_t i = 0; i < kMaxBoxes; ++i) {
        lv_obj_t *b = boxes_[i];
        if (i >= boxes.size()) {
            if (!lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        const OverlayBox &o = boxes[i];
        // 裁到预览区里面；太小的框（不到 2 个边框宽）也画出来，看得见
        const int x = std::clamp(o.x, 0, theme::kPreviewW - 1);
        const int y = std::clamp(o.y, 0, theme::kPreviewH - 1);
        const int w = std::clamp(o.w, 2 * theme::kBoxBorder, theme::kPreviewW - x);
        const int h = std::clamp(o.h, 2 * theme::kBoxBorder, theme::kPreviewH - y);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, w, h);
        if (box_texts_[i] != o.label) {
            if (const std::string m = missing_name_glyphs(o.label); !m.empty() && !box_glyph_warned_) {
                box_glyph_warned_ = true;  // 只提醒一次
                LOG_W("ui", "界面字体里没有“%s”（检测框标签“%s”），会显示成方框", m.c_str(), o.label.c_str());
            }
            box_texts_[i] = o.label;
            lv_label_set_text(box_labels_[i], o.label.c_str());
        }
        if (box_colors_[i] != o.color) {
            box_colors_[i] = o.color;
            lv_obj_set_style_border_color(b, color(o.color), 0);
            lv_obj_set_style_bg_color(box_labels_[i], color(o.color), 0);
        }
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}

std::size_t Screens::visible_boxes() const
{
    std::size_t n = 0;
    for (lv_obj_t *b : boxes_) {
        n += lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) ? 0U : 1U;
    }
    return n;
}

lv_obj_t *Screens::nav_button(Page target) const
{
    return nav_buttons_[idx(target)];
}

lv_obj_t *Screens::back_button(Page page) const
{
    return back_buttons_[idx(page)];
}

bool Screens::page_visible(Page p) const
{
    return !lv_obj_has_flag(pages_[idx(p)], LV_OBJ_FLAG_HIDDEN);
}

}  // namespace rksec::ui
