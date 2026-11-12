#pragma once

#include <string>
#include <string_view>

namespace rksec::version {

// 语义化版本号，来自 CMakeLists.txt 的 project(VERSION)，如 "0.0.1"
std::string_view semver() noexcept;

// git describe --tags --dirty --always，如 "v0.0.1-5-g9aa4471-dirty"；
// 不在 git 仓库里编译时为 "unknown"
std::string_view git_describe() noexcept;

// CMAKE_BUILD_TYPE，如 "RelWithDebInfo"
std::string_view build_type() noexcept;

// 编译器，如 "GNU 11.4.0"
std::string_view compiler() noexcept;

// 一行完整版本信息，给 --version、日志、MQTT 上报用
std::string full();

}  // namespace rksec::version
