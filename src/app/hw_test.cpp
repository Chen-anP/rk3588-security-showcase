#include "app/hw_test.hpp"
#include "app/proc_stats.hpp"
#include <chrono>
#include <cstdio>
namespace rksec::app {
int hw_test(const hw::PirOptions &options, int seconds)
{
    install_stop_signals();
    std::string error;
    auto input = hw::open_pir(options, error);
    if (!input) {
        std::fprintf(stderr, "红外设备不可用 %s line %d: %s\n", options.chip.c_str(), options.line, error.c_str());
        return 4;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    hw::PirState state;
    auto report = [&](const hw::InputResult &result) {
        const bool changed = state.update(result, options.active_high);
        if (result.status == hw::InputStatus::error) {
            std::fprintf(stderr, "红外设备不可用: %s\n", result.error.c_str());
            return false;
        }
        if (changed) {
            const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            std::printf("%.3f 秒 %s\n", elapsed,
                        state.active ? "检测到移动（输出有效）" : "感应输出恢复（不表示现场无人）");
            std::fflush(stdout);
        }
        return true;
    };
    if (!report(input->initial())) {
        return 4;
    }
    while (!stop_requested() && Clock::now() - start < std::chrono::seconds(seconds)) {
        if (!report(input->wait(std::chrono::milliseconds(100)))) {
            return 4;
        }
    }
    std::printf("红外测试结束：触发 %llu 次，恢复 %llu 次（初始状态不计数）\n",
                static_cast<unsigned long long>(state.triggers), static_cast<unsigned long long>(state.restores));
    return 0;
}
}  // namespace rksec::app
