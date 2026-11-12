#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "media/yuv_frame.hpp"

// MJPEG 硬件解码（瑞芯微 MPP，RK3588 的 jpegd 核），第 5b 步。
//
//   MppJpegDecoder dec;
//   dec.open({}, err);
//   MppJpegDecoder::Frame f = dec.decode(jpeg, size, err);   // f->layout / f->data / f->fd
//   …交给 RGA（第 5c 步），用完放手 f，输出缓冲区回到池子里
//
// - 输出是 MPP 自己分配的 DMA-BUF：有 fd，RGA、NPU、显示都能直接用，不用 CPU 拷贝（零拷贝）
// - 格式按 JPEG 原来的采样：4:2:0 → NV12，4:2:2 → NV16（这个摄像头是 4:2:2，见 5a）。不写死，看 layout
// - 输出缓冲区是一个池子（默认 4 块）：返回的 Frame 拿着不放，那一块就一直被占用；
//   池子用完 decode() 返回空（统计 no_buffer），不会等——和采集一样，宁可丢帧也不卡住
// - 输入（压缩数据，几十 KB）每帧拷进 MPP 的输入缓冲区，代价可以忽略
// - 每次 decode 同步等硬件做完（有超时）。只在一个线程里调用；Frame 可以在任何线程放手
// - 解码器先销毁也没关系：还没放手的 Frame 会让底层资源一直活到最后一个 Frame 放手
//
// 只在板子上编译（需要 librockchip-mpp，CMake 自动判断，定义 RKSEC_HAVE_MPP）。
namespace rksec::media {

class MppJpegDecoder {
  public:
    struct Options {
        int max_width = 1920;  // 输出缓冲区按这个尺寸分配
        int max_height = 1080;
        int out_buffers = 4;                                    // 输出缓冲区个数
        std::size_t max_packet = std::size_t{2} * 1024 * 1024;  // 一帧 MJPG 最大字节数（720p 实测 72～127 KB）
        int timeout_ms = 1000;                                  // 等硬件解完一帧最多多久
    };

    using Frame = std::shared_ptr<const YuvView>;

    MppJpegDecoder();
    ~MppJpegDecoder();
    MppJpegDecoder(const MppJpegDecoder &) = delete;
    MppJpegDecoder &operator=(const MppJpegDecoder &) = delete;

    bool open(const Options &opts, std::string &err);

    // 失败或者没有空闲的输出缓冲区时返回 nullptr，err 是原因
    Frame decode(const std::uint8_t *data, std::size_t size, std::string &err);

    struct Stats {
        std::uint64_t ok = 0;
        std::uint64_t failed = 0;
        std::uint64_t no_buffer = 0;  // 输出缓冲区都被占着（下游太慢或者忘了放手）
    };
    Stats stats() const;

    struct Core;  // 实现细节（MPP 的句柄和缓冲区），头文件里不出现 MPP 的类型

  private:
    std::shared_ptr<Core> core_;
};

}  // namespace rksec::media
