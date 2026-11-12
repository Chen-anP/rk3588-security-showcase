#include "face/gallery.hpp"

#include <cmath>
#include <map>

namespace rksec::face {

float l2_normalize(std::vector<float> &v)
{
    double sum = 0;
    for (const float x : v) {
        sum += static_cast<double>(x) * static_cast<double>(x);
    }
    const double n = std::sqrt(sum);
    if (n <= 0) {
        return 0;
    }
    for (float &x : v) {
        x = static_cast<float>(static_cast<double>(x) / n);
    }
    return static_cast<float>(n);
}

float cosine(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size() || a.empty()) {
        return -1;
    }
    double dot = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return static_cast<float>(dot);
}

void Gallery::add(const std::string &name, std::vector<float> embedding, std::int64_t person_id)
{
    l2_normalize(embedding);
    entries_.push_back({name, std::move(embedding), person_id});
}

std::map<std::string, float> Gallery::scores(const std::vector<float> &embedding) const
{
    std::map<std::string, float> best;
    for (const Entry &e : entries_) {
        const float s = cosine(embedding, e.embedding);
        auto [it, inserted] = best.try_emplace(e.name, s);
        if (!inserted && s > it->second) {
            it->second = s;
        }
    }
    return best;
}

Match Gallery::match(const std::vector<float> &embedding, float threshold) const
{
    // 每个人取他所有照片里最高的分数，再在人之间比
    Match m;
    for (const auto &[name, s] : scores(embedding)) {
        if (s > m.score) {
            m.second = m.score;
            m.score = s;
            m.name = name;
        } else if (s > m.second) {
            m.second = s;
        }
    }
    for (const auto &entry : entries_) {
        if (entry.name == m.name && entry.person_id > 0) {
            m.person_id = entry.person_id;
            break;
        }
    }
    m.matched = !m.name.empty() && m.score >= threshold;
    return m;
}

std::size_t Gallery::people() const
{
    std::map<std::string, int> names;
    for (const Entry &e : entries_) {
        names[e.name] = 1;
    }
    return names.size();
}

std::string name_from_file(const std::string &path)
{
    std::string s = path;
    if (const auto slash = s.find_last_of('/'); slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    if (const auto dot = s.find_last_of('.'); dot != std::string::npos && dot > 0) {
        s = s.substr(0, dot);
    }
    // 结尾的 “_数字” 是同一个人的第几张照片
    if (const auto us = s.find_last_of('_'); us != std::string::npos && us > 0 && us + 1 < s.size()) {
        bool digits = true;
        for (std::size_t i = us + 1; i < s.size(); ++i) {
            digits = digits && s[i] >= '0' && s[i] <= '9';
        }
        if (digits) {
            s = s.substr(0, us);
        }
    }
    return s;
}

}  // namespace rksec::face
