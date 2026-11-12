// 事件录像（第 8b 步）：预录缓冲、事件状态机、配额、事件表、MP4 写入、录像器整体；
// 导出（转普通 MP4）、删除、列表（第 8d 步）。
// MP4 用一段真实的小码流（tests/data/tiny_h264.hpp）写出来，再用 libavformat 读回来检查。
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "data/tiny_h264.hpp"
#include "face/face_db.hpp"
#include "media/h264_stream.hpp"
#include "record/event_admin.hpp"
#include "record/event_logic.hpp"
#include "record/event_recorder.hpp"
#include "record/event_store.hpp"
#include "record/mp4_remux.hpp"
#include "record/mp4_writer.hpp"
#include "record/pre_record.hpp"
#include "record/retention.hpp"

using rksec::media::EncodedPacket;
using rksec::record::EventAction;
using rksec::record::EventOptions;
using rksec::record::EventRow;
using rksec::record::EventStore;
using rksec::record::EventTracker;
using rksec::record::PacketPtr;
using rksec::record::PreRecordBuffer;

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kSec = 1'000'000;
constexpr std::int64_t kFrame = 40'000;  // 25 fps

PacketPtr pkt(std::int64_t pts, bool key, std::size_t bytes = 100)
{
    auto p = std::make_shared<EncodedPacket>();
    p->pts_us = pts;
    p->keyframe = key;
    p->data.assign(bytes, 0);
    return p;
}

// 把 tiny_h264 切成一帧一个包（一个包 = 前面的 SPS/PPS/SEI + 一个 slice），按 25 fps 编时间
std::vector<EncodedPacket> tiny_frames()
{
    const std::uint8_t *d = rksec::testdata::kTinyH264;
    const std::size_t n = sizeof(rksec::testdata::kTinyH264);
    const auto nals = rksec::media::split_annexb(d, n);
    std::vector<EncodedPacket> out;
    std::size_t begin = 0;  // 这一帧从哪个字节开始（含起始码）
    for (std::size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type != rksec::media::kNalSlice && nals[i].type != rksec::media::kNalIdr) {
            continue;
        }
        const std::size_t end = i + 1 < nals.size() ? nals[i + 1].start - 3 : n;
        std::size_t e = end;
        while (e > begin && d[e - 1] == 0) {
            --e;  // 下一个 4 字节起始码多出来的 00
        }
        EncodedPacket p;
        p.data.assign(d + begin, d + e);
        p.keyframe = nals[i].type == rksec::media::kNalIdr;
        p.pts_us = static_cast<std::int64_t>(out.size()) * kFrame;
        out.push_back(std::move(p));
        begin = e;
    }
    return out;
}

std::vector<std::uint8_t> tiny_sps_pps()
{
    return rksec::media::extract_parameter_sets(rksec::testdata::kTinyH264, sizeof(rksec::testdata::kTinyH264));
}

struct ReadBack {
    int packets = 0;
    int keyframes = 0;
    bool first_is_key = false;
    double duration_s = 0;
    int width = 0;
    bool ok = false;
};

// 用 libavformat 把 MP4 读回来
ReadBack read_mp4(const std::string &path)
{
    ReadBack r;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
        return r;
    }
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->nb_streams == 1) {
        r.width = ctx->streams[0]->codecpar->width;
        AVPacket *p = av_packet_alloc();
        std::int64_t last = 0;
        while (av_read_frame(ctx, p) >= 0) {
            if (r.packets == 0) {
                r.first_is_key = (p->flags & AV_PKT_FLAG_KEY) != 0;
            }
            r.keyframes += (p->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0;
            ++r.packets;
            last = p->pts + p->duration;
            av_packet_unref(p);
        }
        r.duration_s = static_cast<double>(last) * av_q2d(ctx->streams[0]->time_base);
        av_packet_free(&p);
        r.ok = true;
    }
    avformat_close_input(&ctx);
    return r;
}

class RecordDir : public ::testing::Test {
  protected:
    void SetUp() override
    {
        dir = fs::temp_directory_path() / ("rksec_record_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
        db = (dir / "data.db").string();
    }
    void TearDown() override { fs::remove_all(dir); }
    fs::path dir;
    std::string db;
    std::string err;
};

}  // namespace

// ---------------------------------------------------------------- 预录缓冲

