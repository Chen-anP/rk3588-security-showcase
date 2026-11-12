// 界面模拟（第 4c 步）的冒烟测试：headless 跑 1 秒，界面在动、页面在切、假画面在出。
// 只检查“有没有”，不检查耗时——耗时和机器、负载有关，断言门限会时好时坏（D15 的教训），用 --ui-bench 实测
#include <gtest/gtest.h>

#include "app/ui_sim.hpp"

using namespace rksec::app;

TEST(UiSim, HeadlessRunsRendersAndSwitchesPages)
{
    SimOptions o;
    o.seconds = 1;
    o.auto_nav = true;
    o.nav_every_ms = 200;
    o.quiet = true;
    const auto backend = headless_backend();
    const SimReport r = ui_sim(o, *backend);
    // 门限只要求“不止一帧”：理想是 25 帧，但 CI 上 TSan 慢 5～15 倍、又和其他测试并行，
    // 实测出现过只生成 5 帧（门限 > 5 时失败）。这里证明的是“在动”，不是“够快”
    EXPECT_GE(r.ui_frames, 2U) << "预览每帧都在变，界面应该一直在重画";
    EXPECT_GE(r.previews_made, 2U) << "假画面线程应该持续生成";
    EXPECT_GE(r.page_switches, 3U) << "每 200 ms 切一次页面";
    EXPECT_GT(r.frame_p50_ms, 0.0);
}

TEST(UiSim, BusyThreadsStopCleanly)
{
    // 占满 CPU 的线程要能按时停下（TSan 下也跑：检查停止标志的读写）
    SimOptions o;
    o.seconds = 1;
    o.busy_threads = 2;
    o.quiet = true;
    const auto backend = headless_backend();
    const SimReport r = ui_sim(o, *backend);
    EXPECT_GT(r.ui_frames, 0U);
    EXPECT_LT(r.seconds, 10.0) << "停不下来";
}
