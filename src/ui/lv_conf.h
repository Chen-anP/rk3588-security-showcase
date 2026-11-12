/*
 * LVGL 配置（LVGL v9.2.2，版本锁定见 cmake/Lvgl.cmake）。
 *
 * 只写和默认值不同的项，其余用 LVGL 的默认值（lv_conf_internal.h）。
 * 升级 LVGL 时对照新版本的 lv_conf_template.h 检查这里每一项是否还有效。
 * 这个文件也会被 LVGL 的 C 代码包含，只能写 C 语法。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 颜色：32 位 XRGB8888，和 DRM 显示缓冲区、MJPG 解码输出一致，不用转换 */
#define LV_COLOR_DEPTH 32

/* 内存、字符串、格式化都用 C 库：
 * LVGL 自带的内存池是固定大小的一块，用完就分配失败；用 malloc 才能被 ASan、valgrind 检查到越界和泄漏 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* 界面刷新周期 16 ms（最高约 60 帧，和屏幕刷新率一致；实际由翻页等待限速） */
#define LV_DEF_REFR_PERIOD 16

/* 7 寸 1024x600 屏约 170 dpi。LVGL 按它换算 LV_DPX() 等尺寸 */
#define LV_DPI_DEF 170

/* 不用 LVGL 的多线程支持：所有 LVGL 调用都在界面线程里，别的线程通过 LatestImage 等交数据 */
#define LV_USE_OS LV_OS_NONE

/* 日志：警告以上转到本程序的日志（lv_log_register_print_cb） */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

/* 图片缓存关掉：预览图每帧都换，缓存没有意义，还要占内存 */
#define LV_CACHE_DEF_SIZE 0

/* 字体：
 * - 中文：tools/fonts/gen_fonts.sh 生成的 rksec_font_cn_20 / 28（src/ui/fonts/），只含界面用到的字。
 *   LVGL 自带的中文字体（simsun 16 号 1000 字）连“设置”“记录”都没有，不用
 * - 数字和图标（LV_SYMBOL_*）：Montserrat，中文字体里没有的字形到这里找（fallback） */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

/* 主题：深色（安防设备常装在门口、走廊，深色背景不刺眼） */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/* 显示由本程序自己的 DRM 代码负责（src/display/），不用 LVGL 自带的 DRM 驱动：
 * 它总是用输出口模式列表的第一个模式，在这块屏上会选到会闪的那组时序（D14） */
#define LV_USE_LINUX_DRM 0

/* 触摸用 LVGL 自带的 evdev 驱动（读 /dev/input/eventN，自动读取坐标范围做校准） */
#define LV_USE_EVDEV 1

/* 不编译示例和演示程序 */
#define LV_BUILD_EXAMPLES 0

#endif /* LV_CONF_H */
