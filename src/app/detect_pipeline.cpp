#include "app/detect_pipeline.hpp"

#include <pthread.h>

#include <algorithm>
#include <string>
#include <utility>

#include "core/log.hpp"

namespace rksec::app {

namespace {

constexpr std::uint64_t kLogFirstFailures = 5;  // 每种失败只写前几次日志，之后只计数，不刷屏

}  // namespace

DetectPipeline::DetectPipeline(PipelineOptions opts, DetectHooks hooks, ui::LatestImage &preview)
    : workers_(hooks.npu_input && hooks.infer ? std::max(opts.workers, 0) : 0),
      face_enabled_(workers_ > 0 && hooks.face_input && hooks.face_infer), opts_(std::move(opts)),
      hooks_(std::move(hooks)), preview_(preview), jobs_(static_cast<std::size_t>(std::max(workers_, 1))),
      tracker_(opts_.tracker), book_(opts_.identity)
{
    // NPU 输入缓冲区个数 = 推理线程数（见头文件：不排队）
    for (int s = 0; s < workers_; ++s) {
        free_slots_.push_back(s);
    }
    counts_.per_worker.assign(static_cast<std::size_t>(workers_), 0);
}

DetectPipeline::~DetectPipeline()
{
    stop();
}

void DetectPipeline::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    media_ = std::thread([this] { media_loop(); });
    for (int w = 0; w < workers_; ++w) {
        threads_.emplace_back([this, w] { worker_loop(w); });
    }
    if (face_enabled_) {
        face_ = std::thread([this] { face_loop(); });
    }
}

void DetectPipeline::stop()
{
    if (!started_ || stopped_) {
        return;
    }
    stopped_ = true;
    input_.close();
    media_.join();  // media 线程退出后不会再有新的 job
    jobs_.close();  // 推理线程把剩下的 job 做完再退出
    for (auto &t : threads_) {
        t.join();
    }
    face_jobs_.close();  // 人脸线程做完手上的一个再退出
    if (face_.joinable()) {
        face_.join();
    }
}

void DetectPipeline::push(FrameRef f)
{
    input_.push(std::move(f));
}

int DetectPipeline::take_slot()
{
    const std::lock_guard<std::mutex> lock(slots_mutex_);
    if (free_slots_.empty()) {
        return -1;
    }
    const int s = free_slots_.back();
    free_slots_.pop_back();
    return s;
}

void DetectPipeline::release_slot(int slot)
{
    const std::lock_guard<std::mutex> lock(slots_mutex_);
    free_slots_.push_back(slot);
}

void DetectPipeline::media_loop()
{
    pthread_setname_np(pthread_self(), "media");
    std::string err;
    std::uint64_t next_job = 0;
    while (auto f = input_.pop()) {
        const std::uint64_t seq = (*f)->sequence;
        const PipelineClock::time_point captured = (*f)->captured;

        auto t0 = PipelineClock::now();
        const std::shared_ptr<const media::YuvView> yuv = hooks_.decode(**f, err);
        decode_time.record(PipelineClock::now() - t0);
        f.reset();  // 解码时已经把压缩数据拷进解码器，采集缓冲区马上还给驱动
        if (!yuv) {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            if (counts_.decode_failed++ < kLogFirstFailures) {
                LOG_W("pipeline", "第 %llu 帧解码失败：%s", static_cast<unsigned long long>(seq), err.c_str());
            }
            continue;
        }

        // 先出预览：显示延迟最要紧，不等 NPU 输入
        t0 = PipelineClock::now();
        const bool shown = hooks_.preview(*yuv, preview_.back(), err);
        const auto t1 = PipelineClock::now();
        preview_time.record(t1 - t0);
        if (shown) {
            preview_.publish();
            preview_latency.record(t1 - captured);
        }

        if (hooks_.encode) {
            hooks_.encode(yuv, captured);
        }

        bool queued = false;
        bool busy = false;
        bool input_failed = false;
        if (workers_ > 0) {
            const int slot = take_slot();
            if (slot < 0) {
                busy = true;
            } else {
                t0 = PipelineClock::now();
                if (hooks_.npu_input(*yuv, slot, err)) {
                    npu_input_time.record(PipelineClock::now() - t0);
                    // job 编号只给真正送出去的帧，保证连续——排序靠它
                    jobs_.push(Job{next_job++, seq, captured, slot});
                    queued = true;
                } else {
                    release_slot(slot);
                    input_failed = true;
                }
            }
        }

        if (face_enabled_) {
            try_face(*yuv, captured);
        }

        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++counts_.decoded;
        if (!shown && counts_.preview_failed++ < kLogFirstFailures) {
            LOG_W("pipeline", "第 %llu 帧预览图失败：%s", static_cast<unsigned long long>(seq), err.c_str());
        }
        counts_.jobs += queued ? 1 : 0;
        counts_.skipped_busy += busy ? 1 : 0;
        if (input_failed && counts_.npu_input_failed++ < kLogFirstFailures) {
            LOG_W("pipeline", "第 %llu 帧 NPU 输入失败：%s", static_cast<unsigned long long>(seq), err.c_str());
        }
    }
}

