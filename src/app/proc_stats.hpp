#pragma once

#include <atomic>

// 进程自身的运行指标和退出信号，长时间运行的模式（--capture-run、--ui）共用。
namespace rksec::app {

long rss_kb();    // 实际占用的物理内存（RSS），KB
long open_fds();  // 打开的文件描述符个数（含 DMA-BUF、DRM、input 设备）

// 进程到现在用掉的 CPU 时间（用户态 + 内核态，所有线程），秒。
// 两次相减再除以经过的时间 = CPU 占用率（1.0 = 一个核满载）
double cpu_seconds();

// Ctrl+C（SIGINT）和 SIGTERM（systemctl stop、timeout 命令）只设一个标志，主循环看到后正常收尾
void install_stop_signals();
bool stop_requested();

}  // namespace rksec::app
