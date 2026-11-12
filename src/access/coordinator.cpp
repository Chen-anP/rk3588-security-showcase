#include "access/coordinator.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
namespace rksec::access {
View Coordinator::update(Machine::Clock::time_point now, Input input, std::optional<FaceEvent> event)
{
    if (input.present_tracks) {
        frame_ = now;
        tracks_ = *input.present_tracks;
    }
    if (!frame_ || now - *frame_ > std::chrono::milliseconds(500) || tracks_.empty()) {
        machine_.invalidate_candidate();
    }
    const auto before = machine_.session();
    input.sample.reset();
    auto result = machine_.update(now, input);
    if (machine_.session() != before) {
        session_started_ = now;
        sequence_ = 0;
        view_.persisted = false;
        view_.error.clear();
    }
    if (event && event->session == machine_.session() && event->sequence > sequence_) {
        const auto &e = *event;
        const bool fresh = e.captured >= session_started_ && e.captured <= now &&
                           now - e.captured <= std::chrono::milliseconds(500) && frame_ &&
                           now - *frame_ <= std::chrono::milliseconds(500);
        const bool visible = std::find(tracks_.begin(), tracks_.end(), e.track) != tracks_.end();
        Input sample;
        sample.pir_available = input.pir_available;
        if (sequence_ && e.sequence > sequence_ + 1) {
            sample.sample = Sample{e.session, e.sequence - 1, e.track, Verdict::no_face, {}};
            machine_.update(now, sample);
        }
        sequence_ = e.sequence;
        const bool valid = fresh && visible && e.found && std::isfinite(e.score);
        Verdict verdict = Verdict::no_face;
        if (valid && !e.matched) {
            verdict = Verdict::stranger;
        }
        if (valid && e.matched && e.person_id > 0 && e.score >= 0 && e.score <= 1) {
            verdict = Verdict::known;
        }
        sample.sample = Sample{e.session, e.sequence, e.track, verdict,
                               verdict == Verdict::known ? std::to_string(e.person_id) : std::string{}};
        result = machine_.update(now, sample);
        if (result.action == Action::grant) {
            std::string error;
            try {
                view_.persisted = sink_ && sink_(e, result.session, error);
            } catch (const std::exception &) {
                view_.persisted = false;
                error = "record sink exception";
            }
            if (view_.persisted) {
                ++view_.grants;
                view_.error.clear();
                if (now_) {
                    Input tick;
                    tick.pir_available = input.pir_available;
                    machine_.update(now_(), tick);
                }
            } else {
                machine_.record_failed(result.person);
                ++view_.write_errors;
                view_.error = error.empty() ? "record write failed" : error;
            }
        }
    }
    view_.state = machine_.state();
    view_.pir_degraded = !input.pir_available;
    if (view_.state != State::granted) {
        view_.persisted = false;
    }
    return view_;
}
bool trigger_motion(bool edge, bool active, State state)
{
    return edge || (active && state == State::idle);
}
std::string status_text(const View &v)
{
    std::string s;
    if (!v.error.empty()) {
        s = "通行记录失败";
    } else if (v.state == State::granted && v.persisted) {
        s = "模拟开门";
    } else if (v.state == State::granted) {
        s = "记录处理中";
    } else if (v.state == State::identifying) {
        s = "识别中";
    } else if (v.state == State::denied) {
        s = "陌生人";
    } else if (v.state == State::cooldown) {
        s = "冷却中";
    } else {
        s = "待机";
    }
    if (v.pir_degraded) {
        s += " / 红外故障";
    }
    return s;
}
}  // namespace rksec::access
