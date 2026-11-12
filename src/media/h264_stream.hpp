#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// H.264 码流的 Annex B 格式小工具（第 8a 步）。纯逻辑，CI 上测。
//
// MPP 编码器输出的是 Annex B：一串 NAL 单元，每个前面有起始码 00 00 01（或 00 00 00 01）。
//   NAL 头 1 个字节：低 5 位是类型。常用的：1 = 普通帧（P/B），5 = IDR 关键帧，7 = SPS，8 = PPS
// 录像和 RTSP 都要知道：
//   - 一个包是不是关键帧（IDR）：录像必须从关键帧开始，不然播放器开头一段是花的
//   - SPS / PPS（解码需要的参数）：封装 MP4、RTSP 的 SDP 里要用
// 关键帧不是靠 MPP 的元数据判断，而是看码流本身：和硬件无关，也能在 CI 上测。
namespace rksec::media {

constexpr int kNalSlice = 1;  // 非 IDR 的帧
constexpr int kNalIdr = 5;    // IDR 关键帧
constexpr int kNalSps = 7;
constexpr int kNalPps = 8;

struct NalUnit {
    std::size_t start = 0;  // NAL 头（类型那个字节）在 data 里的位置，不含起始码
    std::size_t size = 0;  // 到下一个起始码（或结尾）的字节数，不含起始码，也不含尾部多余的 00
    int type = 0;          // 低 5 位
};

// 把一段 Annex B 码流切成 NAL 单元。没有起始码的数据返回空
std::vector<NalUnit> split_annexb(const std::uint8_t *data, std::size_t size);

// 里面有没有 IDR（类型 5）
bool has_idr(const std::uint8_t *data, std::size_t size);

// 取出所有 SPS 和 PPS，每个带 4 字节起始码（00 00 00 01）拼在一起；没有返回空
std::vector<std::uint8_t> extract_parameter_sets(const std::uint8_t *data, std::size_t size);

}  // namespace rksec::media