TEST(PreRecord, StartsAtKeyframeAndKeepsAtLeastTheWindow)
{
    PreRecordBuffer b{5 * kSec};
    b.push(pkt(0, false));  // 开头不是关键帧：不要
    EXPECT_EQ(b.size(), 0U);
    // 20 秒，每 2 秒一个关键帧（GOP 50）
    for (std::int64_t t = kFrame; t <= 20 * kSec; t += kFrame) {
        b.push(pkt(t, (t / kFrame) % 50 == 1));
    }
    const auto s = b.snapshot();
    ASSERT_FALSE(s.empty());
    EXPECT_TRUE(s.front()->keyframe) << "第一个包一定是关键帧";
    EXPECT_GE(b.span_us(), 5 * kSec) << "至少 5 秒";
    EXPECT_LT(b.span_us(), 7 * kSec + kFrame) << "最多多一个 GOP";
    EXPECT_EQ(b.bytes(), b.size() * 100);
}

TEST(PreRecord, CapWithoutKeyframesDoesNotGrowForever)
{
    PreRecordBuffer b{5 * kSec, 100};
    b.push(pkt(0, true));
    for (int i = 1; i < 500; ++i) {
        b.push(pkt(i * kFrame, false));  // 再也没有关键帧
    }
    EXPECT_LE(b.size(), 100U);
    b.push(pkt(500 * kFrame, true));
    EXPECT_TRUE(b.snapshot().front()->keyframe);
    b.clear();
    EXPECT_EQ(b.size(), 0U);
    EXPECT_EQ(b.bytes(), 0U);
}

// ---------------------------------------------------------------- 状态机

TEST(EventTracker, StartsOnStrangerStopsAfterPostRoll)
{
    EventTracker t{EventOptions{10 * kSec, 300 * kSec}};
    EXPECT_EQ(t.update(false, 0), EventAction::none);
    EXPECT_EQ(t.update(true, 1 * kSec), EventAction::start);
    EXPECT_TRUE(t.active());
    EXPECT_EQ(t.update(true, 2 * kSec), EventAction::none);
    EXPECT_EQ(t.update(false, 8 * kSec), EventAction::none) << "被挡住 6 秒：还没到收尾时间";
    EXPECT_EQ(t.update(true, 9 * kSec), EventAction::none) << "又出现了：同一个事件";
    EXPECT_EQ(t.update(false, 18 * kSec), EventAction::none);
    EXPECT_EQ(t.update(false, 19 * kSec), EventAction::stop) << "最后一次看到后 10 秒";
    EXPECT_FALSE(t.active());
    EXPECT_EQ(t.update(true, 20 * kSec), EventAction::start) << "新的事件";
    EXPECT_EQ(t.event_start_us(), 20 * kSec);
}

TEST(EventTracker, SplitsLongEvents)
{
    EventTracker t{EventOptions{10 * kSec, 60 * kSec}};
    EXPECT_EQ(t.update(true, 0), EventAction::start);
    int splits = 0;
    for (std::int64_t now = kSec; now <= 150 * kSec; now += kSec) {
        splits += t.update(true, now) == EventAction::split ? 1 : 0;
    }
    EXPECT_EQ(splits, 2) << "60、120 秒各切一次";
    EXPECT_TRUE(t.active());
}

// ---------------------------------------------------------------- 配额

TEST(Retention, DeletesOldestUntilUnderQuota)
{
    using rksec::record::plan_retention;
    EXPECT_TRUE(plan_retention({{1, 100}, {2, 100}}, 300).empty());
    EXPECT_EQ(plan_retention({{3, 100}, {1, 100}, {2, 100}}, 150), (std::vector<std::int64_t>{1, 2})) << "从旧到新";
    EXPECT_EQ(plan_retention({{1, 100}, {2, 100}, {3, 100}}, 150, 1), (std::vector<std::int64_t>{2, 3}))
        << "正在录的（1）不删";
    EXPECT_TRUE(plan_retention({}, 0).empty());
}

// ---------------------------------------------------------------- 事件表

TEST_F(RecordDir, EventStoreLifecycle)
{
    auto s = EventStore::open(db, err);
    ASSERT_TRUE(s) << err;
    std::int64_t id = 0;
    ASSERT_TRUE(s->begin("stranger", "2026-11-04 22:00:00", "3", {0xFF, 0xD8}, id, err)) << err;
    ASSERT_TRUE(s->set_file(id, "/x/a.mp4", err));
    std::vector<EventRow> rows;
    ASSERT_TRUE(s->list(rows, err, 10, "recording"));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_TRUE(rows[0].has_snapshot);
    ASSERT_TRUE(s->finish(id, "2026-11-04 22:00:30", 30000, 12345, "3,5", "complete", err));
    ASSERT_TRUE(s->clips(rows, err));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].tracks, "3,5");
    EXPECT_EQ(rows[0].bytes, 12345);
    EXPECT_EQ(rows[0].duration_ms, 30000);
    std::vector<std::uint8_t> jpg;
    ASSERT_TRUE(s->snapshot(id, jpg, err));
    EXPECT_EQ(jpg.size(), 2U);
    ASSERT_TRUE(s->mark(id, "deleted", err));
    ASSERT_TRUE(s->clips(rows, err));
    EXPECT_TRUE(rows.empty()) << "删了文件的不算占用";
    ASSERT_TRUE(s->list(rows, err));
    ASSERT_EQ(rows.size(), 1U) << "记录留着";
    EXPECT_FALSE(rows[0].has_snapshot) << "截图一起清掉";
    EXPECT_FALSE(s->snapshot(999, jpg, err));
}

