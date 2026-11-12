// rk3588-security-sim：电脑上的界面模拟器（第 4c 步）。
// 和板子上的 --ui 用同一套界面代码；显示换成 SDL 窗口，鼠标当手指，摄像头换成假画面。
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include "app/ui_sim.hpp"
#include "core/log.hpp"
#include "sim/sdl_backend.hpp"
#include "ui/theme.hpp"

namespace {

void usage(const char *prog)
{
    std::printf("用法: %s [选项]\n"
                "  --seconds N   运行 N 秒后自动退出（默认一直运行，关窗口或 Ctrl+C 结束）\n"
                "  --busy N      开 N 个线程占满 CPU，模拟“推理很慢”\n"
                "  --auto-nav    每 2 秒自动切换页面\n"
                "  --scale N     窗口放大 N 倍（高分屏上用 2）\n"
                "  --headless    不开窗口（没有图形界面时）\n"
                "  -h, --help    显示本帮助\n",
                prog);
}

bool parse_int(std::string_view s, int lo, int hi, int &out)
{
    int v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size() || v < lo || v > hi) {
        return false;
    }
    out = v;
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    rksec::app::SimOptions opts;
    int scale = 1;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--auto-nav") {
            opts.auto_nav = true;
            continue;
        }
        if (a == "--headless") {
            headless = true;
            continue;
        }
        if ((a == "--seconds" || a == "--busy" || a == "--scale") && i + 1 < argc) {
            const std::string_view v{argv[++i]};
            const bool ok = a == "--seconds" ? parse_int(v, 1, 86400, opts.seconds)
                            : a == "--busy"  ? parse_int(v, 0, 64, opts.busy_threads)
                                             : parse_int(v, 1, 4, scale);
            if (!ok) {
                std::fprintf(stderr, "%s 的值不对：%s\n", argv[i - 1], argv[i]);
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "未知选项或缺少参数: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    std::unique_ptr<rksec::app::SimBackend> backend;
    if (headless) {
        backend = rksec::app::headless_backend();
    } else {
        std::string err;
        backend = rksec::sim::sdl_backend(rksec::ui::theme::kScreenW, rksec::ui::theme::kScreenH, scale, err);
        if (!backend) {
            LOG_E("sim", "%s", err.c_str());
            return 4;
        }
    }
    rksec::app::ui_sim(opts, *backend);
    return 0;
}
