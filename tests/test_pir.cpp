#include "core/config.hpp"
#include "hw/pir.hpp"
#include <gtest/gtest.h>
using namespace rksec::hw;
TEST(PirState, InitialLevelIsNotAnEdge)
{
    PirState s;
    EXPECT_TRUE(s.update({InputStatus::level, true, {}}, true));
    EXPECT_TRUE(s.known);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.triggers, 0U);
    EXPECT_FALSE(s.update({InputStatus::level, true, {}}, true));
}
TEST(PirState, MotionRestoreAndTimeout)
{
    PirState s;
    s.update({InputStatus::level, false, {}}, true);
    EXPECT_TRUE(s.update({InputStatus::level, true, {}}, true));
    EXPECT_FALSE(s.update({InputStatus::timeout, false, {}}, true));
    EXPECT_TRUE(s.active);
    EXPECT_TRUE(s.update({InputStatus::level, false, {}}, true));
    EXPECT_EQ(s.triggers, 1U);
    EXPECT_EQ(s.restores, 1U);
}
TEST(PirState, FailureMakesStateUnknown)
{
    PirState s;
    s.update({InputStatus::level, true, {}}, true);
    EXPECT_TRUE(s.update({InputStatus::error, false, "disconnected"}, true));
    EXPECT_FALSE(s.known);
    EXPECT_EQ(s.restores, 0U);
}
TEST(PirState, ActiveLowInput)
{
    PirState s;
    s.update({InputStatus::level, true, {}}, false);
    s.update({InputStatus::level, false, {}}, false);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.triggers, 1U);
}
TEST(PirInput, InvalidAndMissingDevicesFail)
{
    std::string error;
    EXPECT_FALSE(open_pir({"", -1, true}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(open_pir({"/dev/rksec-nonexistent-chip", 7, true}, error));
    EXPECT_FALSE(error.empty());
}
TEST(PirConfig, DefaultsAndValidation)
{
    std::vector<rksec::ini::Error> errors;
    auto cfg = rksec::config::load("[camera]\ndevice=/dev/video0\n[pir]\nchip=/dev/gpiochip1\nline=7\nactive_level=0\n",
                                   errors);
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->pir.active_level, 0);
    EXPECT_FALSE(
        rksec::config::load("[camera]\ndevice=/dev/video0\n[pir]\nchip=/tmp/a\nline=-1\nactive_level=2\n", errors));
}

namespace {
class FakePir final : public PirInput {
  public:
    InputResult initial() override { return {InputStatus::level, false, {}}; }
    InputResult wait(std::chrono::milliseconds) override
    {
        ++calls;
        if (calls == 1) {
            return {InputStatus::level, true, {}};
        }
        if (calls == 2) {
            return {InputStatus::timeout, false, {}};
        }
        if (calls == 3) {
            return {InputStatus::level, false, {}};
        }
        return {InputStatus::error, false, "device removed"};
    }
    int calls = 0;
};
}  // namespace
TEST(PirInput, FakeSequencePreservesMotionAcrossTimeoutAndReportsFailure)
{
    FakePir input;
    PirState state;
    state.update(input.initial(), true);
    state.update(input.wait(std::chrono::milliseconds(1)), true);
    EXPECT_TRUE(state.active);
    state.update(input.wait(std::chrono::milliseconds(1)), true);
    EXPECT_TRUE(state.active);
    state.update(input.wait(std::chrono::milliseconds(1)), true);
    EXPECT_FALSE(state.active);
    state.update(input.wait(std::chrono::milliseconds(1)), true);
    EXPECT_FALSE(state.known);
    EXPECT_EQ(state.triggers, 1U);
    EXPECT_EQ(state.restores, 1U);
}
