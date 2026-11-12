#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/video_encoder.hpp"

// 把编好的 H.264 包写成 MP4（第 8b 步）。用 libavformat 只做封装，不重新编码。CI 上测（写一段真实的小码流再读回来）。
//
// 用“分段 MP4”（fragmented MP4，movflags = frag_keyframe + empty_moov）：
// - 普通 MP4 的索引（moov）在文件最后、关闭时才写。程序中途被杀掉，文件就打不开
// - 分段 MP4 开头写一个空索引，之后每个关键帧开始一段（moof + mdat），每段自带索引。
//   程序中途被杀，已经写完的段照样能播，最多丢最后一个 GOP（约 2 秒）
// 输入是 Annex B（起始码），libavformat 看到 extradata 是 Annex B，会自己转成 MP4 要的长度前缀格式。
//
// 时间戳：第一个包的 pts_us 当 0，之后按 pts_us 的差算；没有 B 帧，dts = pts。
// 第一个包必须是关键帧，前面的非关键帧丢掉（计数）。
// 文件权限 0600（录像里有人）。close() 后 fsync，断电不丢已经写完的内容。
namespace rksec::record {

class Mp4Writer {
  public:
    struct Options {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> sps_pps;  // Annex B 的 SPS + PPS
    };

    static std::unique_ptr<Mp4Writer> open(const std::string &path, const Options &opts, std::string &err);
    ~Mp4Writer();  // 没 close 就关掉（写完尾部，不 fsync）
    Mp4Writer(const Mp4Writer &) = delete;
    Mp4Writer &operator=(const Mp4Writer &) = delete;

    bool write(const media::EncodedPacket &p, std::string &err);
    bool close(std::string &err);

    std::uint64_t packets() const { return packets_; }
    std::uint64_t skipped() const { return skipped_; }  // 开头不是关键帧被丢掉的
    std::int64_t duration_us() const;

    struct Impl;

  private:
    Mp4Writer();
    std::unique_ptr<Impl> impl_;
    std::uint64_t packets_ = 0;
    std::uint64_t skipped_ = 0;
};

}  // namespace rksec::record
