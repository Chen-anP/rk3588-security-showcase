#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/bounded_queue.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/video_encoder.hpp"
#include "record/event_logic.hpp"
#include "record/event_store.hpp"
#include "record/mp4_writer.hpp"
#include "record/pre_record.hpp"

// 事件录像（第 8b 步）：把预录缓冲、事件状态机、MP4 写入、事件表、配额串起来。
//
//   编码线程 ──on_packet()──▶ 预录缓冲；录像中时同时放进写盘队列
//   主线程 ──update(陌生人编号, 现在, 预览图)──▶ 状态机：开始 / 结束 / 切段 → 放进写盘队列
//   写盘线程：开始 → 事件表加一条（recording）→ 建 xxx.mp4.part → 写预录的包 → 写之后的包
//            结束 → 关文件、fsync → 改名成 xxx.mp4 → fsync 目录 → 事件表改成 complete → 检查配额
//
// - 磁盘读写全在写盘线程：磁盘慢了不拖慢编码、检测、界面
// - 写盘队列很大（4096 条，约 2.5 分钟的包）：满了说明磁盘严重跟不上，丢最旧的并计数、写日志
// - 程序被杀掉、断电：重启时 start() 找到状态还是 recording 的事件，把 .part 改名保留、标成 incomplete
//   （分段 MP4，写完的部分能播）；文件不在了标 lost
// - 配额：start() 时和每个事件结束时，按总大小从最旧的删，记录标 deleted（截图一起清掉）
// - 录像目录权限 0700、文件 0600：录像里有人，只有运行程序的用户能看
namespace rksec::record {

struct RecorderOptions {
    std::string dir;                  // 录像目录（没有就建）
    std::int64_t pre_us = 5'000'000;  // 预录
    EventOptions event;               // 收尾、切段
    std::int64_t quota_bytes = std::int64_t{2048} * 1024 * 1024;
    Mp4Writer::Options video;  // 宽高、SPS/PPS
};

class EventRecorder {
  public:
    EventRecorder(RecorderOptions opts, std::unique_ptr<EventStore> store);
    ~EventRecorder();
    EventRecorder(const EventRecorder &) = delete;
    EventRecorder &operator=(const EventRecorder &) = delete;

    // 建目录、恢复上次没录完的、检查配额、起写盘线程
    bool start(std::string &err);
    // 正在录的事件就地结束（complete），写盘队列写完，线程退出。可以重复调用
    void stop();

    // 编码线程：每个编好的包
    void on_packet(const media::EncodedPacket &p);
    // 主线程：每个新的检测结果（strangers：这一帧里陌生人的跟踪编号，空 = 没有陌生人）；
    // 没有新结果时也要定时调用（strangers 传空），否则检测停了事件永远不结束。
    // preview：现在的预览图（XRGB），事件开始时存成截图；可以是 nullptr
    void update(const std::vector<int> &strangers, std::int64_t now_us, const media::Image *preview);

    bool recording() const;

    struct Stats {
        std::uint64_t events = 0;   // 开始过几个事件
        std::uint64_t files = 0;    // 写完几个文件（切段的话一个事件多个文件）
        std::uint64_t packets = 0;  // 写进文件的包
        std::uint64_t skipped = 0;  // 文件开头不是关键帧被丢掉的包
        std::uint64_t write_errors = 0;
        std::uint64_t queue_dropped = 0;  // 写盘队列满了丢掉的
        std::uint64_t recovered = 0;      // 启动时恢复的上次没录完的
        std::uint64_t deleted = 0;        // 超配额删掉的
        std::string last_file;
    };
    Stats stats() const;

  private:
    struct Cmd {
        enum class Kind : std::uint8_t { begin, packet, end } kind = Kind::packet;
        std::vector<PacketPtr> packets;
        std::string wall;    // begin / end：本地时间
        std::string tracks;  // begin / end：陌生人编号
        std::shared_ptr<media::Image> snapshot;
        bool continued = false;  // begin：切段后的续录
    };

    void push(Cmd c);
    void writer_loop();
    void begin_file(Cmd &c);
    void write_packet(const media::EncodedPacket &p);
    void end_file(const std::string &wall, const std::string &tracks);
    void recover();
    void enforce_quota();
    std::string tracks_text() const;  // 调用方持有 mutex_

    RecorderOptions opts_;
    std::unique_ptr<EventStore> store_;  // 只在写盘线程里用（start() 时线程还没起）

    mutable std::mutex mutex_;  // 保护下面这些（编码线程和主线程都会碰）
    PreRecordBuffer buffer_;
    EventTracker tracker_;
    bool recording_ = false;
    bool split_pending_ = false;
    std::set<int> tracks_;  // 这个事件里出现过的陌生人
    Stats counts_;

    BoundedQueue<Cmd> queue_{4096};
    std::thread thread_;
    bool started_ = false;
    bool stopped_ = false;

    // 写盘线程自己的状态
    std::unique_ptr<Mp4Writer> writer_;
    std::int64_t event_id_ = -1;
    std::string file_;  // 最终文件名（写的时候是 file_ + ".part"）
};

// 现在的本地时间：“2026-11-04 22:00:00”；compact = true 时“20261104_220000”（文件名用）
std::string local_time(bool compact);

// XRGB（B G R X）预览图 → JPEG。给截图用
bool xrgb_to_jpeg(const media::Image &img, int quality, std::vector<std::uint8_t> &out, std::string &err);

}  // namespace rksec::record
