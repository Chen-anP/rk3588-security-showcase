#include "core/ini.hpp"

#include <cctype>
#include <utility>

namespace rksec::ini {

namespace {

std::string_view trim(std::string_view s)
{
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_name(std::string_view s)
{
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace

Document Document::parse(std::string_view text, std::vector<Error> &errors)
{
    Document doc;
    std::string section;  // 顶层键放在 "" 分区
    int line_no = 0;

    while (!text.empty()) {
        ++line_no;
        const auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = (nl == std::string_view::npos) ? std::string_view{} : text.substr(nl + 1);

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']') {
                errors.push_back({line_no, "分区行缺少 ]"});
                continue;
            }
            const auto name = trim(line.substr(1, line.size() - 2));
            if (!is_name(name)) {
                errors.push_back({line_no, "分区名只能用字母、数字、下划线: [" + std::string{name} + "]"});
                continue;
            }
            section = std::string{name};
            doc.sections_[section];  // 空分区也记下来
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            errors.push_back({line_no, "不是“键 = 值”格式: " + std::string{line}});
            continue;
        }
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));
        if (!is_name(key)) {
            errors.push_back({line_no, "键名只能用字母、数字、下划线: " + std::string{key}});
            continue;
        }

        auto &entries = doc.sections_[section];
        const auto [it, inserted] = entries.try_emplace(std::string{key}, Entry{Value{std::string{value}, line_no}});
        if (!inserted) {
            errors.push_back({line_no, "重复的配置项 " + section + "." + std::string{key} + "（第 " +
                                           std::to_string(it->second.value.line) + " 行已经出现过）"});
        }
    }
    return doc;
}

const Value *Document::find(std::string_view section, std::string_view key) const
{
    const auto s = sections_.find(section);
    if (s == sections_.end()) {
        return nullptr;
    }
    const auto k = s->second.find(key);
    if (k == s->second.end()) {
        return nullptr;
    }
    k->second.used = true;
    return &k->second.value;
}

std::vector<std::pair<std::string, const Value *>> Document::unused() const
{
    std::vector<std::pair<std::string, const Value *>> out;
    for (const auto &[section, entries] : sections_) {
        for (const auto &[key, entry] : entries) {
            if (!entry.used) {
                std::string name = section;
                name += '.';
                name += key;
                out.emplace_back(std::move(name), &entry.value);
            }
        }
    }
    return out;
}

}  // namespace rksec::ini
