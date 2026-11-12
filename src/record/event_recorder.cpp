#include "record/event_recorder.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <ctime>
#include <filesystem>
#include <utility>

#include "core/log.hpp"
#include "media/jpeg_encoder.hpp"
#include "record/retention.hpp"

namespace rksec::record {

namespace {

constexpr int kSnapshotQuality = 85;
constexpr std::uint64_t kLogFirst = 5;
constexpr const char *kKind = "stranger";

namespace fs = std::filesystem;

void fsync_dir(const std::string &dir)
{
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

std::int64_t file_size(const std::string &path)
{
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(n);
}

}  // namespace

std::string local_time(bool compact)
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::array<char, 64> buf{};
    // 格式写成字面量：编译器能检查（-Wformat-nonliteral）
    const std::size_t n = compact ? std::strftime(buf.data(), buf.size(), "%Y%m%d_%H%M%S", &tm)
                                  : std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S", &tm);
    return {buf.data(), n};
}

bool xrgb_to_jpeg(const media::Image &img, int quality, std::vector<std::uint8_t> &out, std::string &err)
{
    if (img.width <= 0 || img.height <= 0 || img.pixels.size() < img.stride * static_cast<std::size_t>(img.height)) {
        err = "截图：预览图是空的";
        return false;
    }
    const auto w = static_cast<std::size_t>(img.width);
    std::vector<std::uint8_t> rgb(w * static_cast<std::size_t>(img.height) * 3);
    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t *src = img.pixels.data() + static_cast<std::size_t>(y) * img.stride;
        std::uint8_t *dst = rgb.data() + static_cast<std::size_t>(y) * w * 3;
        for (std::size_t x = 0; x < w; ++x) {
            dst[x * 3] = src[x * 4 + 2];  // XRGB8888 在内存里是 B G R X
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4];
        }
    }
    return media::encode_rgb_jpeg(rgb.data(), img.width, img.height, quality, out, err);
}

EventRecorder::EventRecorder(RecorderOptions opts, std::unique_ptr<EventStore> store)
    : opts_(std::move(opts)), store_(std::move(store)), buffer_(opts_.pre_us), tracker_(opts_.event)
{
}

EventRecorder::~EventRecorder()
{
    stop();
}

bool EventRecorder::start(std::string &err)
{
    if (started_) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(opts_.dir, ec);
    if (ec || !fs::is_directory(opts_.dir)) {
        err = "录像目录 " + opts_.dir + " 建不了：" + ec.message();
        return false;
    }
    ::chmod(opts_.dir.c_str(), S_IRWXU);  // 录像里有人：目录只有自己能进
    recover();
    enforce_quota();
    started_ = true;
    thread_ = std::thread([this] { writer_loop(); });
    return true;
}

void EventRecorder::stop()
{
    if (!started_ || stopped_) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (recording_) {
            Cmd c;
            c.kind = Cmd::Kind::end;
            c.wall = local_time(false);
            c.tracks = tracks_text();
            queue_.push(std::move(c));
            recording_ = false;
        }
    }
    stopped_ = true;
    queue_.close();
    thread_.join();
}

std::string EventRecorder::tracks_text() const
{
    std::string s;
    for (const int t : tracks_) {
        s += (s.empty() ? "" : ",") + std::to_string(t);
    }
    return s;
}

void EventRecorder::push(Cmd c)
{
    if (queue_.push(std::move(c)) == PushResult::dropped_oldest && counts_.queue_dropped++ < kLogFirst) {
        LOG_E("record", "写盘跟不上，写盘队列满了，丢掉了最旧的一条（磁盘满了或者太慢？）");
    }
}

void EventRecorder::on_packet(const media::EncodedPacket &p)
{
    auto shared = std::make_shared<const media::EncodedPacket>(p);
    const std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push(shared);
    if (!recording_) {
        return;
    }
    if (split_pending_ && shared->keyframe) {
        // 切段：在关键帧上断开，新文件从这个关键帧开始
        split_pending_ = false;
        const std::string wall = local_time(false);
        Cmd end;
        end.kind = Cmd::Kind::end;
        end.wall = wall;
        end.tracks = tracks_text();
        push(std::move(end));
        Cmd begin;
        begin.kind = Cmd::Kind::begin;
        begin.wall = wall;
        begin.tracks = tracks_text();
        begin.continued = true;
        begin.packets.push_back(std::move(shared));
        push(std::move(begin));
        return;
    }
    Cmd c;
    c.kind = Cmd::Kind::packet;
    c.packets.push_back(std::move(shared));
    push(std::move(c));
}

