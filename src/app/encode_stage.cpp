#include "app/encode_stage.hpp"

#include <pthread.h>

#include <chrono>
#include <string>
#include <utility>

#include "core/log.hpp"

namespace rksec::app {

namespace {
constexpr std::uint64_t kLogFirstFailures = 5;  // 每种失败只写前几次日志，之后只计数
}

EncodeStage::EncodeStage(std::unique_ptr<media::VideoEncoder> encoder, Sink sink)
    : encoder_(std::move(encoder)), sink_(std::move(sink))
{
}

EncodeStage::~EncodeStage()
{
    stop();
}

void EncodeStage::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    thread_ = std::thread([this] { loop(); });
}

void EncodeStage::stop()
{
    if (!started_ || stopped_) {
        return;
    }
    stopped_ = true;
    queue_.close();
    thread_.join();
}

void EncodeStage::submit(std::shared_ptr<const media::YuvView> frame, std::int64_t pts_us)
{
    if (!frame) {
        return;
    }
    const PushResult r = queue_.push(Item{std::move(frame), pts_us});
    const std::lock_guard<std::mutex> lock(mutex_);
    if (r != PushResult::closed) {
        ++counts_.submitted;
    }
    if (r == PushResult::dropped_oldest) {
        ++counts_.dropped;
    }
}

void EncodeStage::loop()
{
    pthread_setname_np(pthread_self(), "encode");
    std::string err;
    while (auto item = queue_.pop()) {
        media::EncodedPacket pkt;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = encoder_->encode(*item->frame, item->pts_us, pkt, err);
        encode_time.record(std::chrono::steady_clock::now() - t0);
        item.reset();  // 解码器的输出缓冲区马上放手

        if (!ok) {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (counts_.failed++ < kLogFirstFailures) {
                LOG_W("encode", "编码失败：%s", err.c_str());
            }
            continue;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.encoded;
            counts_.bytes += pkt.data.size();
            counts_.keyframes += pkt.keyframe ? 1 : 0;
        }
        if (sink_) {
            sink_(pkt);
        }
    }
}

EncodeStage::Stats EncodeStage::stats() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

}  // namespace rksec::app
