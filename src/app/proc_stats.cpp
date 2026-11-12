#include "app/proc_stats.hpp"

#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace rksec::app {

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_stop_signal(int /*sig*/)
{
    g_stop = true;  // 信号处理函数里只做这一件事：设标志（无锁 atomic 是信号安全的）
}

}  // namespace

long rss_kb()
{
    std::ifstream f("/proc/self/statm");
    long pages_total = 0;
    long pages_rss = 0;
    f >> pages_total >> pages_rss;
    return pages_rss * (sysconf(_SC_PAGESIZE) / 1024);
}

long open_fds()
{
    std::error_code ec;
    long n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

double cpu_seconds()
{
    // /proc/self/stat：第 2 项是括号里的程序名（可能含空格），从最后一个 ')' 之后数：
    // 第 3 项起依次排列，utime 是第 14 项，stime 是第 15 项，单位是时钟滴答
    std::ifstream f("/proc/self/stat");
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto pos = all.rfind(')');
    if (pos == std::string::npos) {
        return 0;
    }
    std::istringstream in(all.substr(pos + 1));
    std::string field;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    for (int i = 3; i <= 15 && in >> field; ++i) {
        if (i == 14) {
            utime = std::stoull(field);
        } else if (i == 15) {
            stime = std::stoull(field);
        }
    }
    const long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? static_cast<double>(utime + stime) / static_cast<double>(hz) : 0;
}

void install_stop_signals()
{
    struct sigaction sa {};
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

bool stop_requested()
{
    return g_stop;
}

}  // namespace rksec::app
