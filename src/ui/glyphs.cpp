#include "ui/glyphs.hpp"

#include <lvgl.h>

#include <algorithm>

#include "ui/fonts.hpp"

namespace rksec::ui {

std::vector<std::uint32_t> code_points(std::string_view utf8)
{
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0U) == 0xC0U) {
            cp = c & 0x1FU;
            extra = 1;
        } else if ((c & 0xF0U) == 0xE0U) {
            cp = c & 0x0FU;
            extra = 2;
        } else {
            cp = c & 0x07U;
            extra = 3;
        }
        ++i;
        for (std::size_t k = 0; k < extra && i < utf8.size(); ++k, ++i) {
            cp = (cp << 6U) | (static_cast<unsigned char>(utf8[i]) & 0x3FU);
        }
        out.push_back(cp);
    }
    return out;
}

namespace {

bool has_glyph(const lv_font_t *font, std::uint32_t cp)
{
    lv_font_glyph_dsc_t dsc{};
    // 只查字体本身，不算 fallback
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && dsc.resolved_font == font;
}

// text 里 ok(码点) 为 false 的字（去重，按出现顺序）
template <typename Ok> std::string collect_missing(std::string_view text, Ok ok)
{
    std::string missing;
    std::vector<std::uint32_t> seen;
    std::size_t i = 0;
    for (const std::uint32_t cp : code_points(text)) {
        // 找到这个字在原文里的 UTF-8 字节，用于报告
        const std::size_t start = i;
        do {
            ++i;
        } while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0U) == 0x80U);
        if (!ok(cp) && std::find(seen.begin(), seen.end(), cp) == seen.end()) {
            seen.push_back(cp);
            missing.append(text.substr(start, i - start));
        }
    }
    return missing;
}

}  // namespace

std::string missing_glyphs(std::string_view text)
{
    // fallback 是图标字体，里面没有中文；两个界面字体都要有
    return collect_missing(
        text, [](std::uint32_t cp) { return has_glyph(&rksec_font_cn_20, cp) && has_glyph(&rksec_font_cn_28, cp); });
}

std::string missing_name_glyphs(std::string_view text)
{
    return collect_missing(
        text, [](std::uint32_t cp) { return has_glyph(&rksec_font_cn_20, cp) || has_glyph(&rksec_font_names_20, cp); });
}

}  // namespace rksec::ui
