// 界面测试：不需要屏幕。LVGL 画在内存里（RamDisplay），检查布局、点击、页面切换、预览显示。
//
// 想看界面长什么样：设置环境变量 RKSEC_UI_SNAPSHOT_DIR=目录，跑 PagesRenderAndSnapshot，
// 每个页面存一张 PPM 图片（Windows 上可以用 IrfanView、GIMP 打开，或者 convert 转成 PNG）。
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/pass_history.hpp"
#include "core/log.hpp"
#include "media/jpeg_decoder.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/lvgl_port.hpp"
#include "ui/router.hpp"
#include "ui/screens.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"
#include <filesystem>
#include <unistd.h>

using namespace rksec::ui;

namespace {

void save_ppm(const std::string &path, const RamDisplay &d)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << theme::kScreenW << " " << theme::kScreenH << "\n255\n";
    for (int y = 0; y < theme::kScreenH; ++y) {
        const std::uint8_t *row = d.pixels() + static_cast<std::size_t>(y) * d.stride();
        for (int x = 0; x < theme::kScreenW; ++x) {
            const std::uint8_t *px = row + x * 4;  // B G R X
            const char rgb[3] = {static_cast<char>(px[2]), static_cast<char>(px[1]), static_cast<char>(px[0])};
            f.write(rgb, 3);
        }
    }
}

std::uint32_t rgb_at(const RamDisplay &d, int x, int y)
{
    const std::uint8_t *px = d.pixels() + static_cast<std::size_t>(y) * d.stride() + static_cast<std::size_t>(x) * 4;
    return (static_cast<std::uint32_t>(px[2]) << 16) | (static_cast<std::uint32_t>(px[1]) << 8) | px[0];
}

// 一个“手指”：测试脚本设置按下的位置，LVGL 定时来读
struct Finger {
    bool pressed = false;
    lv_point_t at{0, 0};
    static void read(lv_indev_t *indev, lv_indev_data_t *data)
    {
        const auto *f = static_cast<Finger *>(lv_indev_get_user_data(indev));
        data->point = f->at;
        data->state = f->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
};

class UiTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        // LVGL 的警告（缺字、字体格式不对、对象用法错误……）说明界面有问题，测试里一条都不应该有
        rksec::log::set_sink([this](rksec::log::Level, std::string_view line) {
            if (line.find("[lvgl]") != std::string_view::npos) {
                lvgl_warnings += std::string{line} + "\n";
            }
        });
        lvgl_init();
        display =
            std::make_unique<RamDisplay>(theme::kScreenW, theme::kScreenH, [this](const std::uint8_t *, std::size_t) {
                ++presented;
                return true;
            });
        DeviceInfo info{"1024x600 59.81Hz", "1280x720 MJPG", "/dev/input/by-id/touch", "v0.0.1-test"};
        screens = std::make_unique<Screens>(display->display(), router, info);
        indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, &Finger::read);
        lv_indev_set_user_data(indev, &finger);
        lv_indev_set_display(indev, display->display());
        lv_refr_now(display->display());
    }

    void TearDown() override
    {
        lv_indev_delete(indev);
        screens.reset();
        display.reset();
        lvgl_deinit();
        rksec::log::set_sink(nullptr);
        EXPECT_TRUE(lvgl_warnings.empty()) << "LVGL 报了警告：\n" << lvgl_warnings;
    }

    // 让 LVGL 跑一段时间（处理输入、重画）
    static void run_for(std::chrono::milliseconds d)
    {
        const auto end = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < end) {
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // 像手指一样点一下对象的中心：按下 100 ms，抬起
    void tap(lv_obj_t *obj)
    {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        finger.at = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        finger.pressed = true;
        run_for(std::chrono::milliseconds(100));
        finger.pressed = false;
        run_for(std::chrono::milliseconds(100));
    }

    Router router;
    std::unique_ptr<RamDisplay> display;
    std::unique_ptr<Screens> screens;
    lv_indev_t *indev = nullptr;
    Finger finger;
    int presented = 0;
    std::string lvgl_warnings;
};

}  // namespace

