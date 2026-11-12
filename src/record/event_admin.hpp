#pragma once

#include <cstdint>
#include <string>

#include "record/event_store.hpp"
#include "record/mp4_remux.hpp"

// 事件的查看、导出、删除（第 8d 步）：--events、--event-export、--event-delete 的核心逻辑。
// 只读写事件表和录像文件，不碰摄像头，界面开着（正在录别的事件）时也能用。CI 上测。
//
// 能对哪些状态做什么：
//   状态         导出                           删除
//   complete     可以                           可以
//   incomplete   可以（导出到中断的地方，提示）   可以
//   recording    不行（还在写，或者程序被杀了、下次启动时恢复）
//   lost         不行（文件不在了）              可以（清掉截图，记录标 deleted）
//   deleted      不行                           不行（已经删过）
namespace rksec::record {

struct ExportResult {
    std::string video;     // 导出的 MP4
    std::string snapshot;  // 导出的截图（没有截图时为空）
    std::int64_t video_bytes = 0;
    RemuxInfo info;
    bool incomplete = false;  // 事件状态是 incomplete
};

// 导出到 dir（没有就建，权限 0700）。文件名和录像一样：20261104_220000_12.mp4 / .jpg。
// 已经有同名文件时不覆盖，返回失败。全部文件权限 0600。
// 截图导出失败时连录像也删掉：要么全导出，要么什么都不留
bool export_event(EventStore &store, std::int64_t id, const std::string &dir, ExportResult &out, std::string &err);

// 删掉录像文件、清掉截图，记录标 deleted（时间、陌生人编号留着）。row 是删除前的记录
bool delete_event(EventStore &store, std::int64_t id, EventRow &row, std::string &err);

// 状态的中文：complete → 完整 …
const char *status_text(const std::string &status);

// --events 的一行：#12  2026-11-04 22:00:00  23.4 秒  11.2 MB  完整  有截图  陌生人 3,5  20261104_220000_12.mp4
std::string event_line(const EventRow &r);

}  // namespace rksec::record
