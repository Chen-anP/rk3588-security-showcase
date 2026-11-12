#include "track/tracker.hpp"

#include <algorithm>
#include <tuple>

namespace rksec::track {

Box to_box(const detect::Detection &d)
{
    const auto w = static_cast<double>(d.x2 - d.x1);
    const auto h = static_cast<double>(d.y2 - d.y1);
    return {static_cast<double>(d.x1) + w / 2, static_cast<double>(d.y1) + h / 2, h > 0 ? w / h : 1.0, h};
}

detect::Detection to_detection(const Box &b)
{
    const double w = b.a * b.h;
    detect::Detection d;
    d.x1 = static_cast<float>(b.cx - w / 2);
    d.y1 = static_cast<float>(b.cy - b.h / 2);
    d.x2 = static_cast<float>(b.cx + w / 2);
    d.y2 = static_cast<float>(b.cy + b.h / 2);
    return d;
}

Tracker::Tracker(TrackerOptions opts) : opts_(opts) {}

void Tracker::match(std::vector<std::size_t> &tracks, std::vector<std::size_t> &dets,
                    const std::vector<detect::Detection> &all, float min_iou,
                    std::vector<std::pair<std::size_t, std::size_t>> &pairs) const
{
    std::vector<std::tuple<float, std::size_t, std::size_t>> cand;  // (IoU, 轨迹, 框)
    for (const std::size_t t : tracks) {
        const detect::Detection pred = to_detection(slots_[t].kf.box());
        for (const std::size_t d : dets) {
            if (all[d].cls != slots_[t].last.cls) {
                continue;  // 不同类别不配
            }
            const float v = detect::iou(pred, all[d]);
            if (v >= min_iou) {
                cand.emplace_back(v, t, d);
            }
        }
    }
    // IoU 从大到小；一样大时按下标，结果确定
    std::sort(cand.begin(), cand.end(), [](const auto &a, const auto &b) {
        if (std::get<0>(a) != std::get<0>(b)) {
            return std::get<0>(a) > std::get<0>(b);
        }
        return std::tie(std::get<1>(a), std::get<2>(a)) < std::tie(std::get<1>(b), std::get<2>(b));
    });
    for (const auto &[v, t, d] : cand) {
        const auto ti = std::find(tracks.begin(), tracks.end(), t);
        const auto di = std::find(dets.begin(), dets.end(), d);
        if (ti == tracks.end() || di == dets.end()) {
            continue;  // 已经配过了
        }
        pairs.emplace_back(t, d);
        tracks.erase(ti);
        dets.erase(di);
    }
}

std::vector<Track> Tracker::update(const std::vector<detect::Detection> &dets)
{
    for (Slot &s : slots_) {
        s.kf.predict();
        ++s.age;
    }

    std::vector<std::size_t> high;
    std::vector<std::size_t> low;
    for (std::size_t i = 0; i < dets.size(); ++i) {
        (dets[i].score >= opts_.high_score ? high : low).push_back(i);
    }
    std::vector<std::size_t> active;     // 已确认 + 暂时丢失
    std::vector<std::size_t> tentative;  // 待确认
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        (slots_[i].state == State::tentative ? tentative : active).push_back(i);
    }

    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    match(active, high, dets, opts_.match_iou, pairs);     // 第一轮
    match(active, low, dets, opts_.low_match_iou, pairs);  // 第二轮：低分框只用来接续
    match(tentative, high, dets, opts_.match_iou, pairs);  // 第三轮

    std::vector<bool> updated(slots_.size(), false);
    for (const auto &[t, d] : pairs) {
        Slot &s = slots_[t];
        s.kf.update(to_box(dets[d]));
        s.last = dets[d];
        ++s.hits;
        ++s.streak;
        updated[t] = true;
        if (s.state == State::lost) {
            s.state = State::confirmed;
            ++stats_.recovered;
        } else if (s.state == State::tentative && s.streak >= opts_.confirm_hits) {
            s.state = State::confirmed;
            s.id = next_id_++;
            ++stats_.created;
        }
        s.lost = 0;
    }

    // 没配上的轨迹：待确认的直接删；已确认的变丢失；丢失太久的删
    std::vector<Slot> kept;
    std::vector<bool> kept_updated;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot &s = slots_[i];
        if (!updated[i]) {
            s.streak = 0;
            if (s.state == State::tentative) {
                continue;
            }
            s.state = State::lost;
            if (++s.lost > opts_.max_lost) {
                ++stats_.removed;
                continue;
            }
        }
        kept.push_back(s);
        kept_updated.push_back(updated[i]);
    }
    slots_ = std::move(kept);

    // 剩下的高分框新建轨迹（待确认）
    for (const std::size_t d : high) {
        if (dets[d].score < opts_.new_track_score) {
            continue;
        }
        Slot s{to_box(dets[d])};
        s.last = dets[d];
        s.hits = 1;
        s.streak = 1;
        s.age = 1;
        if (opts_.confirm_hits <= 1) {
            s.state = State::confirmed;
            s.id = next_id_++;
            ++stats_.created;
        }
        slots_.push_back(s);
        kept_updated.push_back(true);
    }

    std::vector<Track> out;
    stats_.active = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot &s = slots_[i];
        if (s.state == State::tentative) {
            continue;
        }
        ++stats_.active;
        if (s.state == State::confirmed && kept_updated[i]) {
            Track t;
            t.id = s.id;
            t.box = s.last;
            t.vx = s.kf.vx();
            t.vy = s.kf.vy();
            t.hits = s.hits;
            t.age = s.age;
            out.push_back(t);
        }
    }
    std::sort(out.begin(), out.end(), [](const Track &a, const Track &b) { return a.id < b.id; });
    return out;
}

Tracker::Stats Tracker::stats() const
{
    return stats_;
}

}  // namespace rksec::track
