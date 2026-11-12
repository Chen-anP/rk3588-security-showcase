// 预览 + 检测流水线（第 6 步，第 7c 步加了人脸识别）：用假的解码、转换、推理（带随机延迟）测线程逻辑。
// 这些问题只在多线程、乱序时出现，上板子很难复现：结果顺序倒退、缓冲区没还导致检测停掉、
// 推理卡住拖住预览、两帧的数据串到一起。CI 上 TSan 也跑这些测试，查数据竞争。
#include "access/pass_store.hpp"
#include "app/access_service.hpp"
#include "app/detect_pipeline.hpp"
#include <filesystem>
#include <sqlite3.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace rksec;
using namespace rksec::app;
using namespace std::chrono_literals;

namespace {

// 假的硬件：解码出来的 YuvView 用 fd 字段带上采集序号；NPU 输入缓冲区记住是哪一帧；
// 推理把“这块缓冲区里是哪一帧”作为框的类别交回去——结果和帧对不上就说明缓冲区串了
struct FakeHw {
    std::vector<std::uint64_t> slot_frame = std::vector<std::uint64_t>(8, 0);
    std::atomic<int> max_delay_ms{0};
    std::atomic<bool> fail_every_other{false};  // 第 2、4、6… 次推理失败
    std::atomic<int> infer_calls{0};
    std::atomic<bool> fail_decode{false};
    std::atomic<bool> walking_person{false};  // 推理结果换成一个从左往右走的人（测跟踪）
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool gate_open = true;
    std::atomic<int> inside{0};  // 正在推理的个数
    // 人脸：找到的脸的特征是第 face_axis 维为 1 的向量
    std::atomic<bool> face_found{true};
    std::atomic<int> face_axis{0};
    std::atomic<int> face_inputs{0};
    // 选脸（第 9a 步）：块里检测到几张脸（-1：找到时 1、没找到时 0）；挑中的是不是最大的那张
    std::atomic<int> faces_seen{-1};
    std::atomic<bool> not_largest{false};
    std::atomic<int> quality{0};  // face::Quality：不是 ok 时当作质量不够、不识别
    face::Region last_region;     // media 线程写，测试在 stop() 之后读

    DetectHooks hooks(bool detect = true, bool faces = false)
    {
        DetectHooks h;
        h.decode = [this](const Frame &f, std::string &err) -> std::shared_ptr<const media::YuvView> {
            if (fail_decode) {
                err = "假的解码失败";
                return nullptr;
            }
            auto v = std::make_shared<media::YuvView>();
            v->layout = {media::YuvFormat::nv12, 64, 32, 64, 32};
            v->fd = static_cast<int>(f.sequence);
            return v;
        };
        h.preview = [](const media::YuvView &v, media::Image &out, std::string & /*err*/) {
            out.width = 4;
            out.height = 2;
            out.stride = 16;
            out.pixels.assign(32, static_cast<std::uint8_t>(v.fd & 0xFF));
            return true;
        };
        if (!detect) {
            return h;
        }
        h.npu_input = [this](const media::YuvView &v, int slot, std::string & /*err*/) {
            slot_frame[static_cast<std::size_t>(slot)] = static_cast<std::uint64_t>(v.fd);
            return true;
        };
        h.infer = [this](int /*worker*/, int slot, std::vector<detect::Detection> &out, std::string &err) {
            ++inside;
            {
                std::unique_lock<std::mutex> lock(gate_mutex);
                gate_cv.wait(lock, [this] { return gate_open; });
            }
            thread_local std::mt19937 rng{std::random_device{}()};
            if (max_delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(rng() % static_cast<unsigned>(max_delay_ms + 1)));
            }
            const std::uint64_t frame = slot_frame[static_cast<std::size_t>(slot)];
            --inside;
            if (fail_every_other && ++infer_calls % 2 == 0) {
                err = "假的推理失败";
                return false;
            }
            detect::Detection d;
            d.cls = static_cast<int>(frame);
            if (walking_person) {
                const auto x = static_cast<float>(100 + 5 * frame);
                d = detect::Detection{x, 100, x + 80, 300, 0.9F, 0};
            }
            out = {d};
            return true;
        };
        if (!faces) {
            return h;
        }
        h.face_input = [this](const media::YuvView & /*v*/, const face::Region &r, std::string & /*err*/) {
            last_region = r;
            ++face_inputs;
            return true;
        };
        h.face_infer = [this](DetectHooks::FaceOutput &out, std::string & /*err*/) {
            out.quality = static_cast<face::Quality>(quality.load());
            out.found = face_found && out.quality == face::Quality::ok;
            out.faces = faces_seen >= 0 ? faces_seen.load() : (face_found ? 1 : 0);
            out.not_largest = not_largest;
            out.pose = face::Pose{0.3F, 0.5F, true};
            out.box = detect::Detection{10, 10, 40, 50, 0.99F, 0};
            out.embedding = axis(static_cast<std::size_t>(face_axis.load()));
            return true;
        };
        return h;
    }

