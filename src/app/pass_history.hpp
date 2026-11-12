#pragma once
#include "access/pass_store.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
namespace rksec::app {
struct HistoryResult {
    std::vector<access::PassRow> rows;
    std::string error;
};
// Only this worker touches SQLite. UI takes a bounded metadata snapshot.
class PassHistory {
  public:
    explicit PassHistory(std::string path);
    ~PassHistory();
    PassHistory(const PassHistory &) = delete;
    PassHistory &operator=(const PassHistory &) = delete;
    std::optional<HistoryResult> take();

  private:
    void run();
    std::string path_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::optional<HistoryResult> result_;
    std::thread worker_;
};
}  // namespace rksec::app
