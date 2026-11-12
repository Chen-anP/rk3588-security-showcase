#pragma once

#include <string>

// RTSP 的账号密码（第 8c 步）：从单独的文件读，不写在配置文件里、不进 git。纯逻辑，CI 上测。
//
// 文件格式：一行 “用户名:密码”；# 开头的行和空行忽略。
// 规则（不满足就不开 RTSP，日志写明原因——宁可没有实时流，也不要一个谁都能看的流）：
// - 文件权限只能是所有者可读写（0600 或 0400）：别的用户能读到，密码就不算密码了
// - 用户名 1～32 个字符，只能是字母、数字和 _ . -
// - 密码至少 8 个字符，不能有空白
// - 只能有一行账号
namespace rksec::stream {

struct Credentials {
    std::string user;
    std::string password;
};

bool load_credentials(const std::string &path, Credentials &out, std::string &err);

// 不读文件，只检查内容（测试用，也给 load_credentials 用）
bool parse_credentials(const std::string &text, Credentials &out, std::string &err);

}  // namespace rksec::stream