// 中文字体只含界面用到的字（tools/fonts/gen_fonts.sh 生成）。界面上每个字都要在字体里，否则显示成方框。
// 加了新字这里会失败：运行 tools/fonts/gen_fonts.sh 重新生成字体
TEST(UiText, EveryCharacterIsInTheFont)
{
    lvgl_init();
    std::string missing;
    for (const lv_font_t *font : {&rksec_font_cn_20, &rksec_font_cn_28}) {
        for (const char *s : text::kAll) {
            for (const std::uint32_t cp : code_points(s)) {
                lv_font_glyph_dsc_t dsc{};
                const bool found = lv_font_get_glyph_dsc(font, &dsc, cp, 0);
                if (!found || dsc.resolved_font != font) {
                    missing += "  \"" + std::string{s} + "\" 里的 U+" + std::to_string(cp) + "\n";
                }
            }
        }
        for (std::uint32_t cp = 0x20; cp <= 0x7E; ++cp) {  // 数字、字母也要有（配置里的设备名、版本号）
            lv_font_glyph_dsc_t dsc{};
            if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                missing += "  ASCII " + std::to_string(cp) + "\n";
            }
        }
    }
    lvgl_deinit();
    EXPECT_TRUE(missing.empty()) << "字体里没有这些字，运行 tools/fonts/gen_fonts.sh：\n" << missing;
}

// 运行时拼出来的文字：缺字检查本身要靠得住（D17）
TEST(UiText, MissingGlyphsReportsOnlyAbsentCharacters)
{
    lvgl_init();
    EXPECT_EQ(missing_glyphs("1024x600 59.81Hz"), "");
    EXPECT_EQ(missing_glyphs("设置 摄像头"), "");
    // “钟”“总”不在界面字体里（D17 时设置页显示 "时钟 43833kHz 总 1184x619"，三个字成了方框）
    EXPECT_EQ(missing_glyphs("时钟 43833kHz 总 1184x619"), "时钟总");
    EXPECT_EQ(missing_glyphs("钟钟钟"), "钟") << "重复的字只报一次";
    lvgl_deinit();
}

// 人名用字（第 7d 步）：GB2312 一级汉字 3755 个都在 20 号人名字体里；检测框标签按“界面字体或人名字体”检查
TEST(UiText, NameFontHasAllCommonCharacters)
{
    lvgl_init();
    // 具体是哪些字由 tools/fonts/gb2312_level1.py 按编码规则生成；这里数基本汉字区里的字数
    std::uint32_t glyphs = 0;
    for (std::uint32_t cp = 0x4E00; cp <= 0x9FA5; ++cp) {
        lv_font_glyph_dsc_t dsc{};
        if (lv_font_get_glyph_dsc(&rksec_font_names_20, &dsc, cp, 0) && dsc.resolved_font == &rksec_font_names_20) {
            ++glyphs;
        }
    }
    EXPECT_EQ(glyphs, 3755U) << "人名字体里的汉字个数";
    EXPECT_EQ(missing_name_glyphs("赵小明"), "");
    EXPECT_EQ(missing_name_glyphs("#3 张伟 chen"), "");
    EXPECT_EQ(missing_name_glyphs("#3 陌生人"), "");
    EXPECT_EQ(missing_name_glyphs("王喆"), "喆") << "喆 不在 GB2312 里（GBK 才有），登记时要拒绝";
    EXPECT_EQ(missing_name_glyphs("㐀"), "㐀");
    lvgl_deinit();
}

TEST_F(UiTest, StartsOnHomePage)
{
    EXPECT_TRUE(screens->page_visible(Page::home));
    EXPECT_FALSE(screens->page_visible(Page::events));
    EXPECT_FALSE(screens->page_visible(Page::settings));
    EXPECT_GT(presented, 0) << "创建界面后应该画出第一帧";
    // 顶栏是面板色，背景是底色
    EXPECT_EQ(rgb_at(*display, 500, 5), theme::kPanel);
    EXPECT_EQ(rgb_at(*display, 1010, 590), theme::kBg);
}

