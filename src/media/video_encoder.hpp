#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/yuv_frame.hpp"

// 视频编码器的接口（第 8a 步）。板子上的实现是 MPP 硬件编码（mpp_h264_encoder），
// 测试里用假的：编码阶段（EncodeStage）的线程、丢帧、统计这些逻辑在 CI 上用 TSan 测。
namespace rksec::media {

struct EncodedPacket {
    std::vector<std::uint8_t> data;  // H.264 Annex B
    bool keyframe = false;           // 含 IDR
    std::int64_t pts_us = 0;  // 这一帧的采集时间（微秒），原样带回来；录像和 RTSP 用它排时间
};

class VideoEncoder {
  public:
    virtual ~VideoEncoder() = default;
    // 编码一帧，同步：返回时 out 里是这一帧的码流。失败返回 false，err 是原因
    virtual bool encode(const YuvView &frame, std::int64_t pts_us, EncodedPacket &out, std::string &err) = 0;
    // 编码器的 SPS + PPS（Annex B）。RTSP、MP4 开头要用。没有返回空
    virtual std::vector<std::uint8_t> parameter_sets() const = 0;
};

}  // namespace rksec::media
