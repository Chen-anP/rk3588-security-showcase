#include "core/frame_pool.hpp"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rksec {

struct FramePool::State {
    explicit State(int n, ReleaseFn fn)
        : frames(static_cast<std::size_t>(n)), busy(static_cast<std::size_t>(n)), on_release(std::move(fn))
    {
    }

    mutable std::mutex mutex;
    std::vector<Frame> frames;  // 预先分配，acquire 时只填数据
    std::vector<bool> busy;
    ReleaseFn on_release;
    bool alive = true;  // FramePool 还在
    int calling = 0;    // 正在锁外执行的 on_release 个数
    std::condition_variable idle;
    Stats stats;
    int in_use = 0;
};

// shared_ptr 的删除器：不 delete（Frame 属于池子），而是把缓冲区还回去
struct FramePool::Releaser {
    std::shared_ptr<State> state;

    void operator()(const Frame *f) const
    {
        const int index = f->index;
        bool call = false;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->busy[static_cast<std::size_t>(index)] = false;
            --state->in_use;
            ++state->stats.released;
            if (state->alive && state->on_release) {
                call = true;
                ++state->calling;
            }
        }
        if (!call) {
            return;
        }
        state->on_release(index);  // 锁外调用：回调里可以再调用池子的函数
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            --state->calling;
        }
        state->idle.notify_all();
    }
};

FramePool::FramePool(int count, ReleaseFn on_release)
{
    if (count <= 0) {
        throw std::invalid_argument("FramePool 缓冲区个数必须大于 0");
    }
    state_ = std::make_shared<State>(count, std::move(on_release));
    for (int i = 0; i < count; ++i) {
        state_->frames[static_cast<std::size_t>(i)].index = i;
    }
}

FramePool::~FramePool()
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->alive = false;
    state_->idle.wait(lock, [this] { return state_->calling == 0; });
}

FrameRef FramePool::acquire(int index, std::uint64_t sequence, std::chrono::steady_clock::time_point captured,
                            const std::uint8_t *data, std::size_t bytes, int dmabuf_fd)
{
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (index < 0 || index >= static_cast<int>(state_->frames.size()) ||
        state_->busy[static_cast<std::size_t>(index)]) {
        ++state_->stats.rejected;
        return nullptr;
    }
    auto &f = state_->frames[static_cast<std::size_t>(index)];
    f.sequence = sequence;
    f.captured = captured;
    f.data = data;
    f.bytes = bytes;
    f.dmabuf_fd = dmabuf_fd;
    state_->busy[static_cast<std::size_t>(index)] = true;
    ++state_->in_use;
    ++state_->stats.acquired;
    return {&f, Releaser{state_}};
}

int FramePool::count() const
{
    return static_cast<int>(state_->frames.size());
}

int FramePool::in_use() const
{
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->in_use;
}

FramePool::Stats FramePool::stats() const
{
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

}  // namespace rksec
