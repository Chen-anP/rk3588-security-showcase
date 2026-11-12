#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 检查文字里的字在不在界面字体里。
//
// 中文字体只含界面用到的字（tools/fonts/gen_fonts.sh 生成），缺的字显示成方框。
// 固定文字（text.hpp）由测试逐字检查；运行时拼出来的文字（设备信息、以后的姓名）在这里查，
// 缺字时写日志，不会悄悄显示方框（D17：设置页的“时钟”“总”三个字就是这样漏掉的）。
namespace rksec::ui {

// UTF-8 → Unicode 码点（不校验非法序列，界面文字都是程序自己生成的）
std::vector<std::uint32_t> code_points(std::string_view utf8);

// 返回 text 里两个中文字体都没有的字（去重，按出现顺序），全有返回空串。
// 需要先调用 lvgl_init()
std::string missing_glyphs(std::string_view text);

// 检测框标签（20 号）能不能显示：每个字在界面字体或人名用字（GB2312 一级汉字）里（第 7d 步）。
// 登记名字时用它检查，界面上就不会出现方框
std::string missing_name_glyphs(std::string_view text);

}  // namespace rksec::ui
