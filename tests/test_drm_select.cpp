#include <gtest/gtest.h>

#include "display/drm_select.hpp"

using namespace rksec::display;

namespace {

// 第 4a 步在板子上看到的情况
const std::vector<CardInfo> kBoardCards = {{"/dev/dri/card0", "rockchip"}, {"/dev/dri/card1", "rknpu"}};

// D14：内核当前用的是 CVT 减少消隐时序（43.833 MHz）；模式列表第一个（preferred）是会闪的 GTF 时序
ModeInfo cvt_rb()
{
    ModeInfo m;
    m.name = "1024x600";
    m.clock_khz = 43833;
    m.hdisplay = 1024;
    m.hsync_start = 1072;
    m.hsync_end = 1104;
    m.htotal = 1184;
    m.vdisplay = 600;
    m.vsync_start = 603;
    m.vsync_end = 613;
    m.vtotal = 619;
    return m;
}

ModeInfo gtf_preferred()
{
    ModeInfo m;
    m.name = "1024x600";
    m.clock_khz = 50250;
    m.hdisplay = 1024;
    m.htotal = 1344;
    m.vdisplay = 600;
    m.vtotal = 625;
    m.preferred = true;
    m.index = 0;
    return m;
}

ConnectorInfo hdmi(bool connected)
{
    ConnectorInfo c;
    c.id = 215;
    c.name = "HDMI-A-1";
    c.connected = connected;
    c.modes = {gtf_preferred()};
    return c;
}

ConnectorInfo writeback()
{
    ConnectorInfo c;
    c.id = 200;
    c.name = "Writeback-1";
    c.connected = true;  // 回写口总是报告“已连接”
    return c;
}

}  // namespace

TEST(DrmSelect, AutoPicksDisplayCardNotNpu)
{
    std::string why;
    // 卡号顺序反过来（换内核后可能发生）也要选对
    const std::vector<CardInfo> swapped = {{"/dev/dri/card0", "rknpu"}, {"/dev/dri/card1", "rockchip"}};
    EXPECT_EQ(choose_card(kBoardCards, "auto", "rockchip", why), "/dev/dri/card0");
    EXPECT_EQ(choose_card(swapped, "auto", "rockchip", why), "/dev/dri/card1");
}

TEST(DrmSelect, ExplicitCardPath)
{
    std::string why;
    EXPECT_EQ(choose_card(kBoardCards, "/dev/dri/card1", "rockchip", why), "/dev/dri/card1");
    EXPECT_FALSE(choose_card(kBoardCards, "/dev/dri/card5", "rockchip", why));
    EXPECT_NE(why.find("/dev/dri/card5"), std::string::npos) << why;
}

TEST(DrmSelect, NoDisplayCardExplainsWhatWasSeen)
{
    std::string why;
    EXPECT_FALSE(choose_card({{"/dev/dri/card0", "rknpu"}}, "auto", "rockchip", why));
    EXPECT_NE(why.find("card0（rknpu）"), std::string::npos) << why;
    EXPECT_FALSE(choose_card({}, "auto", "rockchip", why));
    EXPECT_NE(why.find("无"), std::string::npos) << why;
}

TEST(DrmSelect, AutoConnectorSkipsWriteback)
{
    std::string why;
    EXPECT_EQ(choose_connector({writeback(), hdmi(true)}, "auto", why), 1U);
}

TEST(DrmSelect, AutoConnectorNothingConnected)
{
    std::string why;
    EXPECT_FALSE(choose_connector({writeback(), hdmi(false)}, "auto", why));
    EXPECT_NE(why.find("HDMI-A-1（未连接）"), std::string::npos) << why;
}

TEST(DrmSelect, NamedConnector)
{
    std::string why;
    EXPECT_EQ(choose_connector({writeback(), hdmi(true)}, "HDMI-A-1", why), 1U);
    EXPECT_FALSE(choose_connector({writeback(), hdmi(false)}, "HDMI-A-1", why));
    EXPECT_NE(why.find("没有连接屏幕"), std::string::npos) << why;
    EXPECT_FALSE(choose_connector({hdmi(true)}, "DSI-1", why));
    EXPECT_NE(why.find("没有名为 DSI-1"), std::string::npos) << why;
}

TEST(DrmSelect, KeepsKernelCurrentModeOverPreferred)
{
    // D14：不能选列表里的 preferred，那组时序 PLL 给不出来，屏幕会闪
    ConnectorInfo c = hdmi(true);
    c.current_mode = cvt_rb();
    const auto choice = choose_mode(c);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->mode.clock_khz, 43833U);
    EXPECT_EQ(choice->mode.htotal, 1184);
    EXPECT_NE(choice->reason.find("沿用内核当前模式"), std::string::npos);
}

TEST(DrmSelect, FallsBackToPreferredThenFirst)
{
    ConnectorInfo c = hdmi(true);
    ModeInfo other = cvt_rb();
    other.index = 1;
    c.modes = {other, gtf_preferred()};
    c.modes[1].index = 1;
    auto choice = choose_mode(c);
    ASSERT_TRUE(choice);
    EXPECT_TRUE(choice->mode.preferred);

    c.modes = {other};
    choice = choose_mode(c);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->mode.clock_khz, 43833U);

    c.modes.clear();
    EXPECT_FALSE(choose_mode(c));
}

TEST(DrmSelect, RefreshRateFromTiming)
{
    // 43833000 / (1184 × 619) = 59.81 Hz；GTF 时序要求 50.25 MHz 时 = 59.82 Hz，PLL 只给 48.924 MHz 时 ≈ 58.2 Hz
    EXPECT_NEAR(refresh_hz(cvt_rb()), 59.81, 0.01);
    ModeInfo actual = gtf_preferred();
    actual.clock_khz = 48924;
    EXPECT_NEAR(refresh_hz(actual), 58.25, 0.01);
    EXPECT_EQ(refresh_hz(ModeInfo{}), 0.0);
    EXPECT_EQ(describe(cvt_rb()), "1024x600 59.81Hz 时钟 43833kHz 总 1184x619");
    // 界面上用的简短描述只有 ASCII，不受界面字体缺字影响（D17）
    const std::string s = short_describe(cvt_rb());
    EXPECT_EQ(s, "1024x600 59.81Hz");
    for (const char c : s) {
        EXPECT_LT(static_cast<unsigned char>(c), 0x80);
    }
}

TEST(DrmSelect, ForcesRgbOutputWhenDriverChoseYcbcr)
{
    // 板子上实际的枚举（modetest -c 的 color_format）
    const std::vector<EnumValue> e = {
        {"rgb", 0}, {"ycbcr444", 1}, {"ycbcr422", 2}, {"ycbcr420", 3}, {"ycbcr_high_subsampling", 4}};
    EXPECT_EQ(want_rgb(e, 1), std::optional<std::uint64_t>{0}) << "D16：驱动选了 ycbcr444，要改成 rgb";
    EXPECT_FALSE(want_rgb(e, 0)) << "已经是 rgb 就不动（不触发多余的模式设置）";
    EXPECT_FALSE(want_rgb({{"auto", 0}, {"ycbcr444", 1}}, 1)) << "没有 rgb 这一项（别的驱动）就不动";
    EXPECT_FALSE(want_rgb({}, 0));
}