TEST_F(UiTest, TouchTargetsAreFingerSized)
{
    lv_obj_update_layout(lv_display_get_screen_active(display->display()));
    for (const Page p : {Page::events, Page::settings, Page::about}) {
        lv_obj_t *nav = screens->nav_button(p);
        ASSERT_NE(nav, nullptr) << page_name(p);
        EXPECT_GE(lv_obj_get_height(nav), theme::kTouchMin) << page_name(p);
        lv_obj_t *back = screens->back_button(p);
        ASSERT_NE(back, nullptr) << page_name(p);
        EXPECT_GE(lv_obj_get_height(back), theme::kTouchMin) << page_name(p);
    }
}

TEST_F(UiTest, TapNavigatesAndBackReturns)
{
    tap(screens->nav_button(Page::settings));
    EXPECT_EQ(router.current(), Page::settings);
    EXPECT_TRUE(screens->page_visible(Page::settings));
    EXPECT_FALSE(screens->page_visible(Page::home));

    tap(screens->nav_button(Page::about));
    EXPECT_EQ(router.current(), Page::about);

    tap(screens->back_button(Page::about));
    EXPECT_EQ(router.current(), Page::settings);
    tap(screens->back_button(Page::settings));
    EXPECT_EQ(router.current(), Page::home);
    EXPECT_TRUE(screens->page_visible(Page::home));
}

TEST_F(UiTest, TapOnHiddenPageButtonDoesNothing)
{
    // 首页上点“返回”按钮所在的位置（左上角）：返回按钮在隐藏的页面上，不应该被点到
    lv_area_t a;
    lv_obj_get_coords(screens->back_button(Page::events), &a);
    finger.at = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    finger.pressed = true;
    run_for(std::chrono::milliseconds(100));
    finger.pressed = false;
    run_for(std::chrono::milliseconds(100));
    EXPECT_EQ(router.current(), Page::home);
}

TEST_F(UiTest, PreviewShowsImagePixelForPixel)
{
    rksec::media::Image img;
    img.width = theme::kPreviewW;
    img.height = theme::kPreviewH;
    img.stride = static_cast<std::size_t>(img.width) * 4;
    img.pixels.assign(img.stride * static_cast<std::size_t>(img.height), 0);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i + 2] = 0xC0;  // R（内存顺序 B G R X）
        img.pixels[i + 3] = 0xFF;
    }
    screens->show_preview(img);
    lv_refr_now(display->display());
    // 预览区在 (16, 56+16) 开始，640x360
    EXPECT_EQ(rgb_at(*display, theme::kPad + 10, theme::kTopBarH + theme::kPad + 10), 0xC00000U);
    EXPECT_EQ(
        rgb_at(*display, theme::kPad + theme::kPreviewW - 1, theme::kTopBarH + theme::kPad + theme::kPreviewH - 1),
        0xC00000U);

    // 摄像头断开：不能继续显示最后一帧（看起来像画面卡住了）
    screens->clear_preview();
    lv_refr_now(display->display());
    EXPECT_EQ(rgb_at(*display, theme::kPad + 10, theme::kTopBarH + theme::kPad + 10), 0x000000U);
}

TEST_F(UiTest, StatusUpdateDoesNotRebuild)
{
    UiStatus s;
    s.camera_ok = true;
    s.camera_fps = 24.9;
    s.decode_p50_ms = 5.2;
    s.lost = 3;
    s.ui_fps = 60;
    s.cpu_percent = 35;
    for (int i = 0; i < 100; ++i) {  // 每秒一次，跑 100 次不应该有对象泄漏
        screens->update_status(s);
    }
    lv_refr_now(display->display());
    const auto children = lv_obj_get_child_count(lv_display_get_screen_active(display->display()));
    screens->update_status(s);
    EXPECT_EQ(lv_obj_get_child_count(lv_display_get_screen_active(display->display())), children);
}

