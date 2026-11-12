#pragma once
#include "access/access_machine.hpp"
#include <functional>
namespace rksec::access {
struct FaceEvent {
    std::uint64_t session = 0, sequence = 0;
    Machine::Clock::time_point captured{};
    int track = 0;
    std::int64_t person_id = 0;
    bool found = false, matched = false;
    double score = 0;
};
struct View {
    State state = State::idle;
    bool persisted = false, pir_degraded = false, pir_active = false;
    std::uint64_t grants = 0, write_errors = 0;
    std::string error;
};
class Coordinator {
  public:
    using Sink = std::function<bool(const FaceEvent &, std::uint64_t, std::string &)>;
    using Now = std::function<Machine::Clock::time_point()>;
    explicit Coordinator(Sink sink, Now now = {}) : sink_(std::move(sink)), now_(std::move(now)) {}
    View update(Machine::Clock::time_point now, Input input = {}, std::optional<FaceEvent> event = {});
    State state() const { return machine_.state(); }
    std::uint64_t session() const { return machine_.session(); }

  private:
    Machine machine_;
    Sink sink_;
    Now now_;
    View view_;
    Machine::Clock::time_point session_started_{};
    std::optional<Machine::Clock::time_point> frame_;
    std::vector<int> tracks_;
    std::uint64_t sequence_ = 0;
};
bool trigger_motion(bool edge, bool active, State state);
std::string status_text(const View &view);
}  // namespace rksec::access
