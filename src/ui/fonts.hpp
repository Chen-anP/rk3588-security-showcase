#pragma once

#include <lvgl.h>

// 中文字体：tools/fonts/gen_fonts.sh 从 Noto Sans CJK SC 生成，只含界面用到的字 + ASCII。
// 20 号用于正文和按钮，28 号用于标题。定义在 src/ui/fonts/*.c（C 文件，所以要 extern "C"）
extern "C" {
LV_FONT_DECLARE(rksec_font_cn_20)
LV_FONT_DECLARE(rksec_font_cn_28)
// 人名用字（第 7d 步）：GB2312 一级汉字 3755 个，20 号，检测框标签上显示登记的名字
LV_FONT_DECLARE(rksec_font_names_20)
}
