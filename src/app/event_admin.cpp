#include "app/event_admin.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/log.hpp"
#include "record/event_admin.hpp"
#include "record/event_store.hpp"

namespace rksec::app {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRejected = 1;
constexpr int kExitDevice = 4;

constexpr double kMiB = 1024.0 * 1024.0;

// 数据库不存在时不新建（--events 不该顺手建出一个空库），返回 nullptr、missing = true
std::unique_ptr<record::EventStore> open_store(const config::AppConfig &cfg, bool &missing)
{
    std::error_code ec;
    missing = !std::filesystem::exists(cfg.face.database, ec);
    if (missing) {
        return nullptr;
    }
    std::string err;
    auto store = record::EventStore::open(cfg.face.database, err);
    if (!store) {
        LOG_E("record", "打不开数据库 %s：%s", cfg.face.database.c_str(), err.c_str());
    }
    return store;
}

}  // namespace

int event_list(const config::AppConfig &cfg, int limit)
{
    bool missing = false;
    auto store = open_store(cfg, missing);
    if (missing) {
        std::printf("数据库 %s 还不存在：还没有录过事件\n", cfg.face.database.c_str());
        return kExitOk;
    }
    if (!store) {
        return kExitDevice;
    }
    std::vector<record::EventRow> rows;
    std::vector<record::EventRow> clips;
    std::string err;
    if (!store->list(rows, err, limit) || !store->clips(clips, err)) {
        LOG_E("record", "%s", err.c_str());
        return kExitDevice;
    }
    if (rows.empty()) {
        std::printf("还没有事件（录像目录 %s）\n", cfg.record.dir.c_str());
    } else {
        std::printf("事件（最新的 %zu 条，新的在前；录像目录 %s）\n", rows.size(), cfg.record.dir.c_str());
    }
    for (const auto &r : rows) {
        std::printf("  %s\n", record::event_line(r).c_str());
    }
    std::int64_t total = 0;
    for (const auto &c : clips) {
        total += c.bytes;
    }
    const auto quota = static_cast<double>(cfg.record.quota_mb);
    std::printf("录像 %zu 个，共 %.1f MB，配额 %.0f MB（用了 %.0f%%）\n", clips.size(),
                static_cast<double>(total) / kMiB, quota, static_cast<double>(total) / kMiB / quota * 100.0);
    return kExitOk;
}

int event_export(const config::AppConfig &cfg, std::int64_t id, const std::string &dir)
{
    bool missing = false;
    auto store = open_store(cfg, missing);
    if (missing) {
        std::fprintf(stderr, "数据库 %s 还不存在：还没有录过事件\n", cfg.face.database.c_str());
        return kExitRejected;
    }
    if (!store) {
        return kExitDevice;
    }
    record::ExportResult res;
    std::string err;
    if (!record::export_event(*store, id, dir, res, err)) {
        std::fprintf(stderr, "导出失败：%s\n", err.c_str());
        return kExitRejected;
    }
    std::printf("录像 %s\n", res.video.c_str());
    std::printf("     %d×%d，%lld 帧（关键帧 %lld），%.1f 秒，%.1f MB，普通 MP4（索引在开头，任何播放器都能拖动）\n",
                res.info.width, res.info.height, static_cast<long long>(res.info.packets),
                static_cast<long long>(res.info.keyframes), static_cast<double>(res.info.duration_us) / 1e6,
                static_cast<double>(res.video_bytes) / kMiB);
    if (!res.snapshot.empty()) {
        std::printf("截图 %s\n", res.snapshot.c_str());
    } else {
        std::printf("这个事件没有截图\n");
    }
    if (res.incomplete || res.info.truncated) {
        std::printf("注意：这段录像不完整（录的时候程序被中断），导出的是中断前能播放的部分\n");
    }
    std::printf("文件里有人，权限是 600。拷到别处以后注意保管，用完删掉\n");
    return kExitOk;
}

int event_delete(const config::AppConfig &cfg, std::int64_t id)
{
    bool missing = false;
    auto store = open_store(cfg, missing);
    if (missing) {
        std::fprintf(stderr, "数据库 %s 还不存在：还没有录过事件\n", cfg.face.database.c_str());
        return kExitRejected;
    }
    if (!store) {
        return kExitDevice;
    }
    record::EventRow row;
    std::string err;
    if (!record::delete_event(*store, id, row, err)) {
        std::fprintf(stderr, "删除失败：%s\n", err.c_str());
        return kExitRejected;
    }
    std::printf("已删除事件 #%lld（%s）：录像和截图删掉了，记录保留（时间、陌生人编号）\n", static_cast<long long>(id),
                row.started.c_str());
    return kExitOk;
}

}  // namespace rksec::app
