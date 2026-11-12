#include "face/score_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rksec::face {

namespace {

constexpr int kBarWidth = 30;
constexpr int kGroupBins = 5;  // 分布图每格 0.05

// 两位小数（分数）
std::string f2(double v)
{
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.2f", v);
    return buf.data();
}

// 百分比（整数）
std::string pct(double ratio)
{
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.0f%%", ratio * 100.0);
    return buf.data();
}

// 一组的一行：“正脸 80 次 P50 0.55 认出 92%”
std::string group_text(const char *name, const ScoreStats::Hist &h, float threshold)
{
    if (h.n == 0) {
        return {};
    }
    std::string s = name;
    s += " " + std::to_string(h.n) + " 次";
    s += " P50 " + f2(static_cast<double>(h.percentile(0.5))) + " 认出 " + pct(h.ratio_at_least(threshold));
    return s;
}

void append_groups(std::string &out, const char *title, const std::string *items, std::size_t count)
{
    std::string line;
    for (std::size_t i = 0; i < count; ++i) {
        if (items[i].empty()) {
            continue;
        }
        line += line.empty() ? "" : "；";
        line += items[i];
    }
    if (!line.empty()) {
        out += "  ";
        out += title;
        out += "  " + line + "\n";
    }
}

}  // namespace

void ScoreStats::Hist::add(float v)
{
    const int bin = std::clamp(static_cast<int>(std::floor(v * 100.0F + 1e-4F)), 0, kBins - 1);
    ++bins[static_cast<std::size_t>(bin)];
    ++n;
}

float ScoreStats::Hist::percentile(double p) const
{
    if (n == 0) {
        return 0;
    }
    // 第 ceil(p × n) 个（至少第 1 个）样本所在的格
    const auto rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(n))));
    std::uint64_t seen = 0;
    for (int i = 0; i < kBins; ++i) {
        seen += bins[static_cast<std::size_t>(i)];
        if (seen >= rank) {
            return static_cast<float>(i) / 100.0F;
        }
    }
    return 1.0F;
}

double ScoreStats::Hist::ratio_at_least(float threshold) const
{
    if (n == 0) {
        return 0;
    }
    const int from = std::clamp(static_cast<int>(std::ceil(threshold * 100.0F - 1e-4F)), 0, kBins);
    std::uint64_t k = 0;
    for (int i = from; i < kBins; ++i) {
        k += bins[static_cast<std::size_t>(i)];
    }
    return static_cast<double>(k) / static_cast<double>(n);
}

int ScoreStats::yaw_group(float yaw)
{
    if (yaw < -0.15F) {
        return 1;
    }
    return yaw > 0.15F ? 2 : 0;
}

int ScoreStats::pitch_group(float pitch)
{
    if (pitch < 0.1F) {
        return 1;
    }
    return pitch > 0.45F ? 2 : 0;
}

int ScoreStats::size_group(float width)
{
    if (width < 100) {
        return 0;
    }
    if (width < 200) {
        return 1;
    }
    return width < 300 ? 2 : 3;
}

void ScoreStats::add(const ScoreSample &s)
{
    all_.add(s.score);
    if (s.pose_valid) {
        yaw_[static_cast<std::size_t>(yaw_group(s.yaw))].add(s.score);
        pitch_[static_cast<std::size_t>(pitch_group(s.pitch))].add(s.score);
    }
    size_[static_cast<std::size_t>(size_group(s.width))].add(s.score);
    if (s.second >= 0) {
        margin_.add(s.score - s.second);
    }
}

std::string ScoreStats::report(float threshold) const
{
    if (all_.n == 0) {
        return "识别分数   没有样本（没找到过脸）\n";
    }
    std::string out = "识别分数   有脸 " + std::to_string(all_.n) + " 次：P10 " +
                      f2(static_cast<double>(all_.percentile(0.1))) + "  P50 " +
                      f2(static_cast<double>(all_.percentile(0.5))) + "  P90 " +
                      f2(static_cast<double>(all_.percentile(0.9))) + "；≥ 阈值 " + f2(static_cast<double>(threshold)) +
                      " 的 " + pct(all_.ratio_at_least(threshold)) + "\n";

    // 分布：每格 0.05，从有样本的第一格到最后一格
    constexpr int kGroups = (Hist::kBins + kGroupBins - 1) / kGroupBins;
    std::array<std::uint64_t, kGroups> g{};
    for (int i = 0; i < Hist::kBins; ++i) {
        g[static_cast<std::size_t>(i / kGroupBins)] += all_.bins[static_cast<std::size_t>(i)];
    }
    int first = 0;
    int last = kGroups - 1;
    while (first < last && g[static_cast<std::size_t>(first)] == 0) {
        ++first;
    }
    while (last > first && g[static_cast<std::size_t>(last)] == 0) {
        --last;
    }
    const std::uint64_t peak = *std::max_element(g.begin(), g.end());
    const int thr_group = static_cast<int>(std::floor(threshold * 100.0F + 1e-4F)) / kGroupBins;
    for (int i = first; i <= last; ++i) {
        const std::uint64_t c = g[static_cast<std::size_t>(i)];
        const int len = peak > 0 ? static_cast<int>((c * kBarWidth + peak - 1) / peak) : 0;
        std::string bar;
        for (int k = 0; k < len; ++k) {
            bar += "█";
        }
        out += "  " + f2(i * 0.05) + "～" + f2((i + 1) * 0.05) + " ";
        out += bar + " " + std::to_string(c);
        if (i == thr_group) {
            out += "  ← 阈值在这一格";
        }
        out += "\n";
    }

    const std::array<std::string, 3> yaw{group_text("正脸", yaw_[0], threshold),
                                         group_text("鼻子偏画面左", yaw_[1], threshold),
                                         group_text("鼻子偏画面右", yaw_[2], threshold)};
    append_groups(out, "按左右转头", yaw.data(), yaw.size());
    const std::array<std::string, 3> pitch{group_text("平视", pitch_[0], threshold),
                                           group_text("抬头", pitch_[1], threshold),
                                           group_text("低头", pitch_[2], threshold)};
    append_groups(out, "按抬头低头", pitch.data(), pitch.size());
    const std::array<std::string, 4> size{
        group_text("脸宽 <100", size_[0], threshold), group_text("100～200", size_[1], threshold),
        group_text("200～300", size_[2], threshold), group_text("≥300", size_[3], threshold)};
    append_groups(out, "按脸的大小", size.data(), size.size());
    if (margin_.n > 0) {
        out += "  和第二像的人差  P10 " + f2(static_cast<double>(margin_.percentile(0.1))) + "  P50 " +
               f2(static_cast<double>(margin_.percentile(0.5))) + "（差得越多越可靠）\n";
    }
    return out;
}

}  // namespace rksec::face
