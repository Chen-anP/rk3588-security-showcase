#include "detect/yolo_post.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace rksec::detect {

namespace {

constexpr int kDflBins = 16;  // 每条边 16 个档位：框张量的通道数 = 4 × 16 = 64
constexpr std::size_t kBins = kDflBins;

// 阈值换成 int8：q > threshold_q 等价于 (q - zp) × scale > threshold
int threshold_q(float threshold, std::int32_t zp, float scale)
{
    const double x = std::floor(static_cast<double>(threshold) / static_cast<double>(scale) + zp);
    return static_cast<int>(std::clamp(x, -129.0, 127.0));
}

float dequant(std::int8_t q, std::int32_t zp, float scale)
{
    return static_cast<float>(static_cast<int>(q) - zp) * scale;
}

// 每个尺度几个张量：9 个输出 → 3（框、类别、分数和），6 个 → 2（没有分数和），其他 → 0（不支持）
std::size_t per_branch(std::size_t n)
{
    return n == 9 ? 3 : n == 6 ? 2 : 0;
}

// 第 b 个尺度的第 k 个张量
const TensorView &tensor(const std::vector<TensorView> &outs, std::size_t pb, int b, std::size_t k)
{
    return outs[static_cast<std::size_t>(b) * pb + k];
}

}  // namespace

float dfl_expect(const float *logits, int bins)
{
    // softmax 先减最大值，防止 exp 溢出
    float mx = logits[0];
    for (int i = 1; i < bins; ++i) {
        mx = std::max(mx, logits[i]);
    }
    float sum = 0;
    float acc = 0;
    for (int i = 0; i < bins; ++i) {
        const float e = std::exp(logits[i] - mx);
        sum += e;
        acc += e * static_cast<float>(i);
    }
    return acc / sum;
}

bool check_yolo11_outputs(const std::vector<TensorView> &outs, int model_w, int model_h, int &classes, std::string &err)
{
    const std::size_t pb = per_branch(outs.size());
    if (pb == 0U) {
        err = "输出张量应该是 9 个（3 个尺度 × 框、类别、分数和）或 6 个，实际 " + std::to_string(outs.size()) + " 个";
        return false;
    }
    classes = 0;
    for (int b = 0; b < 3; ++b) {
        const TensorView &box = tensor(outs, pb, b, 0);
        const TensorView &cls = tensor(outs, pb, b, 1);
        const std::string where = "第 " + std::to_string(b) + " 个尺度";
        if (box.c != 4 * kDflBins) {
            err = where + "的框张量通道数应为 64（4 条边 × 16 档），实际 " + std::to_string(box.c);
            return false;
        }
        // 3 个尺度的步长固定是 8、16、32（YOLO11 的结构决定的）：格子数 = 输入尺寸 / 步长
        const int stride = 8 << b;
        if (box.h * stride != model_h || box.w * stride != model_w) {
            err = where + "的格子数 " + std::to_string(box.w) + "x" + std::to_string(box.h) + " 和模型输入 " +
                  std::to_string(model_w) + "x" + std::to_string(model_h) + " 对不上（步长应为 " +
                  std::to_string(stride) + "）";
            return false;
        }
        if (cls.h != box.h || cls.w != box.w || cls.c <= 0) {
            err = where + "的类别张量尺寸和框张量不一致";
            return false;
        }
        if (classes != 0 && cls.c != classes) {
            err = where + "的类别数 " + std::to_string(cls.c) + " 和其他尺度不同";
            return false;
        }
        classes = cls.c;
        if (pb == 3U) {
            const TensorView &sum = tensor(outs, pb, b, 2);
            if (sum.c != 1 || sum.h != box.h || sum.w != box.w) {
                err = where + "的分数和张量应为 [1, 1, H, W]";
                return false;
            }
        }
        for (std::size_t k = 0; k < pb; ++k) {
            if (tensor(outs, pb, b, k).data == nullptr) {
                err = where + "有张量没有数据";
                return false;
            }
        }
    }
    return true;
}

