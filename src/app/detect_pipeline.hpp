#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "access/coordinator.hpp"
#include "core/bounded_queue.hpp"
#include "core/frame_pool.hpp"
#include "core/reorder_buffer.hpp"
#include "core/stage_timer.hpp"
#include "detect/yolo_post.hpp"
#include "face/align.hpp"
#include "face/face_pick.hpp"
#include "face/gallery.hpp"
#include "face/identity.hpp"
#include "face/score_stats.hpp"
#include "media/yuv_frame.hpp"
#include "track/tracker.hpp"
#include "ui/latest_image.hpp"

// 预览 + 检测流水线（第 6 步）。线程和数据流：
//
//   采集线程 ──push()──▶ [只留最新 1 帧] ──▶ media 线程：解码 → 预览图 → 放进信箱（界面线程随时拿最新的）
//                                                   └─ 有空闲的 NPU 输入缓冲区？
//                                                        有：转成 NPU 输入，编号（job 0、1、2…）交给推理线程
//                                                        没有：这一帧不检测（预览照常），计数
//   推理线程 × workers（每个绑一个 NPU 核）：推理 + 后处理 → 按 job 编号重新排序 → 最新结果（界面线程拿）
//
// 设计要点：
// - 预览不等检测：推理慢了、卡住了，预览照样流畅（非阻塞显示）。检测框比画面晚几十毫秒，是取舍
// - NPU 输入缓冲区个数 = 推理线程数：只有在有线程空着时才送新帧，不排队——
//   排队的帧等到被推理时已经旧了，宁可跳过它、等下一帧新鲜的（延迟优先）
// - 几个核同时推理，后送的帧可能先算完。结果按 job 编号重新排序后再放出去（ReorderBuffer），
//   保证下游（显示框、第 7 步的跟踪）看到的顺序和画面顺序一致
// - 每个 job 都一定有一个结果（失败也交一个失败的结果），否则排序会一直等它
// 人脸识别（第 7c 步，可选）：再开一个人脸线程，只处理一块，不排队：
//
//   media 线程：人脸线程空着、离上次送出过了 face_gap？ → 从最新的跟踪结果里挑一个人（IdentityBook::pick）
//               → 把这一帧里他的上半身那块转成人脸检测的输入 → 交给人脸线程
//   人脸线程：找脸 → 对齐 → 算特征 → 和底库比 → 记到这个编号上（IdentityBook::record）
//   之后每个检测结果里，跟踪编号都带上名字（DetectResult::identities）
//
// - 用的是“上一个检测结果”里人的位置，比这一帧晚几十毫秒；取景范围留了余量（face_search_region）
// - 人脸线程慢了只是名字出来得晚，不影响预览和检测
// - 具体怎么解码、转换、推理由 DetectHooks 提供：板子上是 MPP + RGA + RKNN，测试里是假的（带随机延迟），
//   这样线程、排序、缓冲区归还这些容易出错的逻辑在 CI 上用 TSan 也能测
namespace rksec::app {

using PipelineClock = std::chrono::steady_clock;

struct DetectHooks {
    // media 线程：一帧 MJPG → YUV（硬件解码）。失败返回 nullptr
    std::function<std::shared_ptr<const media::YuvView>(const Frame &, std::string &)> decode;
    // media 线程：YUV → 预览图（out 的尺寸由实现设置）
    std::function<bool(const media::YuvView &, media::Image &, std::string &)> preview;
    // media 线程：YUV → 第 slot 块 NPU 输入（letterbox）。为空表示不检测
    std::function<bool(const media::YuvView &, int slot, std::string &)> npu_input;
    // 第 worker 个推理线程：推理第 slot 块输入，结果是原画面坐标的框
    std::function<bool(int worker, int slot, std::vector<detect::Detection> &, std::string &)> infer;

    // media 线程：解码出来的一帧交给编码阶段（第 8a 步）。第二个参数是采集时间。要快、不能阻塞（EncodeStage::submit）
    std::function<void(const std::shared_ptr<const media::YuvView> &, PipelineClock::time_point)> encode;

