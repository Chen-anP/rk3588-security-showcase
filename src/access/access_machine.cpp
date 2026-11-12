#include "access/access_machine.hpp"
#include <stdexcept>
namespace rksec::access {
Machine::Machine(Options options) : options_(options)
{
    const auto valid = [](std::chrono::milliseconds value) {
        return value.count() > 0 && value <= std::chrono::hours(24);
    };
    if (!valid(options.identify_timeout) || !valid(options.grant_duration) || !valid(options.cooldown) ||
        !valid(options.rearm_absence) || options.confirmations < 2 || options.confirmations > 8) {
        throw std::invalid_argument("invalid access state machine options");
    }
}
void Machine::record_failed(const std::string &person)
{
    if (state_ == State::granted && candidate_person_ == person) {
        seen_.erase(person);
    }
}
Result Machine::update(Clock::time_point now, const Input &input)
{
    Result result;
    result.pir_degraded = !input.pir_available;
    const auto finish = [&] {
        result.state = state_;
        result.session = session_;
        return result;
    };
    if (last_time_ && now < *last_time_) {
        return finish();
    }
    last_time_ = now;
    // 同一人的新跟踪编号必须在缺席清理之前关联，避免编号切换绕过去重。
    if (input.sample && input.sample->session == session_ && input.sample->sequence > sequence_ &&
        input.sample->verdict == Verdict::known && input.sample->track > 0 && !input.sample->person.empty() &&
        input.sample->person.size() <= 128) {
        auto it = seen_.find(input.sample->person);
        if (it != seen_.end()) {
            it->second = {std::nullopt};
        }
    }
    // 保守去重：只有新检测帧持续确认画面无人才能重置。
    // 仍有人员时不依据跟踪编号消失解除，避免编号变化/无脸绕过去重。
    if (input.present_tracks) {
        for (auto it = seen_.begin(); it != seen_.end();) {
            const auto &tracks = *input.present_tracks;
            if (!tracks.empty()) {
                it->second.absent_since.reset();
            } else if (!it->second.absent_since) {
                it->second.absent_since = now;
            }
            if (it->second.absent_since && now - *it->second.absent_since >= options_.rearm_absence) {
                it = seen_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (state_ == State::granted || state_ == State::denied) {
        if (now >= deadline_) {
            state_ = State::cooldown;
            deadline_ += options_.cooldown;
        }
    }
    if (state_ == State::cooldown && now >= deadline_) {
        state_ = State::idle;
    }
    if (state_ == State::identifying && now >= deadline_) {
        state_ = State::cooldown;
        deadline_ = now + options_.cooldown;
        streak_ = 0;
        result.action = Action::timeout;
        return finish();
    }
    if (state_ == State::idle &&
        (input.motion || (!input.pir_available && input.present_tracks && !input.present_tracks->empty()))) {
        state_ = State::identifying;
        deadline_ = now + options_.identify_timeout;
        ++session_;
        sequence_ = 0;
        streak_ = 0;
    }
    if (!input.sample || input.sample->session != session_ || input.sample->sequence == 0 ||
        input.sample->sequence <= sequence_) {
        return finish();
    }
    const auto &sample = *input.sample;
    sequence_ = sample.sequence;
    if (state_ != State::identifying) {
        return finish();
    }
    if (sample.person.size() > 128 || sample.track <= 0 ||
        (sample.verdict != Verdict::known && sample.verdict != Verdict::stranger) ||
        (sample.verdict == Verdict::known && sample.person.empty())) {
        streak_ = 0;
        return finish();
    }
    if (candidate_track_ == sample.track && candidate_verdict_ == sample.verdict &&
        candidate_person_ == sample.person) {
        ++streak_;
    } else {
        candidate_track_ = sample.track;
        candidate_verdict_ = sample.verdict;
        candidate_person_ = sample.person;
        streak_ = 1;
    }
    if (streak_ < options_.confirmations) {
        return finish();
    }
    result.track = sample.track;
    result.person = sample.verdict == Verdict::known ? sample.person : std::string{};
    if (sample.verdict == Verdict::stranger) {
        state_ = State::denied;
        result.action = Action::deny;
        deadline_ = now + options_.grant_duration;
    } else if (seen_.find(sample.person) != seen_.end() || seen_.size() >= kMaxPeople) {
        result.action = seen_.find(sample.person) != seen_.end() ? Action::suppressed : Action::capacity;
        state_ = State::cooldown;
        deadline_ = now + options_.cooldown;
    } else {
        seen_.emplace(sample.person, Seen{std::nullopt});
        state_ = State::granted;
        result.action = Action::grant;
        deadline_ = now + options_.grant_duration;
    }
    streak_ = 0;
    return finish();
}
}  // namespace rksec::access