    static std::vector<float> axis(std::size_t i)
    {
        std::vector<float> v(face::kEmbeddingSize, 0.0F);
        v[i] = 1;
        return v;
    }

    void close_gate()
    {
        const std::lock_guard<std::mutex> lock(gate_mutex);
        gate_open = false;
    }
    void open_gate()
    {
        {
            const std::lock_guard<std::mutex> lock(gate_mutex);
            gate_open = true;
        }
        gate_cv.notify_all();
    }
};

// 模拟摄像头：每 interval 送一帧，共 n 帧（缓冲区被占着时这一帧算丢，和真的驱动一样）
void feed(DetectPipeline &p, FramePool &pool, int n, std::chrono::milliseconds interval)
{
    for (int i = 0; i < n; ++i) {
        if (auto f = pool.acquire(i % pool.count(), static_cast<std::uint64_t>(i), std::chrono::steady_clock::now())) {
            p.push(std::move(f));
        }
        std::this_thread::sleep_for(interval);
    }
}

// 等条件成立，最多 5 秒（不用固定的 sleep 去猜“应该处理完了”：机器慢、TSan 慢时会猜错，D15、UiSim 的教训）
template <typename Pred> bool wait_until(Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// 一帧一帧送：每送一帧，等 media 线程处理完它再送下一帧，这样每一帧都会被处理、不会被挤掉，计数是确定的。
// idle = true 时还要等推理都做完（下一帧一定有空闲的推理线程）
void feed_one_by_one(DetectPipeline &p, FramePool &pool, int n, bool idle = false, int first = 0)
{
    for (int i = first; i < first + n; ++i) {
        auto f = pool.acquire(i % pool.count(), static_cast<std::uint64_t>(i), std::chrono::steady_clock::now());
        ASSERT_TRUE(f) << "缓冲区 " << i % pool.count() << " 还没还回来";
        p.push(std::move(f));
        const auto done = static_cast<std::uint64_t>(i + 1);
        ASSERT_TRUE(wait_until([&] {
            const auto s = p.stats();
            return s.decoded + s.decode_failed + s.input_dropped >= done && (!idle || s.results == s.jobs);
        })) << "第 "
            << i << " 帧 5 秒内没处理完";
    }
}

}  // namespace

