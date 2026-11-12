#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "core/bounded_queue.hpp"
#include "core/stage_timer.hpp"
#include "media/video_encoder.hpp"

// 编码阶段（第 8a 步）：单独一个线程做视频编码，不拖慢预览和检测。
//
//   media 线程：解码出一帧 ──submit()──▶ [最多 2 帧] ──▶ 编码线程：encoder.encode() ──▶ sink(包)
//
// - submit() 不阻塞：编码跟不上时丢最旧的一帧（延迟优先，和检测一样）。丢帧发生在编码之前，
//   编码器的参考帧不受影响，丢了只是码流少一帧、画面少一点流畅，不会花屏
// - 队列里的帧拿着解码器的输出缓冲区不放，所以解码器的缓冲区池要比平时多留几块（hw_pipeline 里调大了）
// - sink 在编码线程里调用，要快：录像、RTSP 把包放进自己的队列就返回，不要在里面写磁盘、发网络
// - 编码器和 sink 由调用者保证比 EncodeStage 活得久（sink 里的东西析构前先 stop()）
namespace rksec::app {

class EncodeStage {
  public:
    using Sink = std::function<void(const media::EncodedPacket &)>;

    EncodeStage(std::unique_ptr<media::VideoEncoder> encoder, Sink sink);
    ~EncodeStage();
    EncodeStage(const EncodeStage &) = delete;
    EncodeStage &operator=(const EncodeStage &) = delete;

    void start();
    // 停止：不再接收新帧，队列里剩下的编完，线程退出。可以重复调用
    void stop();

    // pts_us：这一帧的采集时间（微秒），原样出现在输出的包里
    void submit(std::shared_ptr<const media::YuvView> frame, std::int64_t pts_us);

    // 编码器的 SPS + PPS（Annex B）
    std::vector<std::uint8_t> parameter_sets() const { return encoder_->parameter_sets(); }

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t dropped = 0;  // 编码跟不上，挤掉的
        std::uint64_t encoded = 0;
        std::uint64_t failed = 0;
        std::uint64_t keyframes = 0;
        std::uint64_t bytes = 0;
    };
    Stats stats() const;

    StageStats encode_time{"编码"};

  private:
    struct Item {
        std::shared_ptr<const media::YuvView> frame;
        std::int64_t pts_us = 0;
    };

    void loop();

    std::unique_ptr<media::VideoEncoder> encoder_;
    Sink sink_;
    BoundedQueue<Item> queue_{2};
    std::thread thread_;
    bool started_ = false;
    bool stopped_ = false;

    mutable std::mutex mutex_;
    Stats counts_;
};

}  // namespace rksec::app