TEST_F(RecordDir, Version1DatabaseIsUpgradedWithoutLosingFaces)
{
    // 模拟第 7d 步的库：只有 person、face，user_version = 1
    sqlite3 *raw = nullptr;
    ASSERT_EQ(sqlite3_open(db.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(raw,
                     "CREATE TABLE person (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, created TEXT NOT NULL "
                     "DEFAULT '');"
                     "CREATE TABLE face (id INTEGER PRIMARY KEY, person_id INTEGER NOT NULL REFERENCES person(id) "
                     "ON DELETE CASCADE, model TEXT NOT NULL, embedding BLOB NOT NULL, aligned BLOB, source TEXT, "
                     "created TEXT NOT NULL DEFAULT '');"
                     "INSERT INTO person (name) VALUES ('赵小明'); PRAGMA user_version = 1;",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    sqlite3_close(raw);

    auto s = EventStore::open(db, err);
    ASSERT_TRUE(s) << err;
    std::int64_t id = 0;
    EXPECT_TRUE(s->begin("stranger", "t", "", {}, id, err)) << err;
    auto f = rksec::face::FaceDb::open(db, err);
    ASSERT_TRUE(f) << err;
    std::vector<rksec::face::PersonInfo> people;
    ASSERT_TRUE(f->people("m", people, err));
    ASSERT_EQ(people.size(), 1U) << "升级不动已有的人";
    EXPECT_EQ(people[0].name, "赵小明");

    ASSERT_EQ(sqlite3_open(db.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(raw, "PRAGMA user_version;", -1, &st, nullptr);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), rksec::face::kDatabaseVersion);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

// ---------------------------------------------------------------- MP4

TEST_F(RecordDir, Mp4RoundTrip)
{
    const auto frames = tiny_frames();
    ASSERT_EQ(frames.size(), 30U) << "测试码流应该切出 30 帧";
    EXPECT_TRUE(frames[0].keyframe);
    const std::string path = (dir / "a.mp4").string();
    auto w = rksec::record::Mp4Writer::open(path, {64, 48, tiny_sps_pps()}, err);
    ASSERT_TRUE(w) << err;
    for (std::size_t i = 1; i < frames.size(); ++i) {  // 故意从第 2 帧（P 帧）开始送
        ASSERT_TRUE(w->write(frames[i], err)) << err;
    }
    EXPECT_EQ(w->skipped(), 9U) << "第一个关键帧（第 10 帧）之前的 9 个 P 帧被丢掉";
    EXPECT_EQ(w->packets(), 20U);
    ASSERT_TRUE(w->close(err)) << err;
    EXPECT_EQ(w->duration_us(), 20 * kFrame);

    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777U, 0600U) << "录像里有人：只有自己能读";
    const ReadBack r = read_mp4(path);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.packets, 20);
    EXPECT_TRUE(r.first_is_key);
    EXPECT_EQ(r.keyframes, 2) << "第 10、20 帧";
    EXPECT_EQ(r.width, 64);
    EXPECT_NEAR(r.duration_s, 0.8, 0.05);
}

TEST_F(RecordDir, Mp4IsReadableAfterTheProcessIsKilled)
{
    // 分段 MP4：写到一半进程没了，写完的段照样能读。用子进程模拟 kill -9：
    // 子进程写完 25 帧（关键帧在 0、10、20）后直接 _exit，不关文件、不写文件尾、不跑析构
    const auto frames = tiny_frames();
    const std::string path = (dir / "b.mp4").string();
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        std::string e;
        auto w = rksec::record::Mp4Writer::open(path, {64, 48, tiny_sps_pps()}, e);
        for (std::size_t i = 0; w && i < 25; ++i) {
            w->write(frames[i], e);
        }
        ::_exit(w ? 0 : 1);  // NOLINT：模拟进程被杀，什么都不收拾
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const ReadBack r = read_mp4(path);
    ASSERT_TRUE(r.ok) << "没有文件尾也能打开";
    EXPECT_EQ(r.packets, 20) << "前两个 GOP（0～19 帧）的段已经在文件里，最后一个 GOP 没写完的丢了";
    EXPECT_TRUE(r.first_is_key);
}

TEST_F(RecordDir, Mp4RejectsMissingHeaders)
{
    EXPECT_FALSE(rksec::record::Mp4Writer::open((dir / "c.mp4").string(), {64, 48, {}}, err));
    EXPECT_FALSE(rksec::record::Mp4Writer::open((dir / "no_dir" / "c.mp4").string(), {64, 48, tiny_sps_pps()}, err));
}

// ---------------------------------------------------------------- 录像器整体

namespace {

rksec::record::RecorderOptions recorder_options(const fs::path &dir, std::int64_t quota = std::int64_t{1} << 30)
{
    rksec::record::RecorderOptions o;
    o.dir = (dir / "events").string();
    o.pre_us = 1 * kSec;
    o.event.post_us = 2 * kSec;
    o.event.max_us = 300 * kSec;
    o.quota_bytes = quota;
    o.video = {64, 48, tiny_sps_pps()};
    return o;
}

// 按 25 fps 送 n 帧（循环用测试码流，时间往后接）；strangers_from..strangers_to（帧号）之间有陌生人
void play(rksec::record::EventRecorder &rec, int n, int strangers_from, int strangers_to, std::int64_t &t)
{
    const auto frames = tiny_frames();
    rksec::media::Image preview;
    preview.width = 4;
    preview.height = 2;
    preview.stride = 16;
    preview.pixels.assign(32, 0x80);
    for (int i = 0; i < n; ++i) {
        EncodedPacket p = frames[static_cast<std::size_t>(i) % frames.size()];
        p.pts_us = t;
        rec.on_packet(p);
        const bool s = i >= strangers_from && i < strangers_to;
        rec.update(s ? std::vector<int>{7} : std::vector<int>{}, t, &preview);
        t += kFrame;
    }
}

}  // namespace

TEST_F(RecordDir, RecordsOneEventWithPreRollAndPostRoll)
{
    auto store = EventStore::open(db, err);
    ASSERT_TRUE(store) << err;
    rksec::record::EventRecorder rec{recorder_options(dir), std::move(store)};
    ASSERT_TRUE(rec.start(err)) << err;
    std::int64_t t = 0;
    // 0～3 秒没人，3～5 秒陌生人，之后没人；一共 10 秒
    play(rec, 250, 75, 125, t);
    rec.stop();

    const auto st = rec.stats();
    EXPECT_EQ(st.events, 1U);
    EXPECT_EQ(st.files, 1U);
    EXPECT_EQ(st.write_errors, 0U);
    auto s2 = EventStore::open(db, err);
    std::vector<EventRow> rows;
    ASSERT_TRUE(s2->list(rows, err));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].status, "complete");
    EXPECT_EQ(rows[0].tracks, "7");
    EXPECT_TRUE(rows[0].has_snapshot);
    EXPECT_TRUE(fs::exists(rows[0].file));
    EXPECT_FALSE(fs::exists(rows[0].file + ".part"));
    EXPECT_EQ(rows[0].bytes, static_cast<std::int64_t>(fs::file_size(rows[0].file)));
    const ReadBack r = read_mp4(rows[0].file);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.first_is_key);
    // 预录 1 秒（从更早的关键帧开始，测试码流每 0.4 秒一个）+ 陌生人 2 秒 + 收尾 2 秒 ≈ 5～5.4 秒
    EXPECT_GE(r.duration_s, 4.9);
    EXPECT_LE(r.duration_s, 5.6);
    struct stat dst {};
    ASSERT_EQ(::stat((dir / "events").c_str(), &dst), 0);
    EXPECT_EQ(dst.st_mode & 0777U, 0700U);
}