void EventRecorder::update(const std::vector<int> &strangers, std::int64_t now_us, const media::Image *preview)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const EventAction a = tracker_.update(!strangers.empty(), now_us);
    if (tracker_.active()) {
        tracks_.insert(strangers.begin(), strangers.end());
    }
    switch (a) {
    case EventAction::start: {
        recording_ = true;
        split_pending_ = false;
        ++counts_.events;
        Cmd c;
        c.kind = Cmd::Kind::begin;
        c.wall = local_time(false);
        c.tracks = tracks_text();
        c.packets = buffer_.snapshot();  // 预录：从关键帧开始的最近几秒
        if (preview != nullptr && preview->width > 0) {
            c.snapshot = std::make_shared<media::Image>(*preview);  // 拷一份，JPEG 在写盘线程里压
        }
        LOG_I("record", "陌生人出现（编号 %s），开始录像，预录 %.1f 秒", c.tracks.c_str(),
              static_cast<double>(buffer_.span_us()) / 1e6);
        push(std::move(c));
        break;
    }
    case EventAction::stop: {
        Cmd c;
        c.kind = Cmd::Kind::end;
        c.wall = local_time(false);
        c.tracks = tracks_text();
        LOG_I("record", "陌生人离开，结束录像（编号 %s）", c.tracks.c_str());
        push(std::move(c));
        recording_ = false;
        split_pending_ = false;
        tracks_.clear();
        break;
    }
    case EventAction::split:
        split_pending_ = true;  // 等下一个关键帧（on_packet 里切）
        break;
    case EventAction::none:
        break;
    }
}

bool EventRecorder::recording() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

EventRecorder::Stats EventRecorder::stats() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

// ---------------------------------------------------------------- 写盘线程

void EventRecorder::writer_loop()
{
    pthread_setname_np(pthread_self(), "record");
    while (auto c = queue_.pop()) {
        switch (c->kind) {
        case Cmd::Kind::begin:
            begin_file(*c);
            break;
        case Cmd::Kind::packet:
            for (const auto &p : c->packets) {
                write_packet(*p);
            }
            break;
        case Cmd::Kind::end:
            end_file(c->wall, c->tracks);
            break;
        }
    }
    if (writer_) {
        end_file(local_time(false), "");  // 队列关了还有没结束的（不应该发生，防御）
    }
}

void EventRecorder::begin_file(Cmd &c)
{
    if (writer_) {
        end_file(c.wall, c.tracks);
    }
    std::string err;
    std::vector<std::uint8_t> jpeg;
    if (c.snapshot && !xrgb_to_jpeg(*c.snapshot, kSnapshotQuality, jpeg, err)) {
        LOG_W("record", "%s", err.c_str());
    }
    if (!store_->begin(c.continued ? std::string{kKind} + "（续）" : kKind, c.wall, c.tracks, jpeg, event_id_, err)) {
        LOG_E("record", "事件表写不进去：%s；这个事件不录", err.c_str());
        event_id_ = -1;
        const std::lock_guard<std::mutex> lock(mutex_);
        ++counts_.write_errors;
        return;
    }
    file_ = (fs::path{opts_.dir} / (local_time(true) + "_" + std::to_string(event_id_) + ".mp4")).string();
    store_->set_file(event_id_, file_, err);
    writer_ = Mp4Writer::open(file_ + ".part", opts_.video, err);
    if (!writer_) {
        LOG_E("record", "%s", err.c_str());
        store_->finish(event_id_, c.wall, 0, 0, c.tracks, "lost", err);
        event_id_ = -1;
        const std::lock_guard<std::mutex> lock(mutex_);
        ++counts_.write_errors;
        return;
    }
    for (const auto &p : c.packets) {
        write_packet(*p);
    }
}

