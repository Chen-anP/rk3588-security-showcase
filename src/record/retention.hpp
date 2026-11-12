#pragma once

#include <cstdint>
#include <vector>

// 录像的磁盘配额（第 8b 步）：总大小超过配额时，从最旧的开始删，直到不超。纯逻辑，CI 上测。
// 正在录的那个（keep_id）不删。
namespace rksec::record {

struct StoredClip {
    std::int64_t id = 0;     // 事件编号（越大越新）
    std::int64_t bytes = 0;  // 文件大小
};

// 返回要删的编号（从旧到新）。clips 不要求排好序
std::vector<std::int64_t> plan_retention(std::vector<StoredClip> clips, std::int64_t quota_bytes,
                                         std::int64_t keep_id = -1);

}  // namespace rksec::record
