#include "face/retinaface.hpp"

#include <algorithm>
#include <cmath>

namespace rksec::face {

namespace {

constexpr std::array<int, 3> kSteps = {8, 16, 32};
constexpr std::array<std::array<float, 2>, 3> kMinSizes = {{{16, 32}, {64, 128}, {256, 512}}};
constexpr float kVarCenter = 0.1F;
constexpr float kVarSize = 0.2F;

float clampf(float v, float lo, float hi)
{
    return std::min(std::max(v, lo), hi);
}

}  // namespace

std::vector<Prior> retinaface_priors(int model_w, int model_h)
{
    std::vector<Prior> out;
    const auto fw = static_cast<float>(model_w);
    const auto fh = static_cast<float>(model_h);
    for (std::size_t k = 0; k < kSteps.size(); ++k) {
        const int step = kSteps[k];
        const int rows = (model_h + step - 1) / step;  // 向上取整，和原始实现一致
        const int cols = (model_w + step - 1) / step;
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) {
                for (const float size : kMinSizes[k]) {
                    out.push_back({(static_cast<float>(j) + 0.5F) * static_cast<float>(step) / fw,
                                   (static_cast<float>(i) + 0.5F) * static_cast<float>(step) / fh, size / fw,
                                   size / fh});
                }
            }
        }
    }
    return out;
}

bool check_retinaface_outputs(const std::vector<std::vector<int>> &dims, std::size_t priors, std::string &err)
{
    if (dims.size() != 3) {
        err = "RetinaFace 应该有 3 个输出（框、分数、关键点），实际 " + std::to_string(dims.size()) + " 个";
        return false;
    }
    const std::array<int, 3> last = {4, 2, 10};
    const std::array<const char *, 3> names = {"框", "分数", "关键点"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &d = dims[i];
        // 运行库给的维度可能是 [1, N, 4] 或 [1, N, 4, 0]（第 4 维写 0）
        const bool ok = d.size() >= 3 && d[0] == 1 && d[1] == static_cast<int>(priors) && d[2] == last[i] &&
                        (d.size() == 3 || d[3] <= 1);
        if (!ok) {
            std::string got;
            for (std::size_t k = 0; k < d.size(); ++k) {
                got += (k == 0 ? "" : "x") + std::to_string(d[k]);
            }
            err = std::string{"第 "} + std::to_string(i) + " 个输出（" + names[i] + "）应为 1x" +
                  std::to_string(priors) + "x" + std::to_string(last[i]) + "，实际 " + got;
            return false;
        }
    }
    return true;
}

std::vector<Face> retinaface_decode(const float *loc, const float *conf, const float *landm,
                                    const std::vector<Prior> &priors, int model_w, int model_h,
                                    const RetinaFaceOptions &opts)
{
    const auto fw = static_cast<float>(model_w);
    const auto fh = static_cast<float>(model_h);
    std::vector<Face> cand;
    for (std::size_t i = 0; i < priors.size(); ++i) {
        const float score = conf[i * 2 + 1];
        if (score <= opts.score_threshold) {
            continue;
        }
        const Prior &p = priors[i];
        const float *l = loc + i * 4;
        const float cx = p[0] + l[0] * kVarCenter * p[2];
        const float cy = p[1] + l[1] * kVarCenter * p[3];
        const float w = p[2] * std::exp(l[2] * kVarSize);
        const float h = p[3] * std::exp(l[3] * kVarSize);
        Face f;
        f.box.x1 = clampf((cx - w / 2) * fw, 0, fw);
        f.box.y1 = clampf((cy - h / 2) * fh, 0, fh);
        f.box.x2 = clampf((cx + w / 2) * fw, 0, fw);
        f.box.y2 = clampf((cy + h / 2) * fh, 0, fh);
        f.box.score = score;
        f.box.cls = 0;
        const float *m = landm + i * 10;
        for (std::size_t k = 0; k < 5; ++k) {
            f.landmarks[k].x = (p[0] + m[2 * k] * kVarCenter * p[2]) * fw;
            f.landmarks[k].y = (p[1] + m[2 * k + 1] * kVarCenter * p[3]) * fh;
        }
        cand.push_back(f);
    }
    // NMS：分数从高到低，去掉和已留下的框重叠超过阈值的
    std::stable_sort(cand.begin(), cand.end(), [](const Face &a, const Face &b) { return a.box.score > b.box.score; });
    std::vector<Face> out;
    for (const Face &f : cand) {
        const bool dup = std::any_of(out.begin(), out.end(), [&](const Face &kept) {
            return detect::iou(kept.box, f.box) > opts.nms_threshold;
        });
        if (!dup) {
            out.push_back(f);
            if (static_cast<int>(out.size()) >= opts.max_faces) {
                break;
            }
        }
    }
    return out;
}

void to_source(std::vector<Face> &faces, const media::Letterbox &lb, int src_w, int src_h)
{
    const auto fw = static_cast<float>(src_w);
    const auto fh = static_cast<float>(src_h);
    for (Face &f : faces) {
        std::vector<detect::Detection> one{f.box};
        detect::to_source(one, lb, src_w, src_h);
        f.box = one[0];
        for (Point &p : f.landmarks) {
            p.x = clampf(static_cast<float>(lb.to_source_x(static_cast<double>(p.x))), 0, fw);
            p.y = clampf(static_cast<float>(lb.to_source_y(static_cast<double>(p.y))), 0, fh);
        }
    }
}

int largest_face(const std::vector<Face> &faces)
{
    int best = -1;
    float best_area = -1;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const detect::Detection &b = faces[i].box;
        const float area = (b.x2 - b.x1) * (b.y2 - b.y1);
        if (area > best_area) {
            best_area = area;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace rksec::face
