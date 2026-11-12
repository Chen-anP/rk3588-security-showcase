#pragma once

#include <memory>
#include <string>

#include "media/video_encoder.hpp"

// H.264 硬件编码（瑞芯微 MPP，RK3588 的 VEPU580），第 8a 步。
//
//   MppH264Encoder enc;
//   enc.open({.width = 1280, .height = 720, ...}, err);
//   EncodedPacket pkt;
//   enc.encode(frame, pts_us, pkt, err);     // frame 是解码器输出的 DMA-BUF，不拷贝
//
// - 输入直接用 MPP 解码器输出的 YUV（NV16 / NV12 的 DMA-BUF）：把 fd 导入 MPP，硬件直接读，CPU 不碰像素。
//   格式、行宽按第一帧的实际值告诉编码器（MPP 只认参数里登记的格式，见 apply_input）；输出总是 4:2:0
// - 码率控制 CBR（固定码率）：录像的磁盘占用、RTSP 的网络带宽可以预估
// - 每个 IDR 前面都带 SPS/PPS：RTSP 客户端中途加入、录像从任意关键帧开始都能解码。
//   MPP 没带的由这里补上（板子上的 MPP 1.0.x 不保证每次都带，第 8a 步实测）
// - 没有 B 帧（MPP 默认的参考结构 IPPP）：延迟最低，MP4 的时间戳也简单
// - 同步：encode() 返回时这一帧已经编完。只在一个线程里调用
//
// 只在板子上编译（需要 librockchip-mpp，CMake 自动判断，定义 RKSEC_HAVE_MPP）。
namespace rksec::media {

class MppH264Encoder final : public VideoEncoder {
  public:
    struct Options {
        int width = 1280;  // 画面尺寸，必须和输入帧一致
        int height = 720;
        int fps = 25;  // 码率控制按这个帧率算每帧的预算（不是限制输入帧率）
        int bitrate_kbps = 4000;
        int gop = 50;           // 每隔多少帧一个 IDR
        int timeout_ms = 1000;  // 等硬件编完一帧最多多久
    };

    MppH264Encoder();
    ~MppH264Encoder() override;
    MppH264Encoder(const MppH264Encoder &) = delete;
    MppH264Encoder &operator=(const MppH264Encoder &) = delete;

    bool open(const Options &opts, std::string &err);

    bool encode(const YuvView &frame, std::int64_t pts_us, EncodedPacket &out, std::string &err) override;
    std::vector<std::uint8_t> parameter_sets() const override;
    // 有多少个关键帧是编码器自己补的 SPS/PPS（MPP 没带）。encode() 所在的线程里读，或者停下以后读
    std::uint64_t headers_added() const;
    // 按输入帧的实际格式、行宽重新设置过几次（正常是第一帧 1 次）
    std::uint64_t input_reconfigs() const;

    struct Core;  // 实现细节（MPP 的句柄），头文件里不出现 MPP 的类型

  private:
    std::unique_ptr<Core> core_;
};

}  // namespace rksec::media
