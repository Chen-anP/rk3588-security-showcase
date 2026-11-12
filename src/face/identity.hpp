#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "track/tracker.hpp"

// 给跟踪编号配名字（第 7c 步）。纯逻辑，CI 上能测；时间由调用方传入（测试里不用真的等）。
//
// 为什么不是每一帧都识别：人脸检测 + 识别一次要 10 毫秒左右，画面里有几个人时每帧都做会占满 NPU，
// 而且一个人的身份不会每帧都变。有了跟踪编号以后，只要隔一段时间认一次，结论跟着编号走：
// - 选谁：还没有结论的人优先（隔 retry 再试），有结论的隔 recheck 复查一次（防止编号换了人）
// - 结论：同一个结论（某个名字，或者“陌生人”）连续出现 confirm 次才采纳——单次认错（侧脸、模糊）不改显示
// - 没检测到脸（背对、低头）不算结论，不影响已有的结论
// - 编号一段时间（forget）没出现就删掉它的记录
namespace rksec::face {

enum class IdState : std::uint8_t {
    pending,   // 还没有结论
    known,     // 认出来了：name
    stranger,  // 有脸，但不在底库里
};

struct Identity {
    IdState state = IdState::pending;
    std::string name;
    float score = 0;  // 最近一次采纳时的相似度
};

struct IdentityOptions {
    int confirm = 2;
    std::chrono::milliseconds retry{300};
    std::chrono::milliseconds recheck{2000};
    std::chrono::milliseconds forget{5000};
};

// 一次识别的结果
struct Observation {
    bool face = false;     // 裁出来的那块里有没有脸
    bool matched = false;  // 和底库比，达到阈值
    std::string name;      // matched 时是名字
    float score = 0;       // 和最像的人的相似度
};

class IdentityBook {
  public:
    using Clock = std::chrono::steady_clock;
    explicit IdentityBook(IdentityOptions opts = {});

    // 选下一个要识别的人：tracks 里类别是人（cls 0）的、已经到时间的；没试过的最先，
    // 其他按到期时间先后；一样时选框大的（离得近，脸清楚）。都没到时间返回 nullopt。返回 tracks 的下标
    std::optional<std::size_t> pick(const std::vector<track::Track> &tracks, Clock::time_point now) const;

    // 已经把 id 送去识别了（从这时开始算下次的间隔）
    void attempted(int id, Clock::time_point now);

    // 识别结果回来了
    void record(int id, const Observation &obs, Clock::time_point now);

    // 每一帧的跟踪结果：记下这些编号还在；很久没出现的编号删掉
    void seen(const std::vector<track::Track> &tracks, Clock::time_point now);

    Identity get(int id) const;

    struct Stats {
        std::uint64_t attempts = 0;
        std::uint64_t no_face = 0;    // 裁出来的块里没检测到脸
        std::uint64_t matched = 0;    // 有脸，认出来了
        std::uint64_t unmatched = 0;  // 有脸，不在底库里
        std::uint64_t adopted = 0;    // 采纳新结论的次数（pending → 名字、名字 → 陌生人……）
    };
    Stats stats() const { return stats_; }

  private:
    struct Entry {
        Identity id;
        bool tried = false;
        Clock::time_point last_attempt;
        Clock::time_point last_seen;
        // 最近连续出现的同一个结论
        bool cand_matched = false;
        std::string cand_name;
        int streak = 0;
    };

    IdentityOptions opts_;
    std::map<int, Entry> entries_;
    Stats stats_;
};

}  // namespace rksec::face
