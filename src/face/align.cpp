#include "face/align.hpp"

#include <algorithm>
#include <cmath>

namespace rksec::face {

float Similarity::scale() const
{
    return std::sqrt(a * a + b * b);
}

bool estimate_similarity(const std::array<Point, 5> &src, const std::array<Point, 5> &dst, Similarity &out)
{
    // 两组点各自减去重心，问题变成只求旋转 + 缩放：把点看成复数，dst ≈ (a + ib)·src，
    // 最小二乘解 (a + ib) = Σ conj(src)·dst / Σ|src|²。用 double 算，避免 float 累加误差
    double sx = 0;
    double sy = 0;
    double dx = 0;
    double dy = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        sx += static_cast<double>(src[i].x);
        sy += static_cast<double>(src[i].y);
        dx += static_cast<double>(dst[i].x);
        dy += static_cast<double>(dst[i].y);
    }
    const auto n = static_cast<double>(src.size());
    sx /= n;
    sy /= n;
    dx /= n;
    dy /= n;
    double norm = 0;
    double re = 0;
    double im = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const double px = static_cast<double>(src[i].x) - sx;
        const double py = static_cast<double>(src[i].y) - sy;
        const double qx = static_cast<double>(dst[i].x) - dx;
        const double qy = static_cast<double>(dst[i].y) - dy;
        norm += px * px + py * py;
        re += px * qx + py * qy;
        im += px * qy - py * qx;
    }
    if (norm < 1e-6) {
        return false;
    }
    const double a = re / norm;
    const double b = im / norm;
    out.a = static_cast<float>(a);
    out.b = static_cast<float>(b);
    out.tx = static_cast<float>(dx - (a * sx - b * sy));
    out.ty = static_cast<float>(dy - (b * sx + a * sy));
    return true;
}

namespace {

// 源图 (x, y) 处的 R、G、B；在图外返回 false（按 0 处理）
inline bool pixel(const ImageView &s, int x, int y, std::array<float, 3> &rgb)
{
    if (x < 0 || y < 0 || x >= s.width || y >= s.height) {
        return false;
    }
    const std::uint8_t *p = s.data + static_cast<std::size_t>(y) * s.stride;
    if (s.format == PixelFormat::rgb888) {
        p += static_cast<std::size_t>(x) * 3;
        rgb = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    } else {
        p += static_cast<std::size_t>(x) * 4;
        rgb = {static_cast<float>(p[2]), static_cast<float>(p[1]), static_cast<float>(p[0])};
    }
    return true;
}

}  // namespace

void warp_to_rgb(const ImageView &src, const Similarity &m, std::uint8_t *dst, int dst_w, int dst_h)
{
    // 反变换：src = R⁻¹ · (dst − t)，R⁻¹ = [a b; −b a] / (a² + b²)
    const float k = m.a * m.a + m.b * m.b;
    const float ia = k > 0 ? m.a / k : 0;
    const float ib = k > 0 ? m.b / k : 0;
    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            const float qx = static_cast<float>(x) - m.tx;
            const float qy = static_cast<float>(y) - m.ty;
            const float fx = ia * qx + ib * qy;
            const float fy = -ib * qx + ia * qy;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float wx = fx - static_cast<float>(x0);
            const float wy = fy - static_cast<float>(y0);
            std::array<float, 3> acc{};
            const std::array<std::array<int, 2>, 4> nb = {{{x0, y0}, {x0 + 1, y0}, {x0, y0 + 1}, {x0 + 1, y0 + 1}}};
            const std::array<float, 4> w = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
            for (std::size_t i = 0; i < nb.size(); ++i) {
                std::array<float, 3> c{};
                if (w[i] > 0 && pixel(src, nb[i][0], nb[i][1], c)) {
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        acc[ch] += w[i] * c[ch];
                    }
                }
            }
            std::uint8_t *o =
                dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w) + static_cast<std::size_t>(x)) * 3;
            for (std::size_t ch = 0; ch < 3; ++ch) {
                o[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(acc[ch]), 0L, 255L));
            }
        }
    }
}

bool align_face(const ImageView &src, const std::array<Point, 5> &landmarks, std::uint8_t *dst)
{
    Similarity m;
    if (!estimate_similarity(landmarks, kArcFaceRef, m)) {
        return false;
    }
    warp_to_rgb(src, m, dst, kAlignedSize, kAlignedSize);
    return true;
}

Region face_search_region(const detect::Detection &person, int src_w, int src_h)
{
    const float bw = person.x2 - person.x1;
    const float bh = person.y2 - person.y1;
    const float top_margin = 0.05F * bh;
    float w = std::max(bw * 1.1F, static_cast<float>(kMinRegion));
    float h = std::min(w, bh + top_margin);
    h = std::max(h, static_cast<float>(kMinRegion));
    w = std::min(w, static_cast<float>(src_w));
    h = std::min(h, static_cast<float>(src_h));
    float x = (person.x1 + person.x2) / 2 - w / 2;
    float y = person.y1 - top_margin;
    x = std::clamp(x, 0.0F, static_cast<float>(src_w) - w);
    y = std::clamp(y, 0.0F, static_cast<float>(src_h) - h);
    // 都取偶数：起点向下取偶，宽高向下取偶，保证不出画面
    const auto even = [](int v) { return v / 2 * 2; };
    Region r;
    r.x = even(static_cast<int>(x));
    r.y = even(static_cast<int>(y));
    r.width = even(std::min(static_cast<int>(w), src_w - r.x));
    r.height = even(std::min(static_cast<int>(h), src_h - r.y));
    r.person = person;
    return r;
}

}  // namespace rksec::face
