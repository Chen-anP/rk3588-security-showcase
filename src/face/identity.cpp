#include "face/identity.hpp"

#include <utility>

namespace rksec::face {

IdentityBook::IdentityBook(IdentityOptions opts) : opts_(opts) {}

std::optional<std::size_t> IdentityBook::pick(const std::vector<track::Track> &tracks, Clock::time_point now) const
{
    std::optional<std::size_t> best;
    Clock::time_point best_due{};
    float best_area = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const track::Track &t = tracks[i];
        if (t.box.cls != 0) {
            continue;
        }
        Clock::time_point due = Clock::time_point::min();  // 没试过：马上
        if (const auto it = entries_.find(t.id); it != entries_.end() && it->second.tried) {
            const Entry &e = it->second;
            due = e.last_attempt + (e.id.state == IdState::pending ? opts_.retry : opts_.recheck);
        }
        if (due > now) {
            continue;
        }
        const float area = (t.box.x2 - t.box.x1) * (t.box.y2 - t.box.y1);
        if (!best || due < best_due || (due == best_due && area > best_area)) {
            best = i;
            best_due = due;
            best_area = area;
        }
    }
    return best;
}

void IdentityBook::attempted(int id, Clock::time_point now)
{
    Entry &e = entries_[id];
    e.tried = true;
    e.last_attempt = now;
    e.last_seen = now;
    ++stats_.attempts;
}

void IdentityBook::record(int id, const Observation &obs, Clock::time_point now)
{
    if (!obs.face) {
        ++stats_.no_face;
        return;
    }
    ++(obs.matched ? stats_.matched : stats_.unmatched);
    Entry &e = entries_[id];
    e.last_seen = now;
    const std::string name = obs.matched ? obs.name : std::string{};
    if (e.streak > 0 && e.cand_matched == obs.matched && e.cand_name == name) {
        ++e.streak;
    } else {
        e.cand_matched = obs.matched;
        e.cand_name = name;
        e.streak = 1;
    }
    if (e.streak < opts_.confirm) {
        return;
    }
    const IdState state = obs.matched ? IdState::known : IdState::stranger;
    if (e.id.state != state || e.id.name != name) {
        ++stats_.adopted;
    }
    e.id.state = state;
    e.id.name = name;
    e.id.score = obs.score;
}

void IdentityBook::seen(const std::vector<track::Track> &tracks, Clock::time_point now)
{
    for (const track::Track &t : tracks) {
        entries_[t.id].last_seen = now;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.last_seen > opts_.forget) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

Identity IdentityBook::get(int id) const
{
    const auto it = entries_.find(id);
    return it == entries_.end() ? Identity{} : it->second.id;
}

}  // namespace rksec::face
