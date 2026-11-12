#pragma once

#include <cstdint>
#include <string>

#include "core/config.hpp"

// 事件录像的命令（第 8d 步）：列出、导出、删除。核心逻辑在 record/event_admin.hpp。
// 只用数据库（face.database）和录像目录，不碰摄像头，不拿单实例锁：界面开着时也能用。
//
// 退出码：0 成功；1 做不了（没有这个事件、状态不对、目标文件已经存在……），原因已打印；4 数据库打不开
namespace rksec::app {

// --events [条数]：最新的若干条事件（默认 20），末尾汇总录像占用和配额
int event_list(const config::AppConfig &cfg, int limit);

// --event-export 编号 目录：录像转成普通 MP4 + 截图，导出到目录
int event_export(const config::AppConfig &cfg, std::int64_t id, const std::string &dir);

// --event-delete 编号：删录像和截图，记录标“已删除”
int event_delete(const config::AppConfig &cfg, std::int64_t id);

}  // namespace rksec::app