TEST_F(RecordDir, StopWhileRecordingFinishesTheFile)
{
    auto store = EventStore::open(db, err);
    rksec::record::EventRecorder rec{recorder_options(dir), std::move(store)};
    ASSERT_TRUE(rec.start(err)) << err;
    std::int64_t t = 0;
    play(rec, 100, 30, 100, t);  // 陌生人一直在，程序就退出了
    EXPECT_TRUE(rec.recording());
    rec.stop();
    auto s2 = EventStore::open(db, err);
    std::vector<EventRow> rows;
    ASSERT_TRUE(s2->list(rows, err));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].status, "complete");
    EXPECT_TRUE(read_mp4(rows[0].file).ok);
}

TEST_F(RecordDir, LeftoverFromCrashIsRecoveredOnStart)
{
    // 模拟上次被 kill -9：事件表里一条 recording，目录里一个能读的 .part
    fs::create_directories(dir / "events");
    const std::string file = (dir / "events" / "x_1.mp4").string();
    {
        auto w = rksec::record::Mp4Writer::open(file + ".part", {64, 48, tiny_sps_pps()}, err);
        ASSERT_TRUE(w);
        for (const auto &f : tiny_frames()) {
            w->write(f, err);
        }
        ASSERT_TRUE(w->close(err));
    }
    std::int64_t id1 = 0;
    std::int64_t id2 = 0;
    {
        auto s = EventStore::open(db, err);
        ASSERT_TRUE(s->begin("stranger", "t1", "1", {}, id1, err));
        ASSERT_TRUE(s->set_file(id1, file, err));
        ASSERT_TRUE(s->begin("stranger", "t2", "2", {}, id2, err));  // 文件都没来得及建
    }
    auto store = EventStore::open(db, err);
    rksec::record::EventRecorder rec{recorder_options(dir), std::move(store)};
    ASSERT_TRUE(rec.start(err)) << err;
    rec.stop();
    EXPECT_EQ(rec.stats().recovered, 2U);
    auto s2 = EventStore::open(db, err);
    std::vector<EventRow> rows;
    ASSERT_TRUE(s2->list(rows, err));
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].id, id1);
    EXPECT_EQ(rows[1].status, "incomplete");
    EXPECT_GT(rows[1].bytes, 0);
    EXPECT_TRUE(fs::exists(file));
    EXPECT_FALSE(fs::exists(file + ".part"));
    EXPECT_EQ(rows[0].id, id2);
    EXPECT_EQ(rows[0].status, "lost");
}

