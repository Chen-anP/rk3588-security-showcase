#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// MJPG 帧的 CPU 解码（libjpeg-turbo）。第 4b 步用它做预览，同时作为第 5 步硬件解码（MPP）的对比基线。
//
// - 输出 XRGB8888（内存里的字节顺序是 B G R X，和 DRM 的 XRGB8888、LVGL 的 XRGB8888 一致）
// - 解码时直接缩小（JPEG 的 DCT 缩放，1/2、1/4……），比先全尺寸解码再缩放省很多：
//   1280x720 → 640x360 只需要算四分之一的像素
// - 摄像头偶尔给出不完整的帧（USB 传输出错）。libjpeg-turbo 对“能解出来但有问题”的数据只给警告，
//   这种帧照常显示，计数；完全解不了的返回失败，不会崩溃
// - 不是线程安全的：每个线程用自己的 JpegDecoder
namespace rksec::media {

struct Image {
    int width = 0;
    int height = 0;
    std::size_t stride = 0;  // 每行字节数
    std::vector<std::uint8_t> pixels;
};

class JpegDecoder {
  public:
    JpegDecoder();
    ~JpegDecoder();
    JpegDecoder(const JpegDecoder &) = delete;
    JpegDecoder &operator=(const JpegDecoder &) = delete;

    // 解码到 out，选不超过 max_width 的最大缩放档。out 的缓冲区会复用，尺寸不变时不重新分配内存。
    // 失败返回 false，err 里是原因
    bool decode(const std::uint8_t *data, std::size_t size, int max_width, Image &out, std::string &err);

    struct Stats {
        std::uint64_t ok = 0;
        std::uint64_t warnings = 0;  // 数据有问题但解出来了（也算在 ok 里）
        std::uint64_t failed = 0;
    };
    Stats stats() const { return stats_; }

  private:
    void *handle_ = nullptr;
    Stats stats_;
};

// 按 max_width 选缩放后的尺寸：libjpeg-turbo 支持的缩放档里，宽度不超过 max_width 的最大一档。
// 都超过时用最小一档。测试和日志用
void scaled_size(int width, int height, int max_width, int &out_w, int &out_h);

}  // namespace rksec::media
