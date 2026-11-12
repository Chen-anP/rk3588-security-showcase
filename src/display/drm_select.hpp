#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 选择显卡、输出口和显示模式的规则。只做判断，不碰设备，所以能在 CI 上测试。
//
// 第 4a 步摸底的结论（见 docs 里的 D14）：
// - /dev/dri 下有两张“卡”：card0 是 rockchip-drm（显示），card1 是 RKNPU（NPU 也注册成了 DRM 设备）。
//   卡号由驱动加载顺序决定，换内核可能变，所以按驱动名选，不写死 card0。
// - 屏幕读不到 EDID，内核按启动参数 video=HDMI-A-1:1024x600MR@60e 设好了正确时序（43.833 MHz）。
//   但输出口的模式列表里第一个（preferred）是另一组时序（GTF，50.25 MHz，会闪）。
//   所以默认“沿用内核当前正在用的模式”，不自己从列表里挑。
namespace rksec::display {

struct CardInfo {
    std::string path;    // /dev/dri/card0
    std::string driver;  // drmGetVersion 返回的驱动名，比如 "rockchip"、"rknpu"
};

// device：配置里的 display.device，"auto" 或具体路径。
// auto：选第一张驱动名是 want_driver 的卡。找不到返回 nullopt，why 里是原因（带上看到了哪些卡）
std::optional<std::string> choose_card(const std::vector<CardInfo> &cards, std::string_view device,
                                       std::string_view want_driver, std::string &why);

struct ModeInfo {
    std::string name;  // "1024x600"
    std::uint32_t clock_khz = 0;
    std::uint16_t hdisplay = 0, hsync_start = 0, hsync_end = 0, htotal = 0;
    std::uint16_t vdisplay = 0, vsync_start = 0, vsync_end = 0, vtotal = 0;
    bool preferred = false;
    std::size_t index = 0;  // 在输出口模式列表里的位置（current_mode 不在列表里时无意义）
};

// 实际刷新率 = 像素时钟 ÷ (行总长 × 帧总行数)。比模式里写的“60”更真实（D14：1024x600 实际 59.81 Hz）
double refresh_hz(const ModeInfo &m);

// 一行可读描述，写日志用："1024x600 59.81Hz 时钟 43833kHz 总 1184x619"
std::string describe(const ModeInfo &m);

// 给界面显示的简短描述："1024x600 59.81Hz"（只有 ASCII，不依赖界面字体里有哪些中文，D17）
std::string short_describe(const ModeInfo &m);

struct ConnectorInfo {
    std::uint32_t id = 0;
    std::string name;  // "HDMI-A-1"、"Writeback-1"
    bool connected = false;
    std::vector<ModeInfo> modes;
    std::optional<ModeInfo> current_mode;  // 已经有 CRTC 在驱动它时，CRTC 当前的模式
};

// name：配置里的 display.connector，"auto" 或输出口名字。
// auto：第一个已连接、而且不是 Writeback（回写，不是真的屏幕）的输出口。
std::optional<std::size_t> choose_connector(const std::vector<ConnectorInfo> &conns, std::string_view name,
                                            std::string &why);

struct ModeChoice {
    ModeInfo mode;
    std::string reason;  // 为什么选它，写进日志
};

// 有当前模式就沿用；没有就用 preferred；再没有就用列表第一个。一个模式都没有返回 nullopt
std::optional<ModeChoice> choose_mode(const ConnectorInfo &conn);

// HDMI 输出的颜色格式（Rockchip 驱动的连接器属性 color_format，枚举 rgb / ycbcr444 / …）。
// 第 4b 步上屏发现（D16）：屏读不到 EDID 时驱动仍选了 ycbcr444，这块屏只认 RGB，整个画面偏蓝（控制台也是）。
// HDMI 规范要求所有显示设备都支持 RGB，所以一律改成 rgb。
struct EnumValue {
    std::string name;
    std::uint64_t value = 0;
};
// 需要改：返回 rgb 对应的值；已经是 rgb、或者属性里没有 rgb 这一项：返回 nullopt（不动）
std::optional<std::uint64_t> want_rgb(const std::vector<EnumValue> &enums, std::uint64_t current);

}  // namespace rksec::display