TEST_F(RecordDir, QuotaDeletesOldestRecordings)
{
    auto store = EventStore::open(db, err);
    // 配额极小：每个录像都比它大，结束一个就把以前的全删掉（最新的那个也超了，但它是刚录完的——也会删）
    rksec::record::EventRecorder rec{recorder_options(dir, 1), std::move(store)};
    ASSERT_TRUE(rec.start(err)) << err;
    std::int64_t t = 0;
    play(rec, 150, 10, 20, t);  // 事件 1
    play(rec, 150, 10, 20, t);  // 事件 2
    rec.stop();
    const auto st = rec.stats();
    EXPECT_EQ(st.events, 2U);
    EXPECT_EQ(st.deleted, 2U);
    auto s2 = EventStore::open(db, err);
    std::vector<EventRow> rows;
    ASSERT_TRUE(s2->list(rows, err));
    ASSERT_EQ(rows.size(), 2U);
    for (const auto &r : rows) {
        EXPECT_EQ(r.status, "deleted");
        EXPECT_FALSE(fs::exists(r.file));
    }
}

TEST_F(RecordDir, LongEventIsSplitAtKeyframes)
{
    auto store = EventStore::open(db, err);
    auto o = recorder_options(dir);
    o.event.max_us = 3 * kSec;
    rksec::record::EventRecorder rec{o, std::move(store)};
    ASSERT_TRUE(rec.start(err)) << err;
    std::int64_t t = 0;
    play(rec, 300, 10, 250, t);  // 陌生人在约 10 秒
    rec.stop();
    auto s2 = EventStore::open(db, err);
    std::vector<EventRow> rows;
    ASSERT_TRUE(s2->list(rows, err));
    EXPECT_GE(rows.size(), 3U) << "10 秒的事件，每段最长 3 秒";
    EXPECT_EQ(rec.stats().events, 1U) << "还是同一个事件";
    for (const auto &r : rows) {
        EXPECT_EQ(r.status, "complete");
        const ReadBack rb = read_mp4(r.file);
        EXPECT_TRUE(rb.ok && rb.first_is_key) << r.file << "：每段都从关键帧开始";
    }
}

// ---------------------------------------------------------------- 导出、删除（第 8d 步）

namespace {

// MP4 顶层的盒子（box）类型，按文件里的顺序：分段 MP4 是 ftyp moov moof mdat moof mdat …，
// faststart 的普通 MP4 是 ftyp moov … mdat（moov 在 mdat 前面，播放器先拿到索引）
std::vector<std::string> top_boxes(const std::string &path)
{
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    std::uint64_t pos = 0;
    while (pos + 8 <= size) {
        std::array<unsigned char, 16> h{};
        f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char *>(h.data()), 16);
        std::uint64_t len =
            (std::uint64_t{h[0]} << 24) | (std::uint64_t{h[1]} << 16) | (std::uint64_t{h[2]} << 8) | h[3];
        if (len == 1) {  // 64 位长度
            len = 0;
            for (int i = 8; i < 16; ++i) {
                len = (len << 8) | h[static_cast<std::size_t>(i)];
            }
        } else if (len == 0) {
            len = size - pos;  // 一直到文件尾
        }
        out.emplace_back(reinterpret_cast<const char *>(h.data()) + 4, 4);
        if (len < 8) {
            break;
        }
        pos += len;
    }
    return out;
}