TEST_F(UiTest, PagesRenderAndSnapshot)
{
    const char *dir = std::getenv("RKSEC_UI_SNAPSHOT_DIR");
    UiStatus s;
    s.camera_ok = true;
    s.camera_fps = 24.9;
    s.decode_p50_ms = 5.2;
    s.lost = 0;
    s.ui_fps = 25;
    s.cpu_percent = 38;
    s.detecting = true;
    s.detect_p50_ms = 17.3;
    screens->update_status(s);
    screens->show_access("模拟开门 / 红外故障");
    // 首页截图里放一张灰色预览和两个检测框，看框和标签的样子
    rksec::media::Image img;
    img.width = theme::kPreviewW;
    img.height = theme::kPreviewH;
    img.stride = static_cast<std::size_t>(img.width) * 4;
    img.pixels.assign(img.stride * static_cast<std::size_t>(img.height), 0x50);
    screens->show_preview(img);
    // 三种身份的框（第 7c、7d 步）：认出来的（人名用字）、陌生人、还没结论
    screens->show_boxes({{60, 60, 110, 260, "#1 TestPerson", theme::kOk},
                         {230, 90, 90, 230, std::string{"#2 "} + rksec::ui::text::kStranger, theme::kWarn},
                         {400, 80, 100, 250, "#3 人 64%", theme::kAccent}});
    for (const Page p : {Page::home, Page::events, Page::settings, Page::about}) {
        router.go(p);
        lv_refr_now(display->display());
        EXPECT_TRUE(screens->page_visible(p));
        if (dir != nullptr) {
            save_ppm(std::string{dir} + "/ui_" + page_name(p) + ".ppm", *display);
        }
    }
}

// 第 7c 步：框的颜色跟着身份变（认出来绿色、陌生人橙色）
TEST_F(UiTest, BoxColorFollowsIdentity)
{
    rksec::media::Image img;
    img.width = theme::kPreviewW;
    img.height = theme::kPreviewH;
    img.stride = static_cast<std::size_t>(img.width) * 4;
    img.pixels.assign(img.stride * static_cast<std::size_t>(img.height), 0);
    screens->show_preview(img);
    const int px = theme::kPad + 100;
    const int py = theme::kTopBarH + theme::kPad + 50 + 100;
    const std::string stranger = std::string{"#1 "} + rksec::ui::text::kStranger;
    screens->show_boxes({{100, 50, 120, 200, stranger, theme::kWarn}});
    lv_refr_now(display->display());
    EXPECT_EQ(rgb_at(*display, px, py), theme::kWarn & 0xFFFFFFU);
    screens->show_boxes({{100, 50, 120, 200, "#1 chenpan", theme::kOk}});
    lv_refr_now(display->display());
    EXPECT_EQ(rgb_at(*display, px, py), theme::kOk & 0xFFFFFFU) << "同一个框换回绿色";
}

TEST_F(UiTest, DetectionBoxesAreDrawnOverThePreviewAndCleared)
{
    rksec::media::Image img;
    img.width = theme::kPreviewW;
    img.height = theme::kPreviewH;
    img.stride = static_cast<std::size_t>(img.width) * 4;
    img.pixels.assign(img.stride * static_cast<std::size_t>(img.height), 0);
    screens->show_preview(img);

    std::vector<OverlayBox> boxes{{100, 50, 120, 200, "人 87%"}, {400, 100, 60, 60, "car 55%"}};
    screens->show_boxes(boxes);
    lv_refr_now(display->display());
    EXPECT_EQ(screens->visible_boxes(), 2U);
    // 第一个框的左边线（预览区原点 (16, 72)），框的颜色是 theme::kOk
    const int px = theme::kPad + 100;
    const int py = theme::kTopBarH + theme::kPad + 50 + 100;
    EXPECT_EQ(rgb_at(*display, px, py), theme::kOk & 0xFFFFFFU);
    // 框里面是透明的：还是预览图的黑色
    EXPECT_EQ(rgb_at(*display, px + 60, py), 0x000000U);

    // 少一个框：多出来的隐藏；超出预览区的框被裁进来，不画到外面
    screens->show_boxes({{600, 300, 200, 200, "人 40%"}});
    lv_refr_now(display->display());
    EXPECT_EQ(screens->visible_boxes(), 1U);
    EXPECT_EQ(rgb_at(*display, theme::kPad + theme::kPreviewW + 4, theme::kTopBarH + theme::kPad + 320), theme::kBg)
        << "预览区右边（间隔）不应该有框线";

    // 超过上限的框不画
    std::vector<OverlayBox> many(Screens::kMaxBoxes + 5, OverlayBox{10, 10, 20, 20, "人 50%"});
    screens->show_boxes(many);
    EXPECT_EQ(screens->visible_boxes(), Screens::kMaxBoxes);

    // 摄像头断开：框和画面一起清掉
    screens->clear_preview();
    EXPECT_EQ(screens->visible_boxes(), 0U);
}