    // 人脸识别（第 7c 步）。两个都有才做
    // media 线程：把 YUV 里 region 这一块（原画面坐标）转成人脸检测的输入
    std::function<bool(const media::YuvView &, const face::Region &, std::string &)> face_input;
    // 人脸线程：在刚才那一块里找脸、对齐、算特征
    struct FaceOutput {
        bool found = false;     // 找到了这个人自己的脸（几张时按 face::pick_face 挑，第 9a 步）
        detect::Detection box;  // 脸的位置（原画面坐标），score 是人脸检测的分数
        std::vector<float> embedding;  // 归一化的特征
        face::Pose pose;               // 粗略的头部姿态（统计用）
        face::Quality quality = face::Quality::ok;  // 挑中了脸但质量不够时 found = false，这里是原因（box、pose 照填）
        int faces = 0;                              // 裁的那块里一共检测到几张脸
        bool not_largest = false;  // 挑中的不是最大的那张（第 7c 步的规则会选错的情况）
        face::PickResult pick;     // 选脸的细节（没挑中时看最接近的那张差在哪）
    };
    std::function<bool(FaceOutput &, std::string &)> face_infer;
};

struct PipelineOptions {
    int workers = 1;                // 推理线程个数（0 = 不检测）
    track::TrackerOptions tracker;  // 跟踪参数（第 7a 步）
    // 人脸识别（第 7c 步）
    face::IdentityOptions identity;                // 多久认一次、几次一致才采纳
    std::shared_ptr<const face::Gallery> gallery;  // 底库；空指针 = 没有登记的人（有脸都是陌生人）
    float match_threshold = 0.4F;                  // 相似度达到多少算认出来
    std::chrono::milliseconds face_gap{100};       // 两次人脸识别至少隔多久（限制 NPU 占用）
};

// 只改推理线程数、其他用默认值（下面 face_report 用到 DetectPipeline::Stats，声明在类后面）
inline PipelineOptions with_workers(int n)
{
    PipelineOptions o;
    o.workers = n;
    return o;
}

struct DetectResult {
    std::uint64_t job = 0;               // 推理任务编号（连续）
    std::uint64_t frame = 0;             // 采集序号
    PipelineClock::time_point captured;  // 采集时间（驱动时间戳）
    PipelineClock::time_point done;      // 按顺序放出来的时间
    int worker = -1;
    bool ok = false;
    std::vector<detect::Detection> dets;
    // 跟踪（第 7a 步）：这一帧配上了的、已确认的轨迹。跟踪必须按帧顺序做，所以放在排序之后
    std::vector<track::Track> tracks;
    // 人脸识别（第 7c 步）：和 tracks 一一对应，每个编号现在的身份（没开人脸识别时都是 pending）
    std::vector<face::Identity> identities;
};

class DetectPipeline {
  public:
    DetectPipeline(PipelineOptions opts, DetectHooks hooks, ui::LatestImage &preview);
    ~DetectPipeline();
    DetectPipeline(const DetectPipeline &) = delete;
    DetectPipeline &operator=(const DetectPipeline &) = delete;

    void start();
    // 停止：不再接收新帧，已经送去推理的做完，线程全部退出。可以重复调用
    void stop();

    // 采集回调里调用：只留最新一帧，处理不过来时丢旧帧（旧帧马上还给驱动）
    void push(FrameRef f);

    // 界面线程：有比上次更新的结果就返回最新的那个（中间来不及拿的被覆盖、计数）
    std::optional<DetectResult> take_result();

    void set_access_session(std::uint64_t session) { access_session_.store(session); }
    std::optional<access::FaceEvent> take_face_event() { return face_events_.try_pop(); }
    bool detecting() const { return workers_ > 0; }
    bool recognizing() const { return face_enabled_; }
    int workers() const { return workers_; }

