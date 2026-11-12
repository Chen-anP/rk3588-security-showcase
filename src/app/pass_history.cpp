#include "app/pass_history.hpp"
#include <chrono>
#include <utility>
namespace rksec::app {
PassHistory::PassHistory(std::string path) : path_(std::move(path)), worker_([this] { run(); }) {}
PassHistory::~PassHistory()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
}
std::optional<HistoryResult> PassHistory::take()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(result_);
    result_.reset();
    return result;
}
void PassHistory::run()
{
    for (;;) {
        HistoryResult result;
        auto store = access::PassStore::open(path_, result.error, true);
        if (store && !store->list(result.rows, 50, result.error))
            result.rows.clear();
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        result_ = std::move(result);
        if (wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; }))
            return;
    }
}
}  // namespace rksec::app
