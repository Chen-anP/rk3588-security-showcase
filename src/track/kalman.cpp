#include "track/kalman.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

namespace rksec::track {

namespace {

constexpr double kStdPosition = 1.0 / 20;   // 位置噪声 = 框高 × 1/20
constexpr double kStdVelocity = 1.0 / 160;  // 速度噪声 = 框高 × 1/160

constexpr std::size_t at(std::size_t r, std::size_t c)
{
    return r * 8 + c;
}

// 4×4 矩阵求逆（高斯-约当消元，带选主元）。S 是正定的，不会奇异
std::array<double, 16> inverse4(std::array<double, 16> m)
{
    std::array<double, 16> inv{};
    for (std::size_t i = 0; i < 4; ++i) {
        inv[i * 4 + i] = 1;
    }
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 4; ++r) {
            if (std::fabs(m[r * 4 + col]) > std::fabs(m[pivot * 4 + col])) {
                pivot = r;
            }
        }
        if (pivot != col) {
            for (std::size_t c = 0; c < 4; ++c) {
                std::swap(m[col * 4 + c], m[pivot * 4 + c]);
                std::swap(inv[col * 4 + c], inv[pivot * 4 + c]);
            }
        }
        const double d = m[col * 4 + col];
        for (std::size_t c = 0; c < 4; ++c) {
            m[col * 4 + c] /= d;
            inv[col * 4 + c] /= d;
        }
        for (std::size_t r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const double f = m[r * 4 + col];
            for (std::size_t c = 0; c < 4; ++c) {
                m[r * 4 + c] -= f * m[col * 4 + c];
                inv[r * 4 + c] -= f * inv[col * 4 + c];
            }
        }
    }
    return inv;
}

}  // namespace

KalmanFilter::KalmanFilter(const Box &first)
{
    x_ = {first.cx, first.cy, first.a, first.h, 0, 0, 0, 0};
    const double h = first.h;
    const std::array<double, 8> std_dev = {2 * kStdPosition * h,  2 * kStdPosition * h,  1e-2, 2 * kStdPosition * h,
                                           10 * kStdVelocity * h, 10 * kStdVelocity * h, 1e-5, 10 * kStdVelocity * h};
    for (std::size_t i = 0; i < 8; ++i) {
        p_[at(i, i)] = std_dev[i] * std_dev[i];
    }
}

void KalmanFilter::predict()
{
    for (std::size_t i = 0; i < 4; ++i) {
        x_[i] += x_[i + 4];
    }
    // P = F P Fᵀ + Q，F = [I I; 0 I]
    std::array<double, 64> a{};
    for (std::size_t r = 0; r < 8; ++r) {
        for (std::size_t c = 0; c < 8; ++c) {
            a[at(r, c)] = p_[at(r, c)] + (r < 4 ? p_[at(r + 4, c)] : 0);
        }
    }
    for (std::size_t r = 0; r < 8; ++r) {
        for (std::size_t c = 0; c < 8; ++c) {
            p_[at(r, c)] = a[at(r, c)] + (c < 4 ? a[at(r, c + 4)] : 0);
        }
    }
    const double h = x_[3];
    const std::array<double, 8> q = {kStdPosition * h, kStdPosition * h, 1e-2, kStdPosition * h,
                                     kStdVelocity * h, kStdVelocity * h, 1e-5, kStdVelocity * h};
    for (std::size_t i = 0; i < 8; ++i) {
        p_[at(i, i)] += q[i] * q[i];
    }
}

void KalmanFilter::update(const Box &meas)
{
    const double h = x_[3];
    const std::array<double, 4> r = {kStdPosition * h, kStdPosition * h, 1e-1, kStdPosition * h};
    // S = H P Hᵀ + R（P 的左上 4×4）
    std::array<double, 16> s{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            s[i * 4 + j] = p_[at(i, j)];
        }
        s[i * 4 + i] += r[i] * r[i];
    }
    const std::array<double, 16> si = inverse4(s);
    // K = P Hᵀ S⁻¹（8×4）；P Hᵀ 就是 P 的前 4 列
    std::array<double, 32> k{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            double v = 0;
            for (std::size_t m = 0; m < 4; ++m) {
                v += p_[at(i, m)] * si[m * 4 + j];
            }
            k[i * 4 + j] = v;
        }
    }
    const std::array<double, 4> y = {meas.cx - x_[0], meas.cy - x_[1], meas.a - x_[2], meas.h - x_[3]};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            x_[i] += k[i * 4 + j] * y[j];
        }
    }
    // P = P − K (P Hᵀ)ᵀ
    std::array<double, 64> np = p_;
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 8; ++j) {
            double v = 0;
            for (std::size_t m = 0; m < 4; ++m) {
                v += k[i * 4 + m] * p_[at(j, m)];
            }
            np[at(i, j)] -= v;
        }
    }
    p_ = np;
}

Box KalmanFilter::box() const
{
    return {x_[0], x_[1], x_[2], x_[3]};
}

}  // namespace rksec::track