TEST(DetectPipeline, ResultsComeOutInOrderAndMatchTheirFrames)
{
    FakeHw hw;
    hw.max_delay_ms = 6;  // 3 个线程、随机延迟：一定会乱序
    ui::LatestImage preview;
    DetectPipeline p{with_workers(3), hw.hooks(), preview};
    FramePool pool{4, [](int) {}};
    p.start();

    std::atomic<bool> done{false};
    std::uint64_t last_job = 0;
    bool any = false;
    bool order_ok = true;
    bool match_ok = true;
    std::thread ui([&] {
        while (!done) {
            if (auto r = p.take_result()) {
                if (any && r->job <= last_job) {
                    order_ok = false;
                }
                any = true;
                last_job = r->job;
                if (r->dets.size() != 1 || static_cast<std::uint64_t>(r->dets[0].cls) != r->frame) {
                    match_ok = false;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    feed(p, pool, 150, 1ms);
    p.stop();
    done = true;
    ui.join();

    const auto s = p.stats();
    EXPECT_TRUE(any);
    EXPECT_TRUE(order_ok) << "界面拿到的结果编号必须一直变大";
    EXPECT_TRUE(match_ok) << "结果和它的帧对不上：NPU 输入缓冲区被别的帧覆盖了";
    EXPECT_GT(s.jobs, 0U);
    EXPECT_EQ(s.results, s.jobs) << "每个送去推理的帧都要有结果（停下来时做完）";
    std::uint64_t sum = 0;
    for (const auto n : s.per_worker) {
        sum += n;
    }
    EXPECT_EQ(sum, s.jobs);
    EXPECT_EQ(s.decoded, s.jobs + s.skipped_busy) << "每一帧要么送去推理，要么因为忙被跳过";
    EXPECT_EQ(preview.stats().published, s.decoded) << "每一帧都出预览";
}

TEST(DetectPipeline, PreviewKeepsFlowingWhileInferenceIsStuck)
{
    FakeHw hw;
    hw.close_gate();  // 推理卡住
    ui::LatestImage preview;
    DetectPipeline p{with_workers(2), hw.hooks(), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_one_by_one(p, pool, 10);
    const auto s = p.stats();
    EXPECT_EQ(preview.stats().published, 10U) << "推理卡住时每一帧照样出预览";
    EXPECT_EQ(s.jobs, 2U) << "只有 2 块 NPU 输入缓冲区，都被卡住的推理占着";
    EXPECT_EQ(s.skipped_busy, 8U);
    EXPECT_FALSE(p.take_result()) << "还没有任何结果";
    hw.open_gate();
    p.stop();
    EXPECT_EQ(p.stats().results, 2U);
}

TEST(DetectPipeline, FailedInferenceDoesNotBlockLaterResults)
{
    FakeHw hw;
    hw.fail_every_other = true;
    hw.max_delay_ms = 2;
    ui::LatestImage preview;
    DetectPipeline p{with_workers(2), hw.hooks(), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_one_by_one(p, pool, 20, /*idle=*/true);  // 每一帧都有空闲线程：20 个 job，10 个失败
    p.stop();
    const auto s = p.stats();
    EXPECT_EQ(s.jobs, 20U);
    EXPECT_EQ(s.infer_failed, 10U);
    EXPECT_EQ(s.results, s.jobs) << "失败的也交了结果，排序没有卡住";
    const auto r = p.take_result();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value().job + 1, s.jobs) << "最后拿到的是最后一个 job";
}

TEST(DetectPipeline, WithoutDetectHooksItIsPreviewOnly)
{
    FakeHw hw;
    ui::LatestImage preview;
    DetectPipeline p{with_workers(3), hw.hooks(false), preview};
    EXPECT_FALSE(p.detecting());
    FramePool pool{4, [](int) {}};
    p.start();
    feed_one_by_one(p, pool, 10);
    p.stop();
    EXPECT_EQ(preview.stats().published, 10U);
    EXPECT_EQ(p.stats().jobs, 0U);
    EXPECT_FALSE(p.take_result());
}

TEST(DetectPipeline, DecodeFailuresAreCountedAndFramesReturned)
{
    FakeHw hw;
    hw.fail_decode = true;
    ui::LatestImage preview;
    std::atomic<int> released{0};
    FramePool pool{4, [&](int) { ++released; }};
    {
        DetectPipeline p{with_workers(1), hw.hooks(), preview};
        p.start();
        feed_one_by_one(p, pool, 10);
        p.stop();
        EXPECT_EQ(p.stats().decode_failed, 10U);
        EXPECT_EQ(p.stats().decoded, 0U);
    }
    EXPECT_EQ(pool.in_use(), 0) << "解码失败的帧也要还给采集";
    EXPECT_GT(released.load(), 0);
}

TEST(DetectPipeline, StopIsSafeToCallTwiceAndBeforeStart)
{
    FakeHw hw;
    ui::LatestImage preview;
    DetectPipeline never_started{with_workers(1), hw.hooks(), preview};
    never_started.stop();
    DetectPipeline p{with_workers(1), hw.hooks(), preview};
    p.start();
    p.stop();
    p.stop();
    SUCCEED();
}

TEST(DetectPipeline, ResultsCarryTrackIdsInFrameOrder)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.max_delay_ms = 3;
    ui::LatestImage preview;
    DetectPipeline p{with_workers(2), hw.hooks(), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_one_by_one(p, pool, 10, /*idle=*/true);
    p.stop();
    const auto r = p.take_result();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r.value().tracks.size(), 1U) << "一个人走了 10 帧，应该有一条已确认的轨迹";
    EXPECT_EQ(r.value().tracks[0].id, 1);
    EXPECT_EQ(p.stats().tracks.created, 1U) << "两个推理线程交替算，排好序再跟踪，编号不会断";
    ASSERT_EQ(r.value().identities.size(), 1U) << "每条轨迹都带一个身份";
    EXPECT_EQ(r.value().identities[0].state, face::IdState::pending) << "没开人脸识别";
    EXPECT_FALSE(p.recognizing());
}

namespace {

// 人脸识别的测试：一个人一直在走，人脸线程每次都看到同一张脸。
// 每次只送一帧、等它处理完；一直送到 done() 成立（人脸线程是异步的，不能假设第几帧时一定做完了）
PipelineOptions face_options(std::shared_ptr<const face::Gallery> gallery)
{
    PipelineOptions o = with_workers(1);
    o.gallery = std::move(gallery);
    o.face_gap = 0ms;
    o.identity.retry = 0ms;
    o.identity.recheck = 0ms;
    return o;
}

template <typename Pred> int feed_until(DetectPipeline &p, FramePool &pool, Pred done)
{
    int i = 0;
    for (; i < 300 && !done(); ++i) {
        feed_one_by_one(p, pool, 1, /*idle=*/true, i);
    }
    return i;
}

}  // namespace

TEST(DetectPipeline, FaceRecognitionNamesTheTrack)
{
    FakeHw hw;
    hw.walking_person = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("张三", FakeHw::axis(0));
    gallery->add("李四", FakeHw::axis(1));
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    EXPECT_TRUE(p.recognizing());
    FramePool pool{4, [](int) {}};
    p.start();
    const int n = feed_until(p, pool, [&] { return p.stats().faces.matched >= 2; });
    feed_one_by_one(p, pool, 1, /*idle=*/true, n);  // 再来一帧：结果里带上采纳后的名字
    p.stop();
    const auto r = p.take_result();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->tracks.size(), 1U);
    ASSERT_EQ(r->identities.size(), 1U);
    EXPECT_EQ(r->identities[0].state, face::IdState::known);
    EXPECT_EQ(r->identities[0].name, "张三");
    const auto s = p.stats();
    EXPECT_GE(s.faces.attempts, 2U);
    EXPECT_EQ(s.faces.unmatched, 0U);
    EXPECT_EQ(s.last_face.track, 1);
    EXPECT_FLOAT_EQ(s.last_face.width, 30);
    EXPECT_FLOAT_EQ(s.smallest_matched_face, 30);
    EXPECT_GT(hw.last_region.width, 0) << "media 线程按人的位置取了一块";
}

TEST(DetectPipeline, FaceNotInGalleryIsAStranger)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.face_axis = 7;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("张三", FakeHw::axis(0));
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    const int n = feed_until(p, pool, [&] { return p.stats().faces.unmatched >= 2; });
    feed_one_by_one(p, pool, 1, /*idle=*/true, n);
    p.stop();
    const auto r = p.take_result();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->identities.size(), 1U);
    EXPECT_EQ(r->identities[0].state, face::IdState::stranger);
    EXPECT_EQ(p.stats().last_face.name, "张三") << "最像的人照样记下来（看分数用），只是没达到阈值";
}

TEST(DetectPipeline, NoFaceFoundKeepsThePersonPending)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.face_found = false;  // 背对镜头
    ui::LatestImage preview;
    DetectPipeline p{face_options(nullptr), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    const int n = feed_until(p, pool, [&] { return p.stats().faces.no_face >= 3; });
    feed_one_by_one(p, pool, 1, /*idle=*/true, n);
    p.stop();
    const auto r = p.take_result();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->identities.size(), 1U);
    EXPECT_EQ(r->identities[0].state, face::IdState::pending);
    EXPECT_EQ(p.stats().smallest_face, 0.0F);
}

TEST(DetectPipeline, FacePickAndScoresAreCounted)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.faces_seen = 2;  // 块里两张脸，挑中的不是最大那张（举着手机）
    hw.not_largest = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("张三", FakeHw::axis(0));
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_until(p, pool, [&] { return p.stats().faces.matched >= 2; });
    p.stop();
    const auto s = p.stats();
    EXPECT_GE(s.face_multi, 2U);
    EXPECT_EQ(s.face_not_largest, s.face_multi);
    EXPECT_EQ(s.face_rejected, 0U);
    EXPECT_EQ(s.scores.count(), s.faces.matched + s.faces.unmatched) << "找到脸的每一次都记分数";
    EXPECT_FLOAT_EQ(s.scores.percentile(0.5), 1.0F);
    EXPECT_FLOAT_EQ(s.last_face.pose.yaw, 0.3F);
    EXPECT_FLOAT_EQ(s.last_face.second, -1.0F) << "底库只有一个人";
    const std::string rep = face_report(s, 0.4F);
    EXPECT_NE(rep.find("挑中的不是最大那张 " + std::to_string(s.face_not_largest) + " 次"), std::string::npos) << rep;
    EXPECT_NE(rep.find("鼻子偏画面右"), std::string::npos) << rep;
    EXPECT_GT(hw.last_region.person.x2, hw.last_region.person.x1) << "裁的块带着人框（挑脸用）";
}

