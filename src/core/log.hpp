#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// 日志：printf 风格，编译时检查格式串（-Wformat=2）。
//   LOG_I("camera", "打开 %s，%dx%d", dev, w, h);
// - 低于当前级别的日志在宏里就跳过，不做任何格式化
// - 在 systemd 下运行（有 JOURNAL_STREAM 环境变量）时输出 "<6>[tag] 内容"，journald 据此识别级别、自己加时间戳；
//   在终端运行时输出 "2026-10-28 14:03:22.123 I [tag] 内容"
// - 一条日志最长 1024 字节，超出截断并以 "…" 结尾
// - 线程安全：多线程同时写不会交错
namespace rksec::log {

enum class Level : std::uint8_t { debug = 0, info = 1, warn = 2, error = 3 };

std::optional<Level> parse_level(std::string_view s);
std::string_view level_name(Level l);

void set_level(Level l);
Level level();

inline bool enabled(Level l)
{
    return static_cast<int>(l) >= static_cast<int>(level());
}

// 输出目的地。默认写 stderr；测试时换成收集到 vector 的函数。传 nullptr 恢复默认。
// 注意：sink 是在日志的锁里被调用的（保证多线程输出不交错），所以 sink 里不能再打日志，否则死锁（P6）
using Sink = std::function<void(Level, std::string_view line)>;
void set_sink(Sink sink);

// 在 systemd 下运行时为 true（启动时检测一次；测试可以强制设置）
void set_journald(bool on);

void write(Level l, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace rksec::log

#define RKSEC_LOG(lvl, tag, ...)                                                                                       \
    do {                                                                                                               \
        if (::rksec::log::enabled(lvl)) {                                                                              \
            ::rksec::log::write(lvl, tag, __VA_ARGS__);                                                                \
        }                                                                                                              \
    } while (0)

#define LOG_D(tag, ...) RKSEC_LOG(::rksec::log::Level::debug, tag, __VA_ARGS__)
#define LOG_I(tag, ...) RKSEC_LOG(::rksec::log::Level::info, tag, __VA_ARGS__)
#define LOG_W(tag, ...) RKSEC_LOG(::rksec::log::Level::warn, tag, __VA_ARGS__)
#define LOG_E(tag, ...) RKSEC_LOG(::rksec::log::Level::error, tag, __VA_ARGS__)