std::vector<Detection> yolo11_decode(const std::vector<TensorView> &outs, int model_w, int model_h,
                                     const PostOptions &opts, std::string &err)
{
    int classes = 0;
    if (!check_yolo11_outputs(outs, model_w, model_h, classes, err)) {
        return {};
    }
    const std::size_t pb = per_branch(outs.size());
    std::vector<Detection> cand;
    std::array<float, 4 * kBins> logits{};

    for (int b = 0; b < 3; ++b) {
        const TensorView &box = tensor(outs, pb, b, 0);
        const TensorView &cls = tensor(outs, pb, b, 1);
        const TensorView *sum = pb == 3U ? &tensor(outs, pb, b, 2) : nullptr;
        const int gh = box.h;
        const int gw = box.w;
        const std::size_t plane = static_cast<std::size_t>(gh) * static_cast<std::size_t>(gw);
        const float stride = static_cast<float>(model_h) / static_cast<float>(gh);
        const int cls_thr = threshold_q(opts.score_threshold, cls.zp, cls.scale);
        const int sum_thr = sum != nullptr ? threshold_q(opts.score_threshold, sum->zp, sum->scale) : 0;

        for (int i = 0; i < gh; ++i) {
            for (int j = 0; j < gw; ++j) {
                const std::size_t off =
                    static_cast<std::size_t>(i) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(j);
                // 分数和都不够阈值，任何一个类别都不可能够，跳过（80 个类别不用逐个看）
                if (sum != nullptr && static_cast<int>(sum->data[off]) < sum_thr) {
                    continue;
                }
                int best = -1;
                int best_q = cls_thr;  // 要严格大于阈值
                for (int c = 0; c < cls.c; ++c) {
                    // int8 量化值本来就是有符号数，按有符号比较是对的
                    // NOLINTNEXTLINE(bugprone-signed-char-misuse)
                    const int q = static_cast<int>(cls.data[static_cast<std::size_t>(c) * plane + off]);
                    if (q > best_q) {
                        best_q = q;
                        best = c;
                    }
                }
                if (best < 0) {
                    continue;
                }
                for (std::size_t k = 0; k < logits.size(); ++k) {
                    logits[k] = dequant(box.data[k * plane + off], box.zp, box.scale);
                }
                // 4 条边依次是 左、上、右、下 到格子中心的距离
                const float l = dfl_expect(&logits[0], kDflBins);
                const float t = dfl_expect(&logits[kBins], kDflBins);
                const float r = dfl_expect(&logits[2 * kBins], kDflBins);
                const float d = dfl_expect(&logits[3 * kBins], kDflBins);
                const float cx = static_cast<float>(j) + 0.5F;
                const float cy = static_cast<float>(i) + 0.5F;
                Detection det;
                det.x1 = std::clamp((cx - l) * stride, 0.0F, static_cast<float>(model_w));
                det.y1 = std::clamp((cy - t) * stride, 0.0F, static_cast<float>(model_h));
                det.x2 = std::clamp((cx + r) * stride, 0.0F, static_cast<float>(model_w));
                det.y2 = std::clamp((cy + d) * stride, 0.0F, static_cast<float>(model_h));
                det.score = dequant(static_cast<std::int8_t>(best_q), cls.zp, cls.scale);
                det.cls = best;
                cand.push_back(det);
            }
        }
    }

    std::vector<Detection> kept = nms(std::move(cand), opts.nms_threshold);
    if (!opts.classes.empty()) {
        kept.erase(std::remove_if(kept.begin(), kept.end(),
                                  [&](const Detection &d) {
                                      return std::find(opts.classes.begin(), opts.classes.end(), d.cls) ==
                                             opts.classes.end();
                                  }),
                   kept.end());
    }
    if (static_cast<int>(kept.size()) > opts.max_detections) {
        kept.resize(static_cast<std::size_t>(std::max(opts.max_detections, 0)));
    }
    return kept;
}

float iou(const Detection &a, const Detection &b)
{
    const float w = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
    const float h = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    const float inter = w * h;
    const float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0 ? inter / uni : 0;
}

std::vector<Detection> nms(std::vector<Detection> dets, float threshold)
{
    std::stable_sort(dets.begin(), dets.end(),
                     [](const Detection &a, const Detection &b) { return a.score > b.score; });
    std::vector<Detection> out;
    std::vector<bool> removed(dets.size(), false);
    for (std::size_t i = 0; i < dets.size(); ++i) {
        if (removed[i]) {
            continue;
        }
        out.push_back(dets[i]);
        for (std::size_t k = i + 1; k < dets.size(); ++k) {
            if (!removed[k] && dets[k].cls == dets[i].cls && iou(dets[i], dets[k]) > threshold) {
                removed[k] = true;
            }
        }
    }
    return out;
}

void to_source(std::vector<Detection> &dets, const media::Letterbox &lb, int src_w, int src_h)
{
    const auto fw = static_cast<float>(src_w);
    const auto fh = static_cast<float>(src_h);
    for (Detection &d : dets) {
        d.x1 = std::clamp(static_cast<float>(lb.to_source_x(static_cast<double>(d.x1))), 0.0F, fw);
        d.y1 = std::clamp(static_cast<float>(lb.to_source_y(static_cast<double>(d.y1))), 0.0F, fh);
        d.x2 = std::clamp(static_cast<float>(lb.to_source_x(static_cast<double>(d.x2))), 0.0F, fw);
        d.y2 = std::clamp(static_cast<float>(lb.to_source_y(static_cast<double>(d.y2))), 0.0F, fh);
    }
}

}  // namespace rksec::detect