bool has_box(const std::vector<std::string> &boxes, const std::string &type)
{
    return std::find(boxes.begin(), boxes.end(), type) != boxes.end();
}

std::ptrdiff_t box_index(const std::vector<std::string> &boxes, const std::string &type)
{
    return std::find(boxes.begin(), boxes.end(), type) - boxes.begin();
}

// 用测试码流写一个分段 MP4（和录像器写的一样）
void write_fragmented(const std::string &path, std::size_t frames)
{
    std::string err;
    auto w = rksec::record::Mp4Writer::open(path, {64, 48, tiny_sps_pps()}, err);
    ASSERT_TRUE(w) << err;
    const auto all = tiny_frames();
    for (std::size_t i = 0; i < frames && i < all.size(); ++i) {
        ASSERT_TRUE(w->write(all[i], err)) << err;
    }
    ASSERT_TRUE(w->close(err)) << err;
}

unsigned file_mode(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 0777U) : 0U;
}

const std::vector<std::uint8_t> kFakeJpeg{0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};

// 事件表里加一个录完的事件，录像文件真的写出来
std::int64_t add_event(EventStore &store, const fs::path &dir, const std::string &status, bool snapshot = true)
{
    std::string err;
    std::int64_t id = 0;
    EXPECT_TRUE(store.begin("stranger", "2026-11-05 10:00:00", "3", snapshot ? kFakeJpeg : std::vector<std::uint8_t>{},
                            id, err))
        << err;
    const std::string file = (dir / ("20261105_100000_" + std::to_string(id) + ".mp4")).string();
    EXPECT_TRUE(store.set_file(id, file, err)) << err;
    if (status != "recording") {
        write_fragmented(file, 30);
        EXPECT_TRUE(store.finish(id, "2026-11-05 10:00:01", 1200, static_cast<std::int64_t>(fs::file_size(file)), "3,5",
                                 status, err))
            << err;
    }
    return id;
}

}  // namespace

TEST_F(RecordDir, RemuxMakesAPlainMp4WithTheIndexFirst)
{
    const std::string in = (dir / "frag.mp4").string();
    write_fragmented(in, 30);
    const auto before = top_boxes(in);
    EXPECT_TRUE(has_box(before, "moof")) << "录像是分段 MP4";

    const std::string out = (dir / "plain.mp4").string();
    rksec::record::RemuxInfo info;
    ASSERT_TRUE(rksec::record::remux_to_faststart(in, out, info, err)) << err;
    const auto after = top_boxes(out);
    EXPECT_FALSE(has_box(after, "moof")) << "导出的是普通 MP4，不分段";
    ASSERT_TRUE(has_box(after, "moov") && has_box(after, "mdat"));
    EXPECT_LT(box_index(after, "moov"), box_index(after, "mdat")) << "faststart：索引在画面数据前面";
    EXPECT_FALSE(fs::exists(out + ".part"));
    EXPECT_EQ(file_mode(out), 0600U);

    EXPECT_EQ(info.packets, 30);
    EXPECT_EQ(info.keyframes, 3);
    EXPECT_EQ(info.width, 64);
    EXPECT_EQ(info.height, 48);
    EXPECT_FALSE(info.truncated);
    EXPECT_NEAR(static_cast<double>(info.duration_us) / 1e6, 1.2, 0.05);

    const ReadBack r = read_mp4(out);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.packets, 30) << "一帧不少";
    EXPECT_EQ(r.keyframes, 3);
    EXPECT_TRUE(r.first_is_key);
    EXPECT_NEAR(r.duration_s, 1.2, 0.05);
}

TEST_F(RecordDir, RemuxNeverOverwrites)
{
    const std::string in = (dir / "frag.mp4").string();
    write_fragmented(in, 30);
    const std::string out = (dir / "exists.mp4").string();
    std::ofstream(out) << "keep";
    rksec::record::RemuxInfo info;
    EXPECT_FALSE(rksec::record::remux_to_faststart(in, out, info, err));
    EXPECT_NE(err.find("已经存在"), std::string::npos) << err;
    EXPECT_EQ(fs::file_size(out), 4U) << "原来的文件没被动";

    EXPECT_FALSE(rksec::record::remux_to_faststart((dir / "none.mp4").string(), (dir / "x.mp4").string(), info, err));
    EXPECT_FALSE(fs::exists(dir / "x.mp4"));
    EXPECT_FALSE(fs::exists(dir / "x.mp4.part")) << "失败时不留半个文件";
}

