#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace rksec::access {
struct NewPass {
    std::string request_id;  // 每次通行使用进程唯一前缀+会话号；重试必须复用。
    std::int64_t person_id = 0;
    std::int64_t occurred_ms = 0;  // Unix UTC 毫秒
    double score = 0;
    std::vector<std::uint8_t> snapshot;
};
struct PassRow {
    std::int64_t id = 0;
    std::string request_id;
    std::optional<std::int64_t> person_id;
    std::int64_t person_ref = 0;  // 历史 ID，人员删除后仍保留；不用于重新关联人员。
    std::string person_name;
    std::int64_t occurred_ms = 0;
    double score = 0;
    bool has_snapshot = false;
};
class PassStore {
  public:
    static std::unique_ptr<PassStore> open(const std::string &path, std::string &error, bool read_only = false);
    ~PassStore();
    PassStore(const PassStore &) = delete;
    PassStore &operator=(const PassStore &) = delete;
    // 同请求同内容返回原 id、inserted=false；同请求不同内容拒绝。
    bool add(const NewPass &pass, std::int64_t &id, bool &inserted, std::string &error);
    bool list(std::vector<PassRow> &rows, int limit, std::string &error);

  private:
    PassStore();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace rksec::access
