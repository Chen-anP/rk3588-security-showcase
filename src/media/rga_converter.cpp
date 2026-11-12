#include "media/rga_converter.hpp"

// librga 的头文件用了 NULL、uint64_t 却没有自己包含 <cstddef>、<cstdint>，要先包含
#include <cstddef>
#include <cstdint>
#include <string>

#include <rga/im2d.h>
#include <rga/rga.h>

namespace rksec::media {

namespace {

int rk_format(YuvFormat f)
{
    return f == YuvFormat::nv12 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_422_SP;
}

int rk_format(RgbFormat f)
{
    // RGA 的格式名按内存里的字节顺序：XRGB8888 在内存里是 B G R X。
    // 板子上的 RGA3 驱动输出不接受 BGRX_8888（no core match），所以用内存布局相同的
    // BGRA_8888；第 4 个字节被写成 alpha，DRM 的 XRGB8888 不读这个字节，结果一样。
    return f == RgbFormat::rgb888 ? RK_FORMAT_RGB_888 : RK_FORMAT_BGRA_8888;
}

int bytes_per_pixel(RgbFormat f)
{
    return f == RgbFormat::rgb888 ? 3 : 4;
}

// 导入的 buffer 句柄：出作用域自动释放
class Handle {
  public:
    Handle(int fd, int size) : h_(importbuffer_fd(fd, size)) {}
    ~Handle()
    {
        if (h_ != 0) {
            releasebuffer_handle(h_);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    rga_buffer_handle_t get() const { return h_; }

  private:
    rga_buffer_handle_t h_;
};

// RGA3 两个核都能用，librga 自己挑空闲的。这个设置按线程生效
bool use_rga3_only(std::string &err)
{
    thread_local bool done = false;
    if (done) {
        return true;
    }
    const IM_STATUS s = imconfig(IM_CONFIG_SCHEDULER_CORE,
                                 static_cast<std::uint64_t>(IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1));
    if (s != IM_STATUS_SUCCESS) {
        err = std::string{"设置只用 RGA3 失败："} + imStrError_t(s);
        return false;
    }
    done = true;
    return true;
}

}  // namespace

std::string rga_version()
{
    return RGA_API_VERSION;
}

bool rga_convert(const YuvView &src, const RgbTarget &dst, const Rect &rect, std::string &err)
{
    return rga_convert(src, Rect{0, 0, src.layout.width, src.layout.height}, dst, rect, err);
}

bool rga_convert(const YuvView &src, const Rect &src_rect, const RgbTarget &dst, const Rect &rect, std::string &err)
{
    const YuvLayout &l = src.layout;
    if (src.fd < 0 || dst.fd < 0 || !l.valid()) {
        err = "RGA 输入或输出没有 DMA-BUF fd，或者输入尺寸不对";
        return false;
    }
    const Rect &sr = src_rect;
    if (sr.x < 0 || sr.y < 0 || sr.x + sr.width > l.width || sr.y + sr.height > l.height || sr.x % 2 != 0 ||
        sr.y % 2 != 0 || sr.width % 2 != 0 || sr.height % 2 != 0) {
        err = "RGA 输入区域 " + std::to_string(sr.x) + "," + std::to_string(sr.y) + " " + std::to_string(sr.width) +
              "x" + std::to_string(sr.height) + " 超出画面或不是偶数";
        return false;
    }
    // RGA3 的硬件限制：输入、输出宽高至少 68×2（驱动 hardware 信息里的 input/output range）。
    // 不满足时驱动只在 dmesg 里报 “no core match”，程序这边只看到笼统的失败，所以先自己检查、说清楚
    constexpr int kMinW = 68;
    if (sr.width < kMinW || rect.width < kMinW || sr.height < 2 || rect.height < 2) {
        err = "RGA3 要求输入、输出宽度至少 68（输入 " + std::to_string(sr.width) + "，输出 " +
              std::to_string(rect.width) + "）";
        return false;
    }
    if (!use_rga3_only(err)) {
        return false;
    }

    const Handle sh{src.fd, static_cast<int>(l.total_bytes())};
    const Handle dh{dst.fd, dst.stride_px * dst.height * bytes_per_pixel(dst.format)};
    if (sh.get() == 0 || dh.get() == 0) {
        err = "RGA 导入 DMA-BUF 失败";
        return false;
    }

    // 不用 wrapbuffer_handle / imcheck 这些宏（可变参数宏不传参数时 clang 会报警告），直接调用后面的函数
    rga_buffer_t s = wrapbuffer_handle_t(sh.get(), l.width, l.height, l.hstride, l.vstride, rk_format(l.format));
    rga_buffer_t d =
        wrapbuffer_handle_t(dh.get(), dst.width, dst.height, dst.stride_px, dst.height, rk_format(dst.format));
    // JPEG 是全范围；librga 默认按有限范围转（灰 200 会变成 214）。
    // 颜色转换模式要设在目标图上：imcvtcolor 也是写 dst.color_space_mode，设在源图上会被忽略。
    d.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;

    rga_buffer_t pat{};
    const im_rect srect{sr.x, sr.y, sr.width, sr.height};
    const im_rect drect{rect.x, rect.y, rect.width, rect.height};
    const im_rect prect{};

    IM_STATUS st = imcheck_t(s, d, pat, srect, drect, prect, 0);
    if (st != IM_STATUS_NOERROR) {
        err = std::string{"RGA 参数检查不通过："} + imStrError_t(st);
        return false;
    }
    st = improcess(s, d, pat, srect, drect, prect, -1, nullptr, nullptr, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        err = std::string{"RGA 处理失败："} + imStrError_t(st);
        return false;
    }
    return true;
}

}  // namespace rksec::media