void DetectPipeline::try_face(const media::YuvView &yuv, PipelineClock::time_point captured)
{
    if (face_busy_.load(std::memory_order_acquire)) {
        return;  // 人脸线程还在处理上一块
    }
    const auto session = access_session_.load();
    const auto now = PipelineClock::now();
    if (now < next_face_) {
        return;
    }
    std::optional<track::Track> who;
    {
        const std::lock_guard<std::mutex> lock(result_mutex_);
        if (const auto i = book_.pick(last_tracks_, now)) {
            who = last_tracks_[*i];
        }
    }
    if (!who) {
        return;
    }
    const face::Region region = face::face_search_region(who->box, yuv.layout.width, yuv.layout.height);
    std::string err;
    const auto t0 = PipelineClock::now();
    const bool ok = hooks_.face_input(yuv, region, err);
    face_input_time.record(PipelineClock::now() - t0);
    next_face_ = now + opts_.face_gap;
    {
        // 失败也算试过：按间隔再试，不在同一个人身上每帧都失败一次
        const std::lock_guard<std::mutex> lock(result_mutex_);
        book_.attempted(who->id, now);
    }
    if (!ok) {
        if (session) {
            face_events_.push(access::FaceEvent{session, ++face_sequence_, captured, who->id, 0, false, false, 0});
        }
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        if (counts_.face_input_failed++ < kLogFirstFailures) {
            LOG_W("pipeline", "人脸输入失败（编号 %d，区域 %d,%d %dx%d）：%s", who->id, region.x, region.y,
                  region.width, region.height, err.c_str());
        }
        return;
    }
    face_busy_.store(true, std::memory_order_release);
    face_jobs_.push(FaceJob{who->id, region.width, session, ++face_sequence_, captured});
}

void DetectPipeline::face_loop()
{
    pthread_setname_np(pthread_self(), "face");
    std::string err;
    while (auto job = face_jobs_.pop()) {
        DetectHooks::FaceOutput out;
        const auto t0 = PipelineClock::now();
        const bool ok = hooks_.face_infer(out, err);
        const auto t1 = PipelineClock::now();
        face_time.record(t1 - t0);
        face_busy_.store(false, std::memory_order_release);  // 输入缓冲区用完了，media 线程可以写下一块

        face::Observation obs;
        face::Match m;
        if (ok && out.found) {
            if (opts_.gallery) {
                m = opts_.gallery->match(out.embedding, opts_.match_threshold);
            }
            obs.face = true;
            obs.matched = m.matched;
            obs.name = m.name;
            obs.score = m.score;
        }
        {
            const std::lock_guard<std::mutex> lock(result_mutex_);
            book_.record(job->track, obs, t1);
        }
        if (job->session) {
            face_events_.push(access::FaceEvent{job->session, job->sequence, job->captured, job->track, m.person_id,
                                                ok && obs.face, obs.matched, static_cast<double>(m.score)});
        }
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        if (!ok) {
            if (counts_.face_infer_failed++ < kLogFirstFailures) {
                LOG_W("pipeline", "人脸识别失败（编号 %d）：%s", job->track, err.c_str());
            }
            continue;
        }
        const float w = out.box.x2 - out.box.x1;
        counts_.face_multi += out.faces > 1 ? 1 : 0;
        counts_.face_not_largest += out.found && out.not_largest ? 1 : 0;
        const bool low_quality = !out.found && out.quality != face::Quality::ok;
        counts_.face_rejected += !out.found && !low_quality && out.faces > 0 ? 1 : 0;
        if (low_quality) {
            ++counts_.face_low_quality[static_cast<std::size_t>(out.quality)];
        }
        if (out.found && opts_.gallery && !opts_.gallery->empty()) {
            counts_.scores.add({m.score, m.second, w, out.pose.yaw, out.pose.pitch, out.pose.valid});
        }
        auto &last = counts_.last_face;
        last.track = job->track;
        last.found = out.found;
        last.width = out.found ? w : 0;
        last.score = m.score;
        last.second = m.second;
        last.name = m.name;
        last.pose = out.pose;
        last.det_score = out.box.score;
        last.faces = out.faces;
        last.pick = out.pick;
        last.quality = out.quality;
        last.region_width = job->region_width;
        if (out.found && (counts_.smallest_face <= 0 || w < counts_.smallest_face)) {
            counts_.smallest_face = w;
        }
        if (obs.matched && (counts_.smallest_matched_face <= 0 || w < counts_.smallest_matched_face)) {
            counts_.smallest_matched_face = w;
        }
    }
}

