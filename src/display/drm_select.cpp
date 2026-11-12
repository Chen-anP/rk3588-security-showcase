#include "display/drm_select.hpp"

#include <array>
#include <cstdio>

namespace rksec::display {

std::optional<std::string> choose_card(const std::vector<CardInfo> &cards, std::string_view device,
                                       std::string_view want_driver, std::string &why)
{
    std::string seen;
    for (const auto &c : cards) {
        seen += (seen.empty() ? "" : "、") + c.path + "（" + (c.driver.empty() ? "驱动名未知" : c.driver) + "）";
    }
    if (seen.empty()) {
        seen = "无";
    }

    if (device != "auto") {
        for (const auto &c : cards) {
            if (c.path == device) {
                return c.path;
            }
        }
        why = "配置的显示设备 " + std::string{device} + " 不存在或打不开；看到的设备：" + seen;
        return std::nullopt;
    }
    for (const auto &c : cards) {
        if (c.driver == want_driver) {
            return c.path;
        }
    }
    why = "没有驱动名为 " + std::string{want_driver} + " 的显示设备；看到的设备：" + seen;
    return std::nullopt;
}

double refresh_hz(const ModeInfo &m)
{
    const double total = static_cast<double>(m.htotal) * static_cast<double>(m.vtotal);
    if (total <= 0) {
        return 0;
    }
    return static_cast<double>(m.clock_khz) * 1000.0 / total;
}

std::string describe(const ModeInfo &m)
{
    std::array<char, 128> buf{};
    std::snprintf(buf.data(), buf.size(), "%ux%u %.2fHz 时钟 %ukHz 总 %ux%u", static_cast<unsigned>(m.hdisplay),
                  static_cast<unsigned>(m.vdisplay), refresh_hz(m), static_cast<unsigned>(m.clock_khz),
                  static_cast<unsigned>(m.htotal), static_cast<unsigned>(m.vtotal));
    return buf.data();
}

std::string short_describe(const ModeInfo &m)
{
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%ux%u %.2fHz", static_cast<unsigned>(m.hdisplay),
                  static_cast<unsigned>(m.vdisplay), refresh_hz(m));
    return buf.data();
}

std::optional<std::size_t> choose_connector(const std::vector<ConnectorInfo> &conns, std::string_view name,
                                            std::string &why)
{
    std::string seen;
    for (const auto &c : conns) {
        seen += (seen.empty() ? "" : "、") + c.name + (c.connected ? "（已连接）" : "（未连接）");
    }
    if (seen.empty()) {
        seen = "无";
    }

    for (std::size_t i = 0; i < conns.size(); ++i) {
        const auto &c = conns[i];
        if (name == "auto") {
            if (c.connected && c.name.rfind("Writeback", 0) != 0) {
                return i;
            }
        } else if (c.name == name) {
            if (!c.connected) {
                why = "输出口 " + c.name + " 没有连接屏幕（检查线缆）";
                return std::nullopt;
            }
            return i;
        }
    }
    why = name == "auto" ? "没有已连接屏幕的输出口；看到的输出口：" + seen
                         : "没有名为 " + std::string{name} + " 的输出口；看到的输出口：" + seen;
    return std::nullopt;
}

std::optional<ModeChoice> choose_mode(const ConnectorInfo &conn)
{
    if (conn.current_mode) {
        return ModeChoice{*conn.current_mode, "沿用内核当前模式（由启动参数 video= 决定）"};
    }
    for (const auto &m : conn.modes) {
        if (m.preferred) {
            return ModeChoice{m, "输出口当前没有在用的模式，用 preferred 模式"};
        }
    }
    if (!conn.modes.empty()) {
        return ModeChoice{conn.modes.front(), "没有 preferred 模式，用列表第一个"};
    }
    return std::nullopt;
}

std::optional<std::uint64_t> want_rgb(const std::vector<EnumValue> &enums, std::uint64_t current)
{
    for (const auto &e : enums) {
        if (e.name == "rgb") {
            if (e.value == current) {
                return std::nullopt;
            }
            return e.value;
        }
    }
    return std::nullopt;
}

}  // namespace rksec::display