TEST_F(RecordDir, RemuxOfAnInterruptedRecordingKeepsTheFinishedPart)
{
    // 和 Mp4IsReadableAfterTheProcessIsKilled 一样：子进程写 25 帧后直接 _exit
    const std::string in = (dir / "killed.mp4").string();
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        std::string e;
        auto w = rksec::record::Mp4Writer::open(in, {64, 48, tiny_sps_pps()}, e);
        const auto frames = tiny_frames();
        for (std::size_t i = 0; w && i < 25; ++i) {
            w->write(frames[i], e);
        }
        ::_exit(w ? 0 : 1);  // NOLINT：模拟进程被杀
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    const std::string out = (dir / "killed_plain.mp4").string();
    rksec::record::RemuxInfo info;
    ASSERT_TRUE(rksec::record::remux_to_faststart(in, out, info, err)) << err;
    EXPECT_EQ(info.packets, 20) << "写完的两个 GOP";
    const ReadBack r = read_mp4(out);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.packets, 20);
}

TEST_F(RecordDir, ExportWritesVideoAndSnapshot)
{
    auto store = EventStore::open(db, err);
    ASSERT_TRUE(store) << err;
    const auto events = dir / "events";
    fs::create_directories(events);
    const std::int64_t id = add_event(*store, events, "complete");

    const std::string out_dir = (dir / "out" / "sub").string();  // 不存在：自动建
    rksec::record::ExportResult res;
    ASSERT_TRUE(rksec::record::export_event(*store, id, out_dir, res, err)) << err;
    const std::string stem = "20261105_100000_" + std::to_string(id);
    EXPECT_EQ(res.video, (fs::path{out_dir} / (stem + ".mp4")).string());
    EXPECT_EQ(res.snapshot, (fs::path{out_dir} / (stem + ".jpg")).string());
    EXPECT_FALSE(res.incomplete);
    EXPECT_EQ(res.info.packets, 30);
    EXPECT_GT(res.video_bytes, 0);
    EXPECT_EQ(file_mode(res.video), 0600U);
    EXPECT_EQ(file_mode(res.snapshot), 0600U);
    EXPECT_EQ(file_mode(out_dir), 0700U) << "新建的导出目录只有自己能进";
    EXPECT_FALSE(has_box(top_boxes(res.video), "moof"));

    std::ifstream jf(res.snapshot, std::ios::binary);
    const std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(jf), {}};
    EXPECT_EQ(jpeg, kFakeJpeg) << "截图原样导出";

    rksec::record::ExportResult again;
    EXPECT_FALSE(rksec::record::export_event(*store, id, out_dir, again, err)) << "同名文件已经有了：不覆盖";
    EXPECT_NE(err.find("已经存在"), std::string::npos) << err;

    EventRow row;
    bool found = false;
    ASSERT_TRUE(store->get(id, row, found, err));
    EXPECT_EQ(row.status, "complete") << "导出不改事件";
}

TEST_F(RecordDir, ExportWithoutSnapshotAndIncomplete)
{
    auto store = EventStore::open(db, err);
    ASSERT_TRUE(store) << err;
    const std::int64_t id = add_event(*store, dir, "incomplete", false);
    rksec::record::ExportResult res;
    ASSERT_TRUE(rksec::record::export_event(*store, id, (dir / "out").string(), res, err)) << err;
    EXPECT_TRUE(res.incomplete) << "不完整的也能导出，但要提示";
    EXPECT_TRUE(res.snapshot.empty());
    EXPECT_TRUE(fs::exists(res.video));
}

TEST_F(RecordDir, ExportRefusesWhatCannotBeExported)
{
    auto store = EventStore::open(db, err);
    ASSERT_TRUE(store) << err;
    const std::string out = (dir / "out").string();
    rksec::record::ExportResult res;

    EXPECT_FALSE(rksec::record::export_event(*store, 999, out, res, err));
    EXPECT_NE(err.find("没有事件 #999"), std::string::npos) << err;

    const std::int64_t rec = add_event(*store, dir, "recording");
    EXPECT_FALSE(rksec::record::export_event(*store, rec, out, res, err));
    EXPECT_NE(err.find("还在录"), std::string::npos) << err;

    const std::int64_t del = add_event(*store, dir, "complete");
    EventRow row;
    ASSERT_TRUE(rksec::record::delete_event(*store, del, row, err)) << err;
    EXPECT_FALSE(rksec::record::export_event(*store, del, out, res, err));
    EXPECT_NE(err.find("已经删除"), std::string::npos) << err;

    const std::int64_t lost = add_event(*store, dir, "complete");
    ASSERT_TRUE(store->mark(lost, "lost", err));
    EXPECT_FALSE(rksec::record::export_event(*store, lost, out, res, err));
    EXPECT_NE(err.find("丢失"), std::string::npos) << err;

    const std::int64_t gone = add_event(*store, dir, "complete");
    bool found = false;
    ASSERT_TRUE(store->get(gone, row, found, err));
    fs::remove(row.file);  // 记录说完整，文件却被人手动删了
    EXPECT_FALSE(rksec::record::export_event(*store, gone, out, res, err));
    EXPECT_NE(err.find("文件不在了"), std::string::npos) << err;

    const std::int64_t ok = add_event(*store, dir, "complete");
    const std::string not_dir = (dir / "plain_file").string();
    std::ofstream(not_dir) << "x";
    EXPECT_FALSE(rksec::record::export_event(*store, ok, not_dir, res, err));
    EXPECT_NE(err.find("不是目录"), std::string::npos) << err;

    if (::geteuid() != 0) {  // root 不受权限限制，测不了
        const auto ro = dir / "readonly";
        fs::create_directories(ro);
        ::chmod(ro.c_str(), S_IRUSR | S_IXUSR);
        EXPECT_FALSE(rksec::record::export_event(*store, ok, ro.string(), res, err)) << "目录不能写";
        ::chmod(ro.c_str(), S_IRWXU);
        EXPECT_TRUE(fs::is_empty(ro));
    }
}

