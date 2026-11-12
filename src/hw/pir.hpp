#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
namespace rksec::hw {
struct PirOptions {
    std::string chip = "/dev/gpiochip1";
    int line = 7;
    bool active_high = true;
};
enum class InputStatus : std::uint8_t { level, timeout, error };
struct InputResult {
    InputStatus status = InputStatus::error;
    bool high = false;
    std::string error;
};
// 后端持续持有输入；初始读值与边沿读取共用同一个申请。
class PirInput {
  public:
    virtual ~PirInput() = default;
    virtual InputResult initial() = 0;
    virtual InputResult wait(std::chrono::milliseconds timeout) = 0;
};
std::unique_ptr<PirInput> open_pir(const PirOptions &options, std::string &error);
struct PirState {
    bool known = false;
    bool active = false;
    std::uint64_t triggers = 0;
    std::uint64_t restores = 0;
    // 返回状态是否改变；错误使状态未知，超时保留已知状态。
    bool update(const InputResult &result, bool active_high);
};
}  // namespace rksec::hw