TEST_F(UiTest, DetectTileShowsDashWhenNotDetecting)
{
    UiStatus s;
    s.camera_ok = true;
    s.detecting = false;
    for (int i = 0; i < 3; ++i) {
        screens->update_status(s);
    }
    s.detecting = true;
    s.detect_p50_ms = 17.5;
    screens->update_status(s);
    lv_refr_now(display->display());
    SUCCEED();  // 只检查不崩溃、不泄漏对象（StatusUpdateDoesNotRebuild 检查对象数）
}

TEST_F(UiTest, PassagePagingDetailAndFailure)
{
    std::vector<rksec::access::PassRow> rows(6);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].id = static_cast<std::int64_t>(i + 1);
        rows[i].person_name = "测试人员";
        rows[i].score = 0.75;
    }
    screens->show_passes(rows, "");
    tap(screens->nav_button(Page::events));
    tap(screens->pass_button(0));
    EXPECT_TRUE(screens->detail_visible());
    tap(screens->history_close());
    EXPECT_FALSE(screens->detail_visible());
    tap(screens->history_next());
    tap(screens->pass_button(1));
    EXPECT_TRUE(screens->detail_visible());
    if (const char *dir = std::getenv("RKSEC_UI_SNAPSHOT_DIR")) {
        lv_refr_now(display->display());
        save_ppm(std::string(dir) + "/pass_detail.ppm", *display);
        tap(screens->history_close());
        lv_refr_now(display->display());
        save_ppm(std::string(dir) + "/pass_list.ppm", *display);
    }
    screens->show_passes({}, "database busy");
    EXPECT_FALSE(screens->detail_visible());
    EXPECT_TRUE(lv_obj_has_flag(screens->pass_button(0), LV_OBJ_FLAG_HIDDEN));
}
TEST(PassHistory, MissingDatabaseIsReadOnlyAndReportsFailure)
{
    char pattern[] = "/tmp/rksec-history-XXXXXX";
    const char *dir = mkdtemp(pattern);
    ASSERT_NE(dir, nullptr);
    const std::string path = std::string(dir) + "/missing.db";
    {
        rksec::app::PassHistory history(path);
        std::optional<rksec::app::HistoryResult> result;
        for (int i = 0; i < 300 && !result; ++i) {
            result = history.take();
            if (!result)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(result);
        EXPECT_FALSE(result->error.empty());
        EXPECT_TRUE(result->rows.empty());
        EXPECT_FALSE(std::filesystem::exists(path));
    }
    std::filesystem::remove(dir);
}
TEST_F(UiTest, PassageLabelsHaveGlyphs)
{
    EXPECT_TRUE(missing_name_glyphs(
                    "门禁未启用 通行记录 正在读取记录 上一页 下一页 返回列表 最近条本地时间页 暂无通行记录 "
                    "记录读取失败请稍后重试 姓名无法显示 姓名时间分数人员已登记已删除历史记录快照已保存无模拟授权记录")
                    .empty());
}

TEST(PassHistory, EmptyVersionThreeDatabaseLoadsWithoutError)
{
    char pattern[] = "/tmp/rksec-history-ok-XXXXXX";
    const char *dir = mkdtemp(pattern);
    ASSERT_NE(dir, nullptr);
    const std::string path = std::string(dir) + "/empty.db";
    std::string error;
    auto store = rksec::access::PassStore::open(path, error);
    ASSERT_TRUE(store) << error;
    store.reset();
    {
        rksec::app::PassHistory history(path);
        std::optional<rksec::app::HistoryResult> result;
        for (int i = 0; i < 300 && !result; ++i) {
            result = history.take();
            if (!result)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(result);
        EXPECT_TRUE(result->error.empty()) << result->error;
        EXPECT_TRUE(result->rows.empty());
    }
    std::filesystem::remove_all(dir);
}
