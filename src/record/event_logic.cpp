#include "record/event_logic.hpp"

namespace rksec::record {

EventAction EventTracker::update(bool stranger, std::int64_t now_us)
{
    if (!active_) {
        if (!stranger) {
            return EventAction::none;
        }
        active_ = true;
        event_start_ = now_us;
        segment_start_ = now_us;
        last_seen_ = now_us;
        return EventAction::start;
    }
    if (stranger) {
        last_seen_ = now_us;
    }
    if (now_us - last_seen_ >= opts_.post_us) {
        active_ = false;
        return EventAction::stop;
    }
    if (now_us - segment_start_ >= opts_.max_us) {
        segment_start_ = now_us;
        return EventAction::split;
    }
    return EventAction::none;
}

}  // namespace rksec::record