void DetectPipeline::worker_loop(int w)
{
    const std::string name = "npu" + std::to_string(w);
    pthread_setname_np(pthread_self(), name.c_str());
    std::string err;
    while (auto job = jobs_.pop()) {
        DetectResult r;
        r.job = job->id;
        r.frame = job->frame;
        r.captured = job->captured;
        r.worker = w;
        const auto t0 = PipelineClock::now();
        r.ok = hooks_.infer(w, job->slot, r.dets, err);
        infer_time.record(PipelineClock::now() - t0);
        release_slot(job->slot);  // 结果已经拷出来了，缓冲区可以给下一帧
        {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++counts_.per_worker[static_cast<std::size_t>(w)];
            if (!r.ok && counts_.infer_failed++ < kLogFirstFailures) {
                LOG_W("pipeline", "第 %llu 帧推理失败（NPU %d）：%s", static_cast<unsigned long long>(r.frame), w,
                      err.c_str());
            }
        }
        if (!r.ok) {
            r.dets.clear();  // 失败的也要交给排序，否则后面的结果一直等它
        }
        deliver(std::move(r));
    }
}

void DetectPipeline::deliver(DetectResult r)
{
    const std::lock_guard<std::mutex> lock(result_mutex_);
    const std::uint64_t id = r.job;
    for (DetectResult &ready : reorder_.push(id, std::move(r))) {
        // 跟踪：这里结果已经按帧顺序排好。推理失败的帧不喂给跟踪器——“没算出来”不等于“没有人”，
        // 喂空列表会让所有轨迹白白丢失一帧
        if (ready.ok) {
            ready.tracks = tracker_.update(ready.dets);
        }
        ready.done = PipelineClock::now();
        if (face_enabled_ && ready.ok) {
            last_tracks_ = ready.tracks;
            book_.seen(ready.tracks, ready.done);
        }
        ready.identities.reserve(ready.tracks.size());
        for (const auto &t : ready.tracks) {
            ready.identities.push_back(book_.get(t.id));
        }
        detect_latency.record(ready.done - ready.captured);
        ++results_;
        if (fresh_) {
            ++overwritten_;
        }
        latest_ = std::move(ready);
        fresh_ = true;
    }
}

std::optional<DetectResult> DetectPipeline::take_result()
{
    const std::lock_guard<std::mutex> lock(result_mutex_);
    if (!fresh_) {
        return std::nullopt;
    }
    fresh_ = false;
    return latest_;
}

DetectPipeline::Stats DetectPipeline::stats() const
{
    Stats s;
    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        s = counts_;
    }
    s.input_dropped = input_.stats().dropped;
    const std::lock_guard<std::mutex> lock(result_mutex_);
    s.results = results_;
    s.results_overwritten = overwritten_;
    const auto rs = reorder_.stats();
    s.reordered = rs.held;
    s.max_pending = rs.max_pending;
    s.tracks = tracker_.stats();
    s.faces = book_.stats();
    return s;
}

std::string face_report(const DetectPipeline::Stats &s, float threshold)
{
    std::string out = "选脸       块里不止一张脸 " + std::to_string(s.face_multi) + " 次，挑中的不是最大那张 " +
                      std::to_string(s.face_not_largest) + " 次，有脸但都不像是这个人的（不识别）" +
                      std::to_string(s.face_rejected) + " 次\n";
    std::uint64_t low = 0;
    std::string why;
    for (int q = 1; q < face::kQualityCount; ++q) {
        const std::uint64_t n = s.face_low_quality[static_cast<std::size_t>(q)];
        low += n;
        if (n > 0) {
            why += (why.empty() ? "：" : "、") + std::string{face::quality_text(static_cast<face::Quality>(q))} + " " +
                   std::to_string(n);
        }
    }
    out += "脸的质量   不够、没拿去识别 " + std::to_string(low) + " 次" + why + "\n";
    out += s.scores.report(threshold);
    return out;
}

}  // namespace rksec::app
