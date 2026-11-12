#pragma once

#include <memory>
#include <string>

#include "app/detect_pipeline.hpp"
#include "app/encode_stage.hpp"
#include "core/config.hpp"
#include "record/event_recorder.hpp"
#include "stream/rtsp_server.hpp"

// 视频输出（第 8b 步录像，第 8c 步加 RTSP）：硬件编码一次，编好的包同时交给
//   - 事件录像（陌生人出现时录 MP4）：要 record.enabled，而且检测和人脸识别都开着（can_record）
//   - RTSP 实时流：要 rtsp.enabled，而且账号文件可用
// 两样都是可选的，哪样起不来只关哪样（日志说明原因）；两样都没有就不编码，open() 返回 nullptr。
// --ui 和 --record-run 共用。
//
//   auto out = Recording::open(cfg, can_record, err);
//   DetectHooks h = hw->hooks();  if (out) out->attach(h);   // 解码出来的帧交给编码线程
//   DetectPipeline pipe{..., h, ...};  pipe.start();
//   主循环：有新检测结果 out->on_result(*r, 预览图)；每一轮 out->tick()
//   收尾：先停采集和流水线，再 out->stop()（编码线程编完剩下的帧、录像器结束正在录的事件、关 RTSP）
namespace rksec::app {

class Recording {
  public:
    static std::unique_ptr<Recording> open(const config::AppConfig &cfg, bool can_record, std::string &err);
    ~Recording();
    Recording(const Recording &) = delete;
    Recording &operator=(const Recording &) = delete;

    void attach(DetectHooks &hooks);
    void on_result(const DetectResult &r, const media::Image *preview);
    void tick();
    void stop();

    EncodeStage &encoder() { return *encode_; }
    record::EventRecorder *recorder() { return recorder_.get(); }  // 没开录像时为 nullptr
    stream::RtspServer *rtsp() { return rtsp_.get(); }             // 没开 RTSP 时为 nullptr
    std::string describe() const;

  private:
    Recording() = default;
    // 析构顺序是声明的逆序：先编码阶段（它的 sink 引用录像器和 RTSP），再 RTSP、录像器
    std::unique_ptr<record::EventRecorder> recorder_;
    std::unique_ptr<stream::RtspServer> rtsp_;
    std::unique_ptr<EncodeStage> encode_;
    std::string description_;
    bool stopped_ = false;
};

// 现在（和包的 pts、检测结果的采集时间同一个时钟：steady_clock，微秒）
std::int64_t now_us();

}  // namespace rksec::app
