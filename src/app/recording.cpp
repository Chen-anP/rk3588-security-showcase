#include "app/recording.hpp"

#include <chrono>

#include "core/log.hpp"
#ifdef RKSEC_HAVE_MPP
#include "media/mpp_h264_encoder.hpp"
#endif

namespace rksec::app {

std::int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(PipelineClock::now().time_since_epoch()).count();
}

Recording::~Recording()
{
    stop();
}

#ifdef RKSEC_HAVE_MPP

namespace {

// 录像：起不来返回 nullptr，why 是原因
std::unique_ptr<record::EventRecorder> open_recorder(const config::AppConfig &cfg, bool can_record,
                                                     const media::MppH264Encoder &enc, std::string &why)
{
    if (!cfg.record.enabled) {
        why = "配置里关了（record.enabled = false）";
        return nullptr;
    }
    if (!can_record) {
        why = "录像靠“陌生人”触发，要检测和人脸识别都开着";
        return nullptr;
    }
    auto store = record::EventStore::open(cfg.face.database, why);
    if (!store) {
        return nullptr;
    }
    record::RecorderOptions ro;
    ro.dir = cfg.record.dir;
    ro.pre_us = std::int64_t{cfg.record.pre_seconds} * 1'000'000;
    ro.event.post_us = std::int64_t{cfg.record.post_seconds} * 1'000'000;
    ro.event.max_us = std::int64_t{cfg.record.max_seconds} * 1'000'000;
    ro.quota_bytes = std::int64_t{cfg.record.quota_mb} * 1024 * 1024;
    ro.video.width = cfg.camera.width;
    ro.video.height = cfg.camera.height;
    ro.video.sps_pps = enc.parameter_sets();
    auto rec = std::make_unique<record::EventRecorder>(std::move(ro), std::move(store));
    if (!rec->start(why)) {
        return nullptr;
    }
    return rec;
}

std::unique_ptr<stream::RtspServer> open_rtsp(const config::AppConfig &cfg, std::string &user, std::string &why)
{
    if (!cfg.rtsp.enabled) {
        why = "配置里关了（rtsp.enabled = false）";
        return nullptr;
    }
    stream::RtspOptions o;
    o.address = cfg.rtsp.address;
    o.port = cfg.rtsp.port;
    o.path = cfg.rtsp.path;
    if (!stream::load_credentials(cfg.rtsp.credentials, o.credentials, why)) {
        return nullptr;
    }
    user = o.credentials.user;
    return stream::RtspServer::start(o, why);
}

}  // namespace

std::unique_ptr<Recording> Recording::open(const config::AppConfig &cfg, bool can_record, std::string &err)
{
    if (!cfg.encode.enabled) {
        err = "配置里关了编码（encode.enabled = false）：没有录像，也没有 RTSP";
        return nullptr;
    }
    auto enc = std::make_unique<media::MppH264Encoder>();
    media::MppH264Encoder::Options eo;
    eo.width = cfg.camera.width;
    eo.height = cfg.camera.height;
    eo.fps = cfg.encode.fps;
    eo.bitrate_kbps = cfg.encode.bitrate_kbps;
    eo.gop = cfg.encode.gop;
    if (!enc->open(eo, err)) {
        err = "编码器：" + err;
        return nullptr;
    }

    std::unique_ptr<Recording> r{new Recording()};
    std::string why;
    r->recorder_ = open_recorder(cfg, can_record, *enc, why);
    const std::string record_text =
        r->recorder_
            ? "录像 " + cfg.record.dir + "（预录 " + std::to_string(cfg.record.pre_seconds) + " 秒，收尾 " +
                  std::to_string(cfg.record.post_seconds) + " 秒，配额 " + std::to_string(cfg.record.quota_mb) + " MB）"
            : "不录像：" + why;
    why.clear();
    std::string user;
    r->rtsp_ = open_rtsp(cfg, user, why);
    const std::string rtsp_text =
        r->rtsp_ ? "RTSP " + r->rtsp_->url() + "（Digest 认证，用户 " + user + "）" : "不开 RTSP：" + why;
    if (!r->recorder_ && !r->rtsp_) {
        err = record_text + "；" + rtsp_text;
        return nullptr;
    }

    record::EventRecorder *rec = r->recorder_.get();
    stream::RtspServer *rtsp = r->rtsp_.get();
    r->encode_ = std::make_unique<EncodeStage>(std::move(enc), [rec, rtsp](const media::EncodedPacket &p) {
        if (rec != nullptr) {
            rec->on_packet(p);
        }
        if (rtsp != nullptr) {
            rtsp->push(p);
        }
    });
    r->encode_->start();
    r->description_ = "H.264 " + std::to_string(cfg.encode.bitrate_kbps) + " kbps；" + record_text + "；" + rtsp_text;
    return r;
}

#else

std::unique_ptr<Recording> Recording::open(const config::AppConfig & /*cfg*/, bool /*can_record*/, std::string &err)
{
    err = "这次编译没有 MPP，不能硬件编码（没有录像，也没有 RTSP）";
    return nullptr;
}

#endif

void Recording::attach(DetectHooks &hooks)
{
    EncodeStage *e = encode_.get();
    hooks.encode = [e](const std::shared_ptr<const media::YuvView> &yuv, PipelineClock::time_point captured) {
        e->submit(yuv, std::chrono::duration_cast<std::chrono::microseconds>(captured.time_since_epoch()).count());
    };
}

void Recording::on_result(const DetectResult &r, const media::Image *preview)
{
    if (!recorder_) {
        return;
    }
    std::vector<int> strangers;
    for (std::size_t i = 0; i < r.tracks.size() && i < r.identities.size(); ++i) {
        if (r.identities[i].state == face::IdState::stranger) {
            strangers.push_back(r.tracks[i].id);
        }
    }
    recorder_->update(strangers, now_us(), preview);
}

void Recording::tick()
{
    if (recorder_) {
        recorder_->update({}, now_us(), nullptr);
    }
}

void Recording::stop()
{
    if (stopped_ || !encode_) {
        return;
    }
    stopped_ = true;
    encode_->stop();  // 剩下的帧编完，包都交出去
    if (recorder_) {
        recorder_->stop();  // 结束正在录的事件
    }
    // RTSP 服务留到析构时再关（之后还要读它的统计）
}

std::string Recording::describe() const
{
    return description_;
}

}  // namespace rksec::app