TEST(DetectPipeline, FacesThatAreNotThePersonsOwnAreNotRecognized)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.face_found = false;  // 块里有两张脸，但都不在这个人的头部位置
    hw.faces_seen = 2;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("张三", FakeHw::axis(0));
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_until(p, pool, [&] { return p.stats().faces.no_face >= 3; });
    p.stop();
    const auto s = p.stats();
    EXPECT_GE(s.face_rejected, 3U);
    EXPECT_EQ(s.faces.unmatched, 0U) << "不拿别的脸去认成陌生人";
    EXPECT_EQ(s.scores.count(), 0U);
    EXPECT_NE(face_report(s, 0.4F).find("没有样本"), std::string::npos);
}

TEST(DetectPipeline, LowQualityFacesAreNotRecognized)
{
    FakeHw hw;
    hw.walking_person = true;
    hw.quality = static_cast<int>(face::Quality::tilted);  // 抬头太高：挑中了自己的脸，但不拿去识别
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("张三", FakeHw::axis(0));
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    p.start();
    feed_until(p, pool, [&] { return p.stats().faces.no_face >= 3; });
    p.stop();
    const auto s = p.stats();
    EXPECT_GE(s.face_low_quality[static_cast<std::size_t>(face::Quality::tilted)], 3U);
    EXPECT_EQ(s.face_rejected, 0U) << "质量不够和“不是他的脸”分开算";
    EXPECT_EQ(s.faces.matched + s.faces.unmatched, 0U);
    EXPECT_EQ(s.last_face.quality, face::Quality::tilted);
    EXPECT_NE(face_report(s, 0.4F).find("抬头低头太大"), std::string::npos) << face_report(s, 0.4F);
}

