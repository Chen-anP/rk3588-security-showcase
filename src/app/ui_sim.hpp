#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// 不接摄像头、不接屏幕运行界面（第 4c 步）：
// - 电脑上（WSL）用 SDL 窗口显示、鼠标当手指——改界面不用每次上板子（rk3588-security-sim）
// - 板子或 CI 上不显示（headless），只渲染、计时——测界面本身的开销（rk3588-security --ui-bench）
//
// 和 --ui 用的是同一套界面代码（Screens、Router、RamDisplay、LatestImage），只是：
//   摄像头 + 解码  →  假画面生成线程（25 fps，移动的条纹，看得出卡没卡）
//   DRM 显示       →  SimBackend（SDL 窗口，或者什么都不做）
//   触摸屏         →  鼠标（SDL）/ 自动切换页面（headless）
//
// “推理很慢时界面不卡”的模拟：--busy N 开 N 个线程一直满载 CPU，看界面帧率和帧间隔 P99 变不变。
namespace rksec::app {

// 显示和输入的来源。只在界面线程里调用
class SimBackend {
  public:
    virtual ~SimBackend() = default;
    virtual const char *name() const = 0;
    // 处理窗口事件（SDL 要求在创建窗口的线程里定期调用）。返回 false = 用户关了窗口
    virtual bool pump() = 0;
    // 显示一整屏 XRGB8888 画面
    virtual bool present(const std::uint8_t *pixels, std::size_t stride) = 0;
    // 当前指针状态（鼠标左键 = 手指按下）
    virtual void pointer(bool &pressed, int &x, int &y) = 0;
};

// 什么都不显示的后端（板子上测开销、CI 上冒烟测试）
std::unique_ptr<SimBackend> headless_backend();

struct SimOptions {
    int seconds = 0;       // 运行多久；0 = 直到关窗口 / Ctrl+C
    int busy_threads = 0;  // 模拟推理占满 CPU 的线程数
    int preview_fps = 25;  // 假画面的帧率（和摄像头一样）
    bool auto_nav = false;  // 每 2 秒自动切换一次页面（headless 时没有鼠标，用它覆盖页面切换的渲染）
    int nav_every_ms = 2000;  // 自动切换页面的间隔
    bool quiet = false;       // 不打每 10 秒的状态日志（测试用）
};

struct SimReport {
    double seconds = 0;
    std::uint64_t ui_frames = 0;
    std::uint64_t previews_made = 0;
    std::uint64_t page_switches = 0;
    double frame_p50_ms = 0;  // 界面一帧（渲染 + 提交）耗时
    double frame_p99_ms = 0;
    double interval_p99_ms = 0;  // 首页相邻两帧的间隔，界面被饿着时会变大
    double cpu_percent = 0;
};

// 运行，结束时打印报告并返回统计。Ctrl+C / SIGTERM 提前结束
SimReport ui_sim(const SimOptions &opts, SimBackend &backend);

}  // namespace rksec::app
