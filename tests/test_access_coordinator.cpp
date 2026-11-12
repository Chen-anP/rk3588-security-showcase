#include "access/coordinator.hpp"
#include "core/config.hpp"
#include "ui/glyphs.hpp"
#include "ui/lvgl_port.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
using namespace rksec::access;
namespace {
Machine::Clock::time_point at(int ms)
{
    return Machine::Clock::time_point{} + std::chrono::milliseconds(ms);
}
Input frame(bool motion = false)
{
    Input i;
    i.motion = motion;
    i.present_tracks = std::vector<int>{1};
    return i;
}
FaceEvent face(Coordinator &c, int ms, std::uint64_t seq = 1)
{
    return {c.session(), seq, at(ms), 1, 42, true, true, 0.8};
}
}  // namespace
TEST(AccessCoordinator, PersistsBeforeReportingSimulatedOpen)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &e, std::uint64_t, std::string &) {
        ++calls;
        EXPECT_EQ(e.person_id, 42);
        return true;
    }};
    c.update(at(0), frame(true));
    EXPECT_FALSE(c.update(at(1), frame(), face(c, 1)).persisted);
    const auto result = c.update(at(2), frame(), face(c, 2, 2));
    EXPECT_TRUE(result.persisted);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.grants, 1U);
    c.update(at(3), frame(), face(c, 3, 3));
    EXPECT_EQ(calls, 1);
}
TEST(AccessCoordinator, FailedRecordNeverShowsSuccessAndCanRetryAfterCooldown)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &error) {
        ++calls;
        error = "disk full";
        return calls > 1;
    }};
    c.update(at(0), frame(true));
    c.update(at(1), frame(), face(c, 1));
    auto result = c.update(at(2), frame(), face(c, 2, 2));
    EXPECT_FALSE(result.persisted);
    EXPECT_EQ(result.grants, 0U);
    EXPECT_EQ(result.write_errors, 1U);
    EXPECT_EQ(status_text(result), "通行记录失败");
    c.update(at(8002), frame(true));
    c.update(at(8003), frame(), face(c, 8003));
    EXPECT_TRUE(c.update(at(8004), frame(), face(c, 8004, 2)).persisted);
    EXPECT_EQ(calls, 2);
}
TEST(AccessCoordinator, OldSessionPreTriggerAndRepeatedResultsCannotGrant)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++calls;
        return true;
    }};
    c.update(at(100), frame(true));
    auto e = face(c, 99);
    c.update(at(101), frame(), e);
    e = face(c, 102, 2);
    e.session = 0;
    c.update(at(102), frame(), e);
    e = face(c, 103, 3);
    c.update(at(103), frame(), e);
    c.update(at(104), frame(), e);
    EXPECT_EQ(calls, 0);
}
TEST(AccessCoordinator, MissingSequenceBreaksConfirmation)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++calls;
        return true;
    }};
    c.update(at(0), frame(true));
    c.update(at(1), frame(), face(c, 1));
    c.update(at(2), frame(), face(c, 2, 3));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(c.update(at(3), frame(), face(c, 3, 4)).persisted);
}
TEST(AccessCoordinator, StaleFrameMissingTrackAndUnknownIdCannotGrant)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++calls;
        return true;
    }};
    c.update(at(0), frame(true));
    auto e = face(c, 600);
    c.update(at(600), {}, e);
    e = face(c, 601, 2);
    e.person_id = 0;
    c.update(at(601), frame(), e);
    e = face(c, 602, 3);
    e.track = 2;
    c.update(at(602), frame(), e);
    EXPECT_EQ(calls, 0);
}
TEST(AccessCoordinator, StrangerAndQualityFailureDoNotWritePass)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++calls;
        return true;
    }};
    c.update(at(0), frame(true));
    auto e = face(c, 1);
    e.found = false;
    c.update(at(1), frame(), e);
    e = face(c, 2, 2);
    e.matched = false;
    c.update(at(2), frame(), e);
    e = face(c, 3, 3);
    e.matched = false;
    const auto result = c.update(at(3), frame(), e);
    EXPECT_EQ(result.state, State::denied);
    EXPECT_EQ(calls, 0);
}
TEST(AccessCoordinator, SinkExceptionIsAVisibleFailure)
{
    Coordinator c{[](const FaceEvent &, std::uint64_t, std::string &) -> bool { throw std::runtime_error("failure"); }};
    c.update(at(0), frame(true));
    c.update(at(1), frame(), face(c, 1));
    const auto result = c.update(at(2), frame(), face(c, 2, 2));
    EXPECT_FALSE(result.persisted);
    EXPECT_EQ(result.write_errors, 1U);
}
TEST(AccessCoordinator, AccessConfigDefaultsAndValidation)
{
    std::vector<rksec::ini::Error> errors;
    auto cfg = rksec::config::load("[camera]\ndevice=/dev/null\n", errors);
    ASSERT_TRUE(cfg);
    EXPECT_FALSE(cfg->access.enabled);
    cfg = rksec::config::load("[camera]\ndevice=/dev/null\n[access]\nenabled=true\n", errors);
    ASSERT_TRUE(cfg);
    EXPECT_TRUE(cfg->access.enabled);
    EXPECT_FALSE(
        rksec::config::load("[camera]\ndevice=/dev/null\n[access]\nenabled=true\n[face]\nrecognize=false\n", errors));
}
TEST(AccessCoordinator, StatusCharactersHaveDisplayGlyphs)
{
    rksec::ui::lvgl_init();
    for (const auto *text : {"待机", "识别中", "模拟开门", "陌生人", "冷却中", "通行记录失败", "记录处理中", "红外故障",
                             "门禁不可用", "识别不可用"}) {
        EXPECT_TRUE(rksec::ui::missing_name_glyphs(text).empty()) << text;
    }
    rksec::ui::lvgl_deinit();
}

