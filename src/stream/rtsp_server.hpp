#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "media/video_encoder.hpp"
#include "stream/credentials.hpp"

// RTSP 实时流（第 8c 步）：GStreamer 的 rtsp-server 跑在程序里，把编码线程编好的 H.264 包直接推出去，
// 不重新编码。电脑、手机用 VLC 打开 rtsp://板子:8554/live。
//
//   编码线程 ──push(包)──▶ [关键帧门] ──▶ appsrc ! h264parse ! rtph264pay ──▶ 每个客户端（RTP over UDP / TCP）
//
// - 所有客户端共用一条流水线（shared）。没有客户端时流水线不存在，push() 直接返回，几乎不花时间
// - 新的流水线（第一个客户端连上）从下一个关键帧开始推，最多等一个 GOP
// - 时间戳由 appsrc 按推进去的时刻打（do-timestamp），和 GStreamer 的时钟对得上
// - 网络慢、appsrc 积压超过 2 MB：后面的包先不推，等下一个关键帧重新开始（丢一段，不花屏、不越积越多）
// - 认证：Digest（只传哈希，不传明文密码）。账号来自单独的文件（credentials.hpp）。视频本身不加密，第 11 步做 RTSPS
// - GStreamer 的事件循环跑在自己的线程里；push() 可以在任何线程调用
//
// 需要的 GStreamer 插件：app（plugins-base）、rtp（plugins-good）、videoparsers（plugins-bad，h264parse）
namespace rksec::stream {

struct RtspOptions {
    std::string address = "0.0.0.0";  // 监听地址
    int port = 8554;                  // 0 = 系统随便给一个（测试用），bound_port() 查
    std::string path = "/live";
    Credentials credentials;
};

class RtspServer {
  public:
    static std::unique_ptr<RtspServer> start(const RtspOptions &opts, std::string &err);
    ~RtspServer();
    RtspServer(const RtspServer &) = delete;
    RtspServer &operator=(const RtspServer &) = delete;

    void push(const media::EncodedPacket &p);

    int bound_port() const;
    std::string url() const;  // rtsp://地址:端口/路径（不含账号）
    int clients() const;      // 现在连着的客户端个数

    struct Stats {
        std::uint64_t pushed = 0;         // 推出去的包
        std::uint64_t no_viewer = 0;      // 没人看，没推
        std::uint64_t waiting_key = 0;    // 等关键帧扔掉的
        std::uint64_t congested = 0;      // 积压太多扔掉的
        std::uint64_t clients_total = 0;  // 一共连过几次
        std::uint64_t sessions = 0;       // 流水线建过几次（每次从没人看到有人看）
    };
    Stats stats() const;

    struct Impl;

  private:
    RtspServer();
    std::unique_ptr<Impl> impl_;
};

}  // namespace rksec::stream