TEST_F(RecordDir, DeleteRemovesVideoAndSnapshotButKeepsTheRecord)
{
    auto store = EventStore::open(db, err);
    ASSERT_TRUE(store) << err;
    const std::int64_t id = add_event(*store, dir, "complete");
    EventRow row;
    ASSERT_TRUE(rksec::record::delete_event(*store, id, row, err)) << err;
    EXPECT_EQ(row.status, "complete") << "返回删除前的记录";
    EXPECT_FALSE(fs::exists(row.file));

    EventRow now;
    bool found = false;
    ASSERT_TRUE(store->get(id, now, found, err));
    ASSERT_TRUE(found) << "记录留着";
    EXPECT_EQ(now.status, "deleted");
    EXPECT_FALSE(now.has_snapshot) << "截图里有人：一起清掉";
    EXPECT_EQ(now.tracks, "3,5");
    std::vector<rksec::record::EventRow> clips;
    ASSERT_TRUE(store->clips(clips, err));
    EXPECT_TRUE(clips.empty()) << "不再算进配额";

    EXPECT_FALSE(rksec::record::delete_event(*store, id, row, err));
    EXPECT_NE(err.find("已经删过"), std::string::npos) << err;
    EXPECT_FALSE(rksec::record::delete_event(*store, 999, row, err));

    const std::int64_t rec = add_event(*store, dir, "recording");
    EXPECT_FALSE(rksec::record::delete_event(*store, rec, row, err)) << "正在录的不能删";
    EXPECT_NE(err.find("还在录"), std::string::npos) << err;

    const std::int64_t lost = add_event(*store, dir, "complete");
    ASSERT_TRUE(store->get(lost, now, found, err));
    fs::remove(now.file);
    ASSERT_TRUE(store->mark(lost, "lost", err));
    EXPECT_TRUE(rksec::record::delete_event(*store, lost, row, err)) << "文件已经不在也能删（清掉截图）：" << err;
    ASSERT_TRUE(store->get(lost, now, found, err));
    EXPECT_EQ(now.status, "deleted");
    EXPECT_FALSE(now.has_snapshot);
}

TEST(EventLine, ShowsWhatTheListNeeds)
{
    EventRow r;
    r.id = 12;
    r.kind = "stranger";
    r.started = "2026-11-04 22:00:00";
    r.duration_ms = 23400;
    r.bytes = 11 * 1024 * 1024;
    r.status = "complete";
    r.tracks = "3,5";
    r.has_snapshot = true;
    r.file = "/var/lib/rk3588-security/events/20261104_220000_12.mp4";
    const std::string s = rksec::record::event_line(r);
    for (const char *part : {"#12", "2026-11-04 22:00:00", "23.4 秒", "11.0 MB", "完整", "有截图", "陌生人 3,5",
                             "20261104_220000_12.mp4"}) {
        EXPECT_NE(s.find(part), std::string::npos) << "缺少 " << part << "：" << s;
    }
    EXPECT_EQ(s.find("/var/lib"), std::string::npos) << "只显示文件名";

    r.status = "deleted";
    r.has_snapshot = false;
    r.kind = "stranger（续）";
    const std::string d = rksec::record::event_line(r);
    EXPECT_NE(d.find("已删除"), std::string::npos) << d;
    EXPECT_NE(d.find("无截图"), std::string::npos) << d;
    EXPECT_NE(d.find("续录"), std::string::npos) << d;
    EXPECT_EQ(d.find(".mp4"), std::string::npos) << "删了的不显示文件名：" << d;
    EXPECT_STREQ(rksec::record::status_text("bogus"), "未知");
}