void EventRecorder::write_packet(const media::EncodedPacket &p)
{
    if (!writer_) {
        return;  // 这个事件开始时就失败了
    }
    std::string err;
    const bool ok = writer_->write(p, err);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ok && counts_.write_errors++ < kLogFirst) {
        LOG_E("record", "%s", err.c_str());
    }
}

void EventRecorder::end_file(const std::string &wall, const std::string &tracks)
{
    if (!writer_) {
        return;
    }
    std::string err;
    bool ok = writer_->close(err);
    if (!ok) {
        LOG_E("record", "%s", err.c_str());
    }
    const std::int64_t duration_ms = writer_->duration_us() / 1000;
    const std::uint64_t packets = writer_->packets();
    const std::uint64_t skipped = writer_->skipped();
    writer_.reset();

    std::error_code ec;
    fs::rename(file_ + ".part", file_, ec);
    if (ec) {
        LOG_E("record", "改名 %s.part 失败：%s", file_.c_str(), ec.message().c_str());
        ok = false;
    }
    fsync_dir(opts_.dir);  // 改名本身也要落盘
    const std::int64_t bytes = file_size(ec ? file_ + ".part" : file_);
    if (!store_->finish(event_id_, wall, duration_ms, bytes, tracks, ok ? "complete" : "incomplete", err)) {
        LOG_E("record", "事件表更新失败：%s", err.c_str());
    }
    LOG_I("record", "录像 %s：%.1f 秒，%.1f MB，%llu 帧", file_.c_str(), static_cast<double>(duration_ms) / 1000.0,
          static_cast<double>(bytes) / 1048576.0, static_cast<unsigned long long>(packets));
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++counts_.files;
        counts_.packets += packets;
        counts_.skipped += skipped;
        counts_.last_file = file_;
    }
    event_id_ = -1;
    enforce_quota();
}

// ---------------------------------------------------------------- 启动时

void EventRecorder::recover()
{
    std::string err;
    std::vector<EventRow> rows;
    if (!store_->list(rows, err, 1000, "recording")) {
        LOG_W("record", "查上次没录完的事件失败：%s", err.c_str());
        return;
    }
    for (const auto &r : rows) {
        std::error_code ec;
        const std::string part = r.file + ".part";
        std::string status = "lost";
        if (!r.file.empty() && fs::exists(part, ec)) {
            fs::rename(part, r.file, ec);
            status = ec ? "lost" : "incomplete";
        } else if (!r.file.empty() && fs::exists(r.file, ec)) {
            status = "incomplete";  // 改了名但没来得及写数据库：内容是完整的，保守标成不完整
        }
        if (status == "incomplete") {
            store_->set_bytes(r.id, file_size(r.file), err);
        }
        store_->mark(r.id, status, err);
        LOG_W("record", "上次没录完的事件 %lld（%s）：%s", static_cast<long long>(r.id), r.started.c_str(),
              status == "incomplete" ? "文件已保留，能播放到中断的地方" : "文件找不到了");
        ++counts_.recovered;
    }
    if (!rows.empty()) {
        fsync_dir(opts_.dir);
    }
}

void EventRecorder::enforce_quota()
{
    std::string err;
    std::vector<EventRow> rows;
    if (!store_->clips(rows, err)) {
        LOG_W("record", "查录像占用失败：%s", err.c_str());
        return;
    }
    std::vector<StoredClip> clips;
    clips.reserve(rows.size());
    for (const auto &r : rows) {
        clips.push_back({r.id, r.bytes});
    }
    const auto victims = plan_retention(clips, opts_.quota_bytes, event_id_);
    for (const std::int64_t id : victims) {
        for (const auto &r : rows) {
            if (r.id != id) {
                continue;
            }
            std::error_code ec;
            fs::remove(r.file, ec);
            store_->mark(id, "deleted", err);
            LOG_I("record", "录像超过配额（%lld MB），删掉最旧的 %s", static_cast<long long>(opts_.quota_bytes >> 20),
                  r.file.c_str());
        }
    }
    if (!victims.empty()) {
        fsync_dir(opts_.dir);
        const std::lock_guard<std::mutex> lock(mutex_);
        counts_.deleted += victims.size();
    }
}

}  // namespace rksec::record
