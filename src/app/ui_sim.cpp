#include "app/ui_sim.hpp"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "app/proc_stats.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "core/version.hpp"
#include "ui/latest_image.hpp"
#include "ui/lvgl_port.hpp"
#include "ui/router.hpp"
#include "ui/screens.hpp"
#include "ui/theme.hpp"

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kStatusEvery = std::chrono::seconds(1);
constexpr int kLogEverySteps = 10;

class Headless final : public SimBackend {
  public:
    const char *name() const override { return "headless（不显示）"; }
    bool pump() override { return true; }
    bool present(const std::uint8_t * /*pixels*/, std::size_t /*stride*/) override { return true; }
    void pointer(bool &pressed, int &x, int &y) override
    {
        pressed = false;
        x = 0;
        y = 0;
    }
};

// 假画面：斜条纹每帧移动 4 像素，再加一条竖白线横向扫过——卡顿、跳帧肉眼看得出来
void draw_fake(media::Image &img, std::uint64_t n)
{
    img.width = ui::theme::kPreviewW;
    img.height = ui::theme::kPreviewH;
    img.stride = static_cast<std::size_t>(img.width) * 4;
    img.pixels.resize(img.stride * static_cast<std::size_t>(img.height));
    const auto shift = static_cast<int>(n * 4);
    const int bar = static_cast<int>(n * 8 % static_cast<std::uint64_t>(img.width));
    for (int y = 0; y < img.height; ++y) {
        std::uint8_t *row = img.pixels.data() + static_cast<std::size_t>(y) * img.stride;
        for (int x = 0; x < img.width; ++x) {
            std::uint8_t *p = row + static_cast<std::size_t>(x) * 4;
            const bool stripe = ((x + y + shift) / 32) % 2 == 0;
            const bool on_bar = x >= bar && x < bar + 6;
            const std::uint8_t v = on_bar ? 255 : (stripe ? 90 : 40);
            p[0] = v;                                                // B
            p[1] = on_bar ? 255 : static_cast<std::uint8_t>(v / 2);  // G
            p[2] = on_bar ? 255 : static_cast<std::uint8_t>(v / 3);  // R
            p[3] = 0xFF;
        }
    }
}

struct Pointer {
    SimBackend *backend = nullptr;
    static void read(lv_indev_t *indev, lv_indev_data_t *data)
    {
        auto *self = static_cast<Pointer *>(lv_indev_get_user_data(indev));
        bool pressed = false;
        int x = 0;
        int y = 0;
        self->backend->pointer(pressed, x, y);
        data->point.x = x;
        data->point.y = y;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
};

}  // namespace

std::unique_ptr<SimBackend> headless_backend()
{
    return std::make_unique<Headless>();
}

