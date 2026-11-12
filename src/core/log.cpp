#include "core/log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <utility>

namespace rksec::log {

namespace {

constexpr std::size_t kMaxMessage = 1024;

std::atomic<int> g_level{static_cast<int>(Level::info)};
std::atomic<bool> g_journald{std::getenv("JOURNAL_STREAM") != nullptr};
std::mutex g_mutex;
Sink g_sink;  // 受 g_mutex 保护；为空时写 stderr

// syslog 优先级：journald 认这个数字
int syslog_priority(Level l)
{
    switch (l) {
    case Level::debug:
        return 7;
    case Level::info:
        return 6;
    case Level::warn:
        return 4;
    case Level::error:
        return 3;
    }
    return 6;
}

char level_letter(Level l)
{
    switch (l) {
    case Level::debug:
        return 'D';
    case Level::info:
        return 'I';
    case Level::warn:
        return 'W';
    case Level::error:
        return 'E';
    }
    return '?';
}

std::string timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    std::array<char, 32> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%F %T", &tm);
    std::snprintf(buf.data() + n, buf.size() - n, ".%03lld", static_cast<long long>(ms));
    return buf.data();
}

}  // namespace

std::optional<Level> parse_level(std::string_view s)
{
    if (s == "debug") {
        return Level::debug;
    }
    if (s == "info") {
        return Level::info;
    }
    if (s == "warn") {
        return Level::warn;
    }
    if (s == "error") {
        return Level::error;
    }
    return std::nullopt;
}

std::string_view level_name(Level l)
{
    switch (l) {
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warn:
        return "warn";
    case Level::error:
        return "error";
    }
    return "?";
}

void set_level(Level l)
{
    g_level.store(static_cast<int>(l), std::memory_order_relaxed);
}

Level level()
{
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void set_sink(Sink sink)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void set_journald(bool on)
{
    g_journald.store(on, std::memory_order_relaxed);
}

void write(Level l, const char *tag, const char *fmt, ...)
{
    std::array<char, kMaxMessage> msg{};
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(msg.data(), msg.size(), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    std::string text{msg.data()};
    if (static_cast<std::size_t>(n) >= msg.size()) {
        // 截断时可能切在一个多字节汉字中间：退回到完整字符，再加省略号
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
            text.pop_back();
        }
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80U) != 0U) {
            text.pop_back();
        }
        text += "…";
    }

    std::string line;
    if (g_journald.load(std::memory_order_relaxed)) {
        line = "<" + std::to_string(syslog_priority(l)) + ">";
    } else {
        line = timestamp() + " " + level_letter(l) + " ";
    }
    line += "[";
    line += tag;
    line += "] ";
    line += text;

    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(l, line);
    } else {
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), stderr);  // 一次写完，多进程输出也不会交错
    }
}

}  // namespace rksec::log
