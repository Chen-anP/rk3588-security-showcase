#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 事件表（第 8b 步）：每次录像一条记录，和人脸库在同一个 SQLite 文件里（表结构见 face/face_db.hpp，
// 版本 2）。纯 CPU，CI 上测。
//
// 一条事件的一生：
//   begin()：开始录，status = recording（先写记录、后建文件：程序中途被杀，启动时能找到它）
//   set_file()：确定文件名（文件名里有事件编号，所以要先有记录）
//   finish()：录完，写结束时间、时长、大小，status = complete / incomplete
//   mark()：超配额删了文件、或者用 --event-delete 手动删 → deleted（记录留着、截图清掉：
//           回放时知道“这里有过一个事件”，但人的画面不留）
//
// 线程：同一个对象只在一个线程里用（EventRecorder 的写盘线程）。别的进程同时读（--events）没问题。
namespace rksec::record {

struct EventRow {
    std::int64_t id = 0;
    std::string kind;  // 现在只有 "stranger"
    std::string started;
    std::string ended;
    std::int64_t duration_ms = 0;
    std::string tracks;  // 跟踪编号，逗号分隔
    std::string file;
    std::int64_t bytes = 0;
    std::string status;
    bool has_snapshot = false;
};

class EventStore {
  public:
    static std::unique_ptr<EventStore> open(const std::string &path, std::string &err);
    ~EventStore();
    EventStore(const EventStore &) = delete;
    EventStore &operator=(const EventStore &) = delete;

    bool begin(const std::string &kind, const std::string &started, const std::string &tracks,
               const std::vector<std::uint8_t> &snapshot, std::int64_t &id, std::string &err);
    bool set_file(std::int64_t id, const std::string &file, std::string &err);
    // tracks：整个事件里出现过的陌生人编号（开始时只知道第一批）
    bool finish(std::int64_t id, const std::string &ended, std::int64_t duration_ms, std::int64_t bytes,
                const std::string &tracks, const std::string &status, std::string &err);
    // 只改状态；改成 deleted 时同时清掉截图
    bool mark(std::int64_t id, const std::string &status, std::string &err);
    // 补上大小（启动时把上次没录完的文件改名后用）
    bool set_bytes(std::int64_t id, std::int64_t bytes, std::string &err);

    // 最新的 limit 条（新的在前）；status 非空时只要这个状态的
    bool list(std::vector<EventRow> &out, std::string &err, int limit = 100, const std::string &status = "");
    // 文件还在的（complete、incomplete），从旧到新：算配额用
    bool clips(std::vector<EventRow> &out, std::string &err);
    // 按编号取一条。没有这条：返回 true、found = false
    bool get(std::int64_t id, EventRow &out, bool &found, std::string &err);
    bool snapshot(std::int64_t id, std::vector<std::uint8_t> &out, std::string &err);

    struct Impl;

  private:
    EventStore();
    std::unique_ptr<Impl> impl_;
};

}  // namespace rksec::record
