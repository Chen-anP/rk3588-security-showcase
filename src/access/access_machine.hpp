#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace rksec::access {
enum class State : std::uint8_t { idle, identifying, granted, denied, cooldown };
enum class Verdict : std::uint8_t { no_face, known, stranger };
enum class Action : std::uint8_t { none, grant, deny, timeout, suppressed, capacity };
struct Options {
    std::chrono::milliseconds identify_timeout{10000};
    std::chrono::milliseconds grant_duration{3000};
    std::chrono::milliseconds cooldown{5000};
    std::chrono::milliseconds rearm_absence{5000};
    int confirmations = 2;
};
struct Sample {
    std::uint64_t session = 0;
    std::uint64_t sequence = 0;  // 同一会话严格递增；同一次推理不得重复计数。
    int track = 0;
    Verdict verdict = Verdict::no_face;
    std::string person;  // 稳定人员键；数据库接入时使用人员 ID，不能使用跟踪编号。
};
struct Input {
    bool motion = false;  // 触发事件，不是持续有效电平。
    bool pir_available = true;
    // 缺省：没有新检测帧；空列表：新帧确认没有人。
    std::optional<std::vector<int>> present_tracks;
    std::optional<Sample> sample;
};
struct Result {
    State state = State::idle;
    Action action = Action::none;
    std::uint64_t session = 0;
    int track = 0;
    std::string person;
    bool pir_degraded = false;
};
// 单线程纯逻辑。调用方定时 tick 并串行投递输入，时间统一使用 steady_clock。
// 不控制外设，不访问数据库；grant/deny 是一次性动作，交给外层执行。
class Machine {
  public:
    using Clock = std::chrono::steady_clock;
    explicit Machine(Options options = {});
    Result update(Clock::time_point now, const Input &input = {});
    std::uint64_t session() const { return session_; }
    void invalidate_candidate() { streak_ = 0; }
    void record_failed(const std::string &person);
    State state() const { return state_; }
    std::size_t suppressed_people() const { return seen_.size(); }
    static constexpr std::size_t kMaxPeople = 64;

  private:
    struct Seen {
        std::optional<Clock::time_point> absent_since;
    };
    Options options_;
    State state_ = State::idle;
    std::optional<Clock::time_point> last_time_;
    Clock::time_point deadline_{};
    std::uint64_t session_ = 0;
    std::uint64_t sequence_ = 0;
    int candidate_track_ = 0;
    Verdict candidate_verdict_ = Verdict::no_face;
    std::string candidate_person_;
    int streak_ = 0;
    std::map<std::string, Seen> seen_;
};
}  // namespace rksec::access
