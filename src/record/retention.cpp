#include "record/retention.hpp"

#include <algorithm>

namespace rksec::record {

std::vector<std::int64_t> plan_retention(std::vector<StoredClip> clips, std::int64_t quota_bytes, std::int64_t keep_id)
{
    std::sort(clips.begin(), clips.end(), [](const StoredClip &a, const StoredClip &b) { return a.id < b.id; });
    std::int64_t total = 0;
    for (const auto &c : clips) {
        total += c.bytes;
    }
    std::vector<std::int64_t> out;
    for (const auto &c : clips) {
        if (total <= quota_bytes) {
            break;
        }
        if (c.id == keep_id) {
            continue;
        }
        out.push_back(c.id);
        total -= c.bytes;
    }
    return out;
}

}  // namespace rksec::record
