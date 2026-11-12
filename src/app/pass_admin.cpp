#include "app/pass_admin.hpp"
#include "access/pass_store.hpp"
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
namespace rksec::app {
int pass_list(const config::AppConfig &cfg, int limit)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(cfg.face.database, ec);
    if (ec) {
        std::fprintf(stderr, "数据库状态检查失败\n");
        return 4;
    }
    if (!exists) {
        std::printf("还没有通行记录（数据库不存在）\n");
        return 0;
    }
    std::string error;
    auto store = access::PassStore::open(cfg.face.database, error, true);
    std::vector<access::PassRow> rows;
    if (!store || !store->list(rows, limit, error)) {
        std::fprintf(stderr, "通行记录查询失败：%s\n", error.c_str());
        return 4;
    }
    std::printf("通行记录 %zu 条（新的在前；时间为 UTC）\n", rows.size());
    for (const auto &row : rows) {
        const auto seconds = static_cast<std::time_t>(row.occurred_ms / 1000);
        std::tm utc{};
        std::array<char, 32> when{};
        if (gmtime_r(&seconds, &utc) == nullptr ||
            std::strftime(when.data(), when.size(), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
            std::fprintf(stderr, "通行时间无效\n");
            return 4;
        }
        std::printf("#%lld %s %s.%03lldZ 分数=%.3f 截图=%s%s\n", static_cast<long long>(row.id),
                    row.person_name.c_str(), when.data(), static_cast<long long>(row.occurred_ms % 1000), row.score,
                    row.has_snapshot ? "有" : "无", row.person_id ? "" : " [人员已删除]");
    }
    return 0;
}
}  // namespace rksec::app
