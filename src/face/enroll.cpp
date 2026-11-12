#include "face/enroll.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace rksec::face {

std::string reject_reason(const EnrollCandidate &c, const EnrollOptions &opts)
{
    std::array<char, 96> buf{};
    if (c.faces == 0) {
        return "没检测到脸";
    }
    if (c.faces > 1) {
        std::snprintf(buf.data(), buf.size(), "有 %d 张脸（登记时画面里只能有一个人）", c.faces);
        return buf.data();
    }
    if (c.width < opts.min_width) {
        std::snprintf(buf.data(), buf.size(), "脸太小（宽 %.0f 像素，要 %.0f 以上，靠近一点）",
                      static_cast<double>(c.width), static_cast<double>(opts.min_width));
        return buf.data();
    }
    if (c.score < opts.min_score) {
        std::snprintf(buf.data(), buf.size(), "人脸分数低（%.2f，要 %.2f 以上：侧脸、模糊或者光线暗）",
                      static_cast<double>(c.score), static_cast<double>(opts.min_score));
        return buf.data();
    }
    return {};
}

std::vector<std::size_t> pick_enroll_frames(const std::vector<EnrollCandidate> &c, const EnrollOptions &opts)
{
    std::vector<std::size_t> usable;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (reject_reason(c[i], opts).empty()) {
            usable.push_back(i);
        }
    }
    // 分数高的优先；一样时帧号小的优先（结果确定，测试好写）
    std::stable_sort(usable.begin(), usable.end(),
                     [&c](std::size_t a, std::size_t b) { return c[a].score > c[b].score; });
    std::vector<std::size_t> picked;
    for (const std::size_t i : usable) {
        if (static_cast<int>(picked.size()) >= opts.want) {
            break;
        }
        const bool far = std::all_of(picked.begin(), picked.end(),
                                     [&](std::size_t j) { return std::abs(c[i].frame - c[j].frame) >= opts.min_gap; });
        if (far) {
            picked.push_back(i);
        }
    }
    std::sort(picked.begin(), picked.end(), [&c](std::size_t a, std::size_t b) { return c[a].frame < c[b].frame; });
    return picked;
}

EnrollCheck check_enroll(const std::vector<std::vector<float>> &embs, const std::string &name, const Gallery &existing,
                         float threshold, const EnrollOptions &opts)
{
    EnrollCheck r;
    if (embs.empty()) {
        r.reason = "没有可用的照片";
        return r;
    }
    for (std::size_t i = 0; i < embs.size(); ++i) {
        for (std::size_t j = i + 1; j < embs.size(); ++j) {
            r.min_self = std::min(r.min_self, cosine(embs[i], embs[j]));
        }
        for (const auto &[other, s] : existing.scores(embs[i])) {
            if (other != name && s > r.closest_score) {
                r.closest_score = s;
                r.closest_other = other;
            }
        }
    }
    std::array<char, 160> buf{};
    if (r.min_self < opts.min_self_similarity) {
        std::snprintf(buf.data(), buf.size(), "这几张不像同一个人（最低相似度 %.2f，要 %.2f 以上）：中途有别人入镜？",
                      static_cast<double>(r.min_self), static_cast<double>(opts.min_self_similarity));
        r.reason = buf.data();
        return r;
    }
    if (r.closest_score >= threshold) {
        std::snprintf(buf.data(), buf.size(), "和已登记的“%s”很像（%.2f，识别阈值 %.2f）：同一个人请用同一个名字登记",
                      r.closest_other.c_str(), static_cast<double>(r.closest_score), static_cast<double>(threshold));
        r.reason = buf.data();
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace rksec::face