TEST(AccessCoordinator, SlowCommittedRecordCannotShowExpiredOpen)
{
    Coordinator c{[](const FaceEvent &, std::uint64_t, std::string &) { return true; }, [] { return at(4000); }};
    c.update(at(0), frame(true));
    c.update(at(1), frame(), face(c, 1));
    const auto result = c.update(at(2), frame(), face(c, 2, 2));
    EXPECT_FALSE(result.persisted);
    EXPECT_NE(result.state, State::granted);
    EXPECT_EQ(result.grants, 1U);
}

TEST(AccessCoordinator, DetectionGapAndEmptyFrameBreakConfirmation)
{
    int calls = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++calls;
        return true;
    }};
    c.update(at(0), frame(true));
    c.update(at(1), frame(), face(c, 1));
    c.update(at(600));
    c.update(at(601), frame(), face(c, 601, 2));
    EXPECT_EQ(calls, 0);
    Input empty;
    empty.present_tracks = std::vector<int>{};
    c.update(at(602), empty);
    c.update(at(603), frame(), face(c, 603, 3));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(c.update(at(604), frame(), face(c, 604, 4)).persisted);
}

TEST(AccessCoordinator, HeldHighRetriesAfterTimeoutAndStillDeduplicates)
{
    int writes = 0;
    Coordinator c{[&](const FaceEvent &, std::uint64_t, std::string &) {
        ++writes;
        return true;
    }};
    c.update(at(0), frame(true));
    c.update(at(10000), frame());
    c.update(at(15000), frame());
    auto retry = frame(trigger_motion(false, true, c.update(at(15000)).state));
    EXPECT_EQ(c.update(at(15001), retry).state, State::identifying);
    c.update(at(15002), frame(), face(c, 15002));
    c.update(at(15003), frame(), face(c, 15003, 2));
    EXPECT_EQ(writes, 1);
    auto idle = c.update(at(23003), frame());
    retry = frame(trigger_motion(false, true, idle.state));
    c.update(at(23004), retry);
    c.update(at(23005), frame(), face(c, 23005));
    c.update(at(23006), frame(), face(c, 23006, 2));
    EXPECT_EQ(writes, 1);
    EXPECT_FALSE(trigger_motion(false, true, State::identifying));
    EXPECT_FALSE(trigger_motion(false, true, State::cooldown));
    EXPECT_FALSE(trigger_motion(false, false, State::idle));
}