SimReport ui_sim(const SimOptions &opts, SimBackend &backend)
{
    install_stop_signals();
    using ui::theme::kScreenH;
    using ui::theme::kScreenW;

    ui::lvgl_init();
    StageStats frame_time{"界面一帧（渲染 + 提交）"};
    // 帧间隔只在首页统计：首页有预览，每来一帧新画面就重画一次，间隔应该约等于 1 / 25 秒。
    // 其他页面没有预览，只有时钟每秒变一次，间隔本来就是 1 秒，混进来会把 P99 拉到 1000 ms，
    // 看不出界面有没有被饿着（4c 第一次实测就是这样）
    StageStats frame_interval{"首页帧间隔（有预览时）"};
    Clock::time_point last_present{};
    ui::Router router;  // 放在前面：显示回调里要看当前在哪一页
    auto ram = std::make_unique<ui::RamDisplay>(kScreenW, kScreenH, [&](const std::uint8_t *px, std::size_t stride) {
        const auto now = Clock::now();
        // 每一帧都看当前页面：页面可能是自动切的，也可能是鼠标 / 手指点的（4c 第一版只认自动切换，
        // 在模拟器里用鼠标点到设置页后，1 秒一帧的间隔又混了进来）
        const bool on_home = router.current() == ui::Page::home;
        if (on_home && last_present.time_since_epoch().count() != 0) {
            frame_interval.record(now - last_present);
        }
        last_present = on_home ? now : Clock::time_point{};
        return backend.present(px, stride);
    });

    Pointer ptr{&backend};
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, &Pointer::read);
    lv_indev_set_user_data(indev, &ptr);
    lv_indev_set_display(indev, ram->display());

    ui::DeviceInfo info{"1024x600 (sim)", "fake 640x360", "", version::full()};  // 只用 ASCII，不受字体子集影响
    auto screens = std::make_unique<ui::Screens>(ram->display(), router, info);

    // ---------- 假画面线程 ----------
    std::atomic<bool> stop{false};
    ui::LatestImage latest;
    std::atomic<std::uint64_t> made{0};
    std::thread maker([&] {
        pthread_setname_np(pthread_self(), "fake-preview");
        const auto period = std::chrono::microseconds(1000000 / std::max(1, opts.preview_fps));
        auto next = Clock::now();
        while (!stop) {
            draw_fake(latest.back(), made.load());
            latest.publish();
            ++made;
            next += period;
            std::this_thread::sleep_until(next);
        }
    });

    // ---------- 模拟推理：占满 CPU 的线程 ----------
    std::vector<std::thread> busy;
    busy.reserve(static_cast<std::size_t>(std::max(0, opts.busy_threads)));
    for (int i = 0; i < opts.busy_threads; ++i) {
        busy.emplace_back([&stop] {
            pthread_setname_np(pthread_self(), "busy");
            volatile std::uint64_t x = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                x = x + 1;
            }
        });
    }

    LOG_I("sim", "界面模拟：%s，假画面 %d fps，占满 CPU 的线程 %d 个%s", backend.name(), opts.preview_fps,
          opts.busy_threads, opts.auto_nav ? "，自动切换页面" : "");

    const auto t0 = Clock::now();
    const double cpu0 = cpu_seconds();
    auto last = t0;
    double cpu_last = cpu0;
    std::uint64_t ui_frames_last = 0;
    std::uint64_t made_last = 0;
    auto next_status = t0;
    const auto nav_every = std::chrono::milliseconds(std::max(100, opts.nav_every_ms));
    auto next_nav = t0 + nav_every;
    int steps = 0;
    std::uint64_t switches = 0;
    // 一半时间在首页（测帧间隔），一半时间在别的页面（覆盖页面切换的渲染）
    const std::array<ui::Page, 6> tour = {ui::Page::settings, ui::Page::home,   ui::Page::about,
                                          ui::Page::home,     ui::Page::events, ui::Page::home};

    while (!stop_requested()) {
        const auto now = Clock::now();
        if (opts.seconds > 0 && now - t0 >= std::chrono::seconds(opts.seconds)) {
            break;
        }
        if (!backend.pump()) {
            break;  // 关了窗口
        }
        if (const media::Image *img = latest.take()) {
            screens->show_preview(*img);
        }
        if (opts.auto_nav && now >= next_nav) {
            next_nav += nav_every;
            const ui::Page to = tour[switches % tour.size()];
            if (to == ui::Page::home) {
                router.home();
            } else {
                router.go(to);
            }
            ++switches;
        }
        if (now >= next_status) {
            next_status += kStatusEvery;
            const double dt = std::chrono::duration<double>(now - last).count();
            const double cpu = cpu_seconds();
            ui::UiStatus st;
            st.camera_ok = true;
            st.camera_fps = dt > 0 ? static_cast<double>(made.load() - made_last) / dt : 0;
            st.ui_fps = dt > 0 ? static_cast<double>(ram->frames() - ui_frames_last) / dt : 0;
            st.cpu_percent = dt > 0 ? (cpu - cpu_last) / dt * 100.0 : 0;
            screens->update_status(st);
            if (++steps % kLogEverySteps == 0 && !opts.quiet) {
                const auto f = frame_time.summary();
                const auto iv = frame_interval.summary();
                LOG_I("sim", "界面 %.0f fps（%s），一帧 p50 %.1f ms p99 %.1f ms，首页帧间隔 p99 %.1f ms，CPU %.0f%%",
                      st.ui_fps, ui::page_name(router.current()), f.p50_ms, f.p99_ms, iv.p99_ms, st.cpu_percent);
            }
            last = now;
            cpu_last = cpu;
            ui_frames_last = ram->frames();
            made_last = made.load();
        }

        const std::uint64_t before = ram->frames();
        const auto h0 = Clock::now();
        const std::uint32_t idle = lv_timer_handler();
        if (ram->frames() != before) {
            frame_time.record(Clock::now() - h0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(idle, 5)));
    }

    stop = true;
    maker.join();
    for (auto &t : busy) {
        t.join();
    }

    SimReport r;
    r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    r.ui_frames = ram->frames();
    r.previews_made = made.load();
    r.page_switches = switches;
    const auto f = frame_time.summary();
    r.frame_p50_ms = f.p50_ms;
    r.frame_p99_ms = f.p99_ms;
    r.interval_p99_ms = frame_interval.summary().p99_ms;
    r.cpu_percent = r.seconds > 0 ? (cpu_seconds() - cpu0) / r.seconds * 100.0 : 0;

    lv_indev_delete(indev);
    screens.reset();
    ram.reset();
    ui::lvgl_deinit();

    if (!opts.quiet) {
        std::printf("运行       %.0f 秒（%s，占满 CPU 的线程 %d 个）\n", r.seconds, backend.name(), opts.busy_threads);
        std::printf("界面       %llu 帧（平均 %.1f fps），自动切换页面 %llu 次\n",
                    static_cast<unsigned long long>(r.ui_frames),
                    r.seconds > 0 ? static_cast<double>(r.ui_frames) / r.seconds : 0.0,
                    static_cast<unsigned long long>(r.page_switches));
        std::printf("%s\n", frame_time.report().c_str());
        std::printf("%s\n", frame_interval.report().c_str());
        std::printf("假画面     生成 %llu 帧\n", static_cast<unsigned long long>(r.previews_made));
        std::printf("CPU        平均 %.0f%%（100%% = 一个核满载，含占满 CPU 的线程）\n", r.cpu_percent);
    }
    return r;
}

}  // namespace rksec::app