    struct Stats {
        std::uint64_t input_dropped = 0;  // 采集来的帧没来得及处理就被新帧挤掉
        std::uint64_t decoded = 0;        // 解码成功
        std::uint64_t decode_failed = 0;
        std::uint64_t preview_failed = 0;
        std::uint64_t jobs = 0;          // 送去推理的帧数
        std::uint64_t skipped_busy = 0;  // 推理线程都在忙，这一帧不检测
        std::uint64_t npu_input_failed = 0;
        std::uint64_t infer_failed = 0;
        std::uint64_t results = 0;              // 按顺序放出来的结果数（= jobs，停下来以后）
        std::uint64_t results_overwritten = 0;  // 界面线程没来得及拿就被新结果覆盖
        std::uint64_t reordered = 0;            // 到早了、等前面的结果的次数（发生乱序）
        std::size_t max_pending = 0;            // 排序时最多同时等着的结果个数
        std::vector<std::uint64_t> per_worker;  // 每个推理线程处理的帧数（看几个核是不是都用上了）
        track::Tracker::Stats tracks;           // 跟踪：确认过的轨迹总数、删除、找回、当前
        // 人脸识别（第 7c 步）
        face::IdentityBook::Stats faces;  // 识别了几次、没找到脸、认出来、陌生人
        std::uint64_t face_input_failed = 0;
        std::uint64_t face_infer_failed = 0;
        // 选脸（第 9a 步）：裁的块里不止一张脸的次数；挑中的不是最大那张的次数（举手机之类）；
        // 有脸但都不像是这个人自己的（全被排除，当作没找到脸）的次数
        std::uint64_t face_multi = 0;
        std::uint64_t face_not_largest = 0;
        std::uint64_t face_rejected = 0;
        // 挑中了这个人的脸，但质量不够、没拿去识别的次数，按原因（下标是 face::Quality）
        std::array<std::uint64_t, face::kQualityCount> face_low_quality{};
        face::ScoreStats scores;          // 识别分数（找到脸的每一次）
        float smallest_face = 0;          // 找到过的最小的脸宽（原画面像素）
        float smallest_matched_face = 0;  // 认出来过的最小的脸宽
        // 最近一次识别（看“站多远还认得出来”用）
        struct FaceSample {
            int track = 0;
            bool found = false;
            float width = 0;    // 脸宽（原画面像素）
            float score = 0;    // 和最像的人的相似度
            float second = -1;  // 和第二像的人的相似度（底库只有一个人时 -1）
            std::string name;   // 最像的人（不管是否达到阈值）
            face::Pose pose;
            float det_score = 0;  // 人脸检测的置信度
            int faces = 0;        // 块里检测到几张脸
            face::PickResult pick;
            face::Quality quality = face::Quality::ok;
            int region_width = 0;  // 裁的那块的宽
        } last_face;
    };
    Stats stats() const;

    // 各阶段耗时（线程安全，随时可以读）
    StageStats decode_time{"解码"};
    StageStats preview_time{"预览图"};
    StageStats npu_input_time{"NPU 输入（letterbox）"};
    StageStats infer_time{"推理 + 后处理"};
    StageStats preview_latency{"采集 → 预览可显示"};
    StageStats detect_latency{"采集 → 检测结果（端到端）"};
    StageStats face_input_time{"人脸输入（RGA 裁剪）"};
    StageStats face_time{"人脸（检测 + 对齐 + 特征）"};

  private:
    struct Job {
        std::uint64_t id = 0;
        std::uint64_t frame = 0;
        PipelineClock::time_point captured;
        int slot = -1;
    };

    struct FaceJob {
        int track = 0;
        int region_width = 0;
        std::uint64_t session = 0, sequence = 0;
        PipelineClock::time_point captured{};
    };

    void media_loop();
    void worker_loop(int w);
    void face_loop();
    void try_face(const media::YuvView &yuv, PipelineClock::time_point captured);
    int take_slot();
    void release_slot(int slot);
    void deliver(DetectResult r);

    const int workers_;
    const bool face_enabled_;
    const PipelineOptions opts_;
    DetectHooks hooks_;
    ui::LatestImage &preview_;

    BoundedQueue<FrameRef> input_{1};
    BoundedQueue<Job> jobs_;
    BoundedQueue<FaceJob> face_jobs_{1};
    BoundedQueue<access::FaceEvent> face_events_{32};
    std::atomic<std::uint64_t> access_session_{0};
    std::uint64_t face_sequence_ = 0;
    std::atomic<bool> face_busy_{false};   // 人脸线程正在用人脸输入缓冲区
    PipelineClock::time_point next_face_;  // media 线程自己用

    std::mutex slots_mutex_;
    std::vector<int> free_slots_;

    mutable std::mutex result_mutex_;  // 保护下面这些
    ReorderBuffer<DetectResult> reorder_;
    track::Tracker tracker_;
    face::IdentityBook book_;
    std::vector<track::Track> last_tracks_;  // 最近一个检测结果的跟踪结果（media 线程挑人用）
    std::optional<DetectResult> latest_;
    bool fresh_ = false;
    std::uint64_t results_ = 0;
    std::uint64_t overwritten_ = 0;

    mutable std::mutex stats_mutex_;
    Stats counts_;

    std::thread media_;
    std::vector<std::thread> threads_;
    std::thread face_;
    bool started_ = false;
    bool stopped_ = false;
};

// 结束报告里人脸识别的部分（第 9a 步）：选脸的情况一行 + 识别分数的统计（face::ScoreStats::report）
std::string face_report(const DetectPipeline::Stats &s, float threshold);

}  // namespace rksec::app
