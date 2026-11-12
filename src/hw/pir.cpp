#include "hw/pir.hpp"
#include <cerrno>
#include <cstring>
#include <gpiod.h>
namespace rksec::hw {
namespace {
class GpioInput final : public PirInput {
  public:
    GpioInput(gpiod_chip *chip, gpiod_line *line) : chip_(chip), line_(line) {}
    ~GpioInput() override
    {
        gpiod_line_release(line_);
        gpiod_chip_close(chip_);
    }
    GpioInput(const GpioInput &) = delete;
    GpioInput &operator=(const GpioInput &) = delete;
    InputResult initial() override
    {
        const int value = gpiod_line_get_value(line_);
        if (value < 0) {
            return {InputStatus::error, false, std::strerror(errno)};
        }
        return {InputStatus::level, value != 0, {}};
    }
    InputResult wait(std::chrono::milliseconds timeout) override
    {
        if (timeout.count() < 0) {
            return {InputStatus::error, false, "negative timeout"};
        }
        const timespec ts{static_cast<time_t>(timeout.count() / 1000),
                          static_cast<long>((timeout.count() % 1000) * 1000000)};
        const int ready = gpiod_line_event_wait(line_, &ts);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            return {InputStatus::timeout, false, {}};
        }
        if (ready < 0) {
            return {InputStatus::error, false, std::strerror(errno)};
        }
        gpiod_line_event event{};
        if (gpiod_line_event_read(line_, &event) < 0) {
            return {InputStatus::error, false, std::strerror(errno)};
        }
        return {InputStatus::level, event.event_type == GPIOD_LINE_EVENT_RISING_EDGE, {}};
    }

  private:
    gpiod_chip *chip_;
    gpiod_line *line_;
};
}  // namespace
std::unique_ptr<PirInput> open_pir(const PirOptions &options, std::string &error)
{
    error.clear();
    if (options.line < 0 || options.chip.empty()) {
        error = "invalid GPIO options";
        return {};
    }
    auto *chip = gpiod_chip_open(options.chip.c_str());
    if (chip == nullptr) {
        error = std::strerror(errno);
        return {};
    }
    auto *line = gpiod_chip_get_line(chip, static_cast<unsigned int>(options.line));
    if (line == nullptr || gpiod_line_request_both_edges_events(line, "rksec-pir") < 0) {
        error = std::strerror(errno);
        gpiod_chip_close(chip);
        return {};
    }
    return std::make_unique<GpioInput>(chip, line);
}
bool PirState::update(const InputResult &result, bool active_high)
{
    if (result.status == InputStatus::timeout) {
        return false;
    }
    if (result.status == InputStatus::error) {
        const bool changed = known;
        known = false;
        return changed;
    }
    const bool next = result.high == active_high;
    const bool changed = !known || next != active;
    if (known && changed) {
        if (next) {
            ++triggers;
        } else {
            ++restores;
        }
    }
    known = true;
    active = next;
    return changed;
}
}  // namespace rksec::hw
