#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 极简 INI 解析：
//   [section]
//   key = value
// - 注释只能单独成行，以 # 或 ; 开头（值里可以出现 #，比如路径）
// - 键、分区名只能用字母、数字、下划线
// - 同一分区里重复的键算错误
namespace rksec::ini {

struct Error {
    int line = 0;  // 从 1 开始；0 表示和具体行无关
    std::string message;
};

struct Value {
    std::string text;
    int line = 0;
};

class Document {
  public:
    // 解析整段文本。出错的行记进 errors，其余行照常解析，这样一次能报出全部问题
    static Document parse(std::string_view text, std::vector<Error> &errors);

    // 查找值；找到后把这个键标记为“已使用”。找不到返回 nullptr
    const Value *find(std::string_view section, std::string_view key) const;

    // 从未被 find 访问过的键（"section.key", 值），用来发现拼错的配置项
    std::vector<std::pair<std::string, const Value *>> unused() const;

  private:
    struct Entry {
        Value value;
        mutable bool used = false;
    };
    std::map<std::string, std::map<std::string, Entry, std::less<>>, std::less<>> sections_;
};

}  // namespace rksec::ini
