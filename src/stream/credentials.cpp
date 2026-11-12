#include "stream/credentials.hpp"

#include <sys/stat.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace rksec::stream {

namespace {

constexpr std::size_t kMaxUser = 32;
constexpr std::size_t kMinPassword = 8;

bool user_ok(const std::string &u)
{
    if (u.empty() || u.size() > kMaxUser) {
        return false;
    }
    for (const char ch : u) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) == 0 && c != '_' && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace

bool parse_credentials(const std::string &text, Credentials &out, std::string &err)
{
    std::istringstream in(text);
    std::string line;
    int found = 0;
    Credentials c;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (++found > 1) {
            err = "只能有一行账号（用户名:密码）";
            return false;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            err = "格式应为 用户名:密码";
            return false;
        }
        c.user = line.substr(0, colon);
        c.password = line.substr(colon + 1);
    }
    if (found == 0) {
        err = "文件里没有账号（用户名:密码）";
        return false;
    }
    if (!user_ok(c.user)) {
        err = "用户名要 1～32 个字符，只能有字母、数字和 _ . -";
        return false;
    }
    if (c.password.size() < kMinPassword) {
        err = "密码至少 " + std::to_string(kMinPassword) + " 个字符";
        return false;
    }
    for (const char ch : c.password) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            err = "密码里不能有空白";
            return false;
        }
    }
    out = c;
    return true;
}

bool load_credentials(const std::string &path, Credentials &out, std::string &err)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        err = "RTSP 账号文件 " + path + " 不存在";
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = "RTSP 账号文件 " + path + " 不是普通文件";
        return false;
    }
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        err = "RTSP 账号文件 " + path + " 别的用户也能读（权限要改成 600：chmod 600 " + path + "）";
        return false;
    }
    std::ifstream f(path);
    if (!f) {
        err = "RTSP 账号文件 " + path + " 打不开";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!parse_credentials(ss.str(), out, err)) {
        err = "RTSP 账号文件 " + path + "：" + err;
        return false;
    }
    return true;
}

}  // namespace rksec::stream