TEST(DetectPipeline, AccessEventsCarryStableIdSessionAndSourceTime)
{
    FakeHw hw;
    hw.walking_person = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("TestPerson", FakeHw::axis(0), 42);
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    p.set_access_session(7);
    FramePool pool{4, [](int) {}};
    p.start();
    feed_until(p, pool, [&] { return p.stats().faces.matched >= 2; });
    p.stop();
    int count = 0;
    std::uint64_t last = 0;
    while (auto event = p.take_face_event()) {
        EXPECT_EQ(event->session, 7U);
        EXPECT_EQ(event->person_id, 42);
        EXPECT_TRUE(event->matched);
        EXPECT_GT(event->sequence, last);
        last = event->sequence;
        EXPECT_NE(event->captured, PipelineClock::time_point{});
        ++count;
    }
    EXPECT_GE(count, 2);
}
TEST(DetectPipeline, AccessSessionIsBoundBeforeInferenceCompletes)
{
    FakeHw hw;
    hw.walking_person = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("TestPerson", FakeHw::axis(0), 42);
    auto hooks = hw.hooks(true, true);
    auto infer = hooks.face_infer;
    std::atomic<bool> entered{false}, release{false};
    hooks.face_infer = [&](DetectHooks::FaceOutput &out, std::string &error) {
        entered = true;
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
        return infer(out, error);
    };
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hooks, preview};
    p.set_access_session(7);
    FramePool pool{4, [](int) {}};
    p.start();
    int n = feed_until(p, pool, [&] { return entered.load(); });
    p.set_access_session(8);
    release = true;
    feed_one_by_one(p, pool, 1, true, n);
    p.stop();
    const auto first = p.take_face_event();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->session, 7U);
}
TEST(AccessService, FakePipelineWritesOneRealPassWithoutBlockingPreview)
{
    const auto dir = std::filesystem::temp_directory_path() / ("rksec_gate_service_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path = (dir / "test.db").string();
    std::string error;
    auto seed = access::PassStore::open(path, error);
    ASSERT_TRUE(seed) << error;
    seed.reset();
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO person(id,name) VALUES(42,'TestPerson')", nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    FakeHw hw;
    hw.walking_person = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("TestPerson", FakeHw::axis(0), 42);
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    config::AppConfig cfg;
    cfg.face.database = path;
    cfg.pir.chip = "/dev/gpiochip999999";
    AccessService gate{cfg};
    p.start();
    auto shown = PipelineClock::now();
    const auto deadline = PipelineClock::now() + 5s;
    int n = 0;
    while (!gate.view().grants && PipelineClock::now() < deadline) {
        feed_one_by_one(p, pool, 1, true, n++);
        if (preview.take()) {
            shown = PipelineClock::now();
        }
        auto result = p.take_result();
        gate.pump(p, result ? &*result : nullptr, &preview.front(), shown);
        std::this_thread::sleep_for(10ms);
    }
    gate.stop();
    p.stop();
    EXPECT_EQ(gate.view().grants, 1U);
    EXPECT_GT(p.stats().results, 3U);
    auto query = access::PassStore::open(path, error, true);
    ASSERT_TRUE(query) << error;
    std::vector<access::PassRow> rows;
    ASSERT_TRUE(query->list(rows, 20, error));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].person_id, 42);
    EXPECT_TRUE(rows[0].has_snapshot);
    query.reset();
    std::filesystem::remove_all(dir);
}
TEST(AccessService, StoreFailureLeavesPreviewRunningAndNeverReportsOpen)
{
    FakeHw hw;
    hw.walking_person = true;
    auto gallery = std::make_shared<face::Gallery>();
    gallery->add("TestPerson", FakeHw::axis(0), 42);
    ui::LatestImage preview;
    DetectPipeline p{face_options(gallery), hw.hooks(true, true), preview};
    FramePool pool{4, [](int) {}};
    config::AppConfig cfg;
    cfg.face.database = "/proc/rksec-test-no-db/faces.db";
    cfg.pir.chip = "/dev/gpiochip999999";
    AccessService gate{cfg};
    p.start();
    auto shown = PipelineClock::now();
    const auto deadline = shown + 5s;
    int n = 0;
    while (!gate.view().write_errors && PipelineClock::now() < deadline) {
        feed_one_by_one(p, pool, 1, true, n++);
        if (preview.take()) {
            shown = PipelineClock::now();
        }
        auto result = p.take_result();
        gate.pump(p, result ? &*result : nullptr, &preview.front(), shown);
        std::this_thread::sleep_for(10ms);
    }
    const auto before = p.stats().results;
    feed_one_by_one(p, pool, 3, true, n);
    gate.stop();
    p.stop();
    EXPECT_EQ(gate.view().grants, 0U);
    EXPECT_GT(gate.view().write_errors, 0U);
    EXPECT_FALSE(gate.view().persisted);
    EXPECT_GT(p.stats().results, before);
}
