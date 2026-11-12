#include "access/access_machine.hpp"
#include <gtest/gtest.h>
using namespace rksec::access;
namespace {
Machine::Clock::time_point at(int ms)
{
    return Machine::Clock::time_point{} + std::chrono::milliseconds(ms);
}
Input motion()
{
    Input i;
    i.motion = true;
    return i;
}
Input sample(Machine &m, std::uint64_t seq, Verdict verdict = Verdict::known, std::string key = "person-a",
             int track = 1)
{
    Input i;
    i.sample = Sample{m.session(), seq, track, verdict, std::move(key)};
    return i;
}
void grant(Machine &m, int start = 0)
{
    m.update(at(start), motion());
    m.update(at(start + 1), sample(m, 1));
    EXPECT_EQ(m.update(at(start + 2), sample(m, 2)).action, Action::grant);
}
}  // namespace
TEST(AccessMachine, NeedsFreshConsecutiveSamplesAndEmitsGrantOnce)
{
    Machine m;
    EXPECT_EQ(m.update(at(0), motion()).state, State::identifying);
    EXPECT_EQ(m.update(at(1), sample(m, 1)).action, Action::none);
    EXPECT_EQ(m.update(at(2), sample(m, 1)).action, Action::none);
    EXPECT_EQ(m.update(at(3), sample(m, 2)).action, Action::grant);
    EXPECT_EQ(m.update(at(4), sample(m, 3)).action, Action::none);
}
TEST(AccessMachine, NoFaceAndDifferentPeopleBreakStreak)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(1), sample(m, 1));
    m.update(at(2), sample(m, 2, Verdict::no_face));
    EXPECT_EQ(m.update(at(3), sample(m, 3)).action, Action::none);
    EXPECT_EQ(m.update(at(4), sample(m, 4, Verdict::known, "person-b")).action, Action::none);
    EXPECT_EQ(m.update(at(5), sample(m, 5, Verdict::known, "person-b")).action, Action::grant);
}
TEST(AccessMachine, TracksCannotShareAConfirmationStreak)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(1), sample(m, 1));
    EXPECT_EQ(m.update(at(2), sample(m, 2, Verdict::known, "person-a", 2)).action, Action::none);
}
TEST(AccessMachine, NoFaceTimesOutWithoutStrangerAction)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(9999), sample(m, 1, Verdict::no_face));
    EXPECT_EQ(m.update(at(10000), motion()).action, Action::timeout);
    EXPECT_EQ(m.update(at(10001)).action, Action::none);
}
TEST(AccessMachine, StrangerRequiresConfirmationAndCannotGrant)
{
    Machine m;
    m.update(at(0), motion());
    EXPECT_EQ(m.update(at(1), sample(m, 1, Verdict::stranger, "")).action, Action::none);
    auto r = m.update(at(2), sample(m, 2, Verdict::stranger, ""));
    EXPECT_EQ(r.action, Action::deny);
    EXPECT_EQ(r.state, State::denied);
    EXPECT_TRUE(r.person.empty());
}
TEST(AccessMachine, CooldownAndSessionRejectOldResults)
{
    Machine m;
    grant(m);
    EXPECT_EQ(m.update(at(3002), motion()).state, State::cooldown);
    auto old = sample(m, 10);
    EXPECT_EQ(m.update(at(8002), motion()).state, State::identifying);
    EXPECT_EQ(m.update(at(8003), old).action, Action::none);
}
TEST(AccessMachine, SamePersonWithNewTrackIsSuppressed)
{
    Machine m;
    grant(m);
    Input visible;
    visible.present_tracks = std::vector<int>{1};
    m.update(at(8000), visible);
    m.update(at(8002), motion());
    m.update(at(8003), sample(m, 1, Verdict::known, "person-a", 2));
    EXPECT_EQ(m.update(at(8004), sample(m, 2, Verdict::known, "person-a", 2)).action, Action::suppressed);
}
TEST(AccessMachine, ConfirmedAbsenceAllowsReturnButTimerAloneDoesNot)
{
    Machine m;
    grant(m);
    m.update(at(20000));
    EXPECT_EQ(m.suppressed_people(), 1U);
    Input absent;
    absent.present_tracks = std::vector<int>{};
    m.update(at(20001), absent);
    EXPECT_EQ(m.suppressed_people(), 1U);
    m.update(at(25001), absent);
    EXPECT_EQ(m.suppressed_people(), 0U);
    grant(m, 25002);
}
TEST(AccessMachine, PirFailureUsesCameraPresenceAndDoesNotDeny)
{
    Machine m;
    Input i;
    i.pir_available = false;
    i.present_tracks = std::vector<int>{7};
    auto r = m.update(at(0), i);
    EXPECT_TRUE(r.pir_degraded);
    EXPECT_EQ(r.state, State::identifying);
    EXPECT_EQ(r.action, Action::none);
}
TEST(AccessMachine, InvalidOptionsAndBackwardsTimeAreSafe)
{
    Options o;
    o.confirmations = 1;
    EXPECT_THROW(Machine{o}, std::invalid_argument);
    Machine m;
    m.update(at(100), motion());
    EXPECT_EQ(m.update(at(99), sample(m, 1)).action, Action::none);
    EXPECT_EQ(m.update(at(101), sample(m, 1)).action, Action::none);
    EXPECT_EQ(m.update(at(102), sample(m, 2)).action, Action::grant);
}
TEST(AccessMachine, EmptyPersonKeyCannotGrant)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(1), sample(m, 1, Verdict::known, ""));
    EXPECT_EQ(m.update(at(2), sample(m, 2, Verdict::known, "")).action, Action::none);
}

TEST(AccessMachine, CapacityIsBoundedAndFailsClosed)
{
    Machine m;
    for (std::size_t n = 0; n <= Machine::kMaxPeople; ++n) {
        const int start = static_cast<int>(n) * 9000;
        m.update(at(start), motion());
        const auto key = "person-" + std::to_string(n);
        m.update(at(start + 1), sample(m, 1, Verdict::known, key));
        const auto result = m.update(at(start + 2), sample(m, 2, Verdict::known, key));
        EXPECT_EQ(result.action, n < Machine::kMaxPeople ? Action::grant : Action::capacity);
        EXPECT_LE(m.suppressed_people(), Machine::kMaxPeople);
    }
}
TEST(AccessMachine, OversizedPersonKeyCannotGrant)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(1), sample(m, 1, Verdict::known, std::string(129, 'x')));
    EXPECT_EQ(m.update(at(2), sample(m, 2, Verdict::known, std::string(129, 'x'))).action, Action::none);
}

TEST(AccessMachine, DeadlineWinsOverASecondRecognition)
{
    Machine m;
    m.update(at(0), motion());
    m.update(at(9999), sample(m, 1));
    EXPECT_EQ(m.update(at(10000), sample(m, 2)).action, Action::timeout);
}
TEST(AccessMachine, PirFailureWithoutPeopleKeepsIdle)
{
    Machine m;
    Input i;
    i.pir_available = false;
    i.present_tracks = std::vector<int>{};
    const auto r = m.update(at(0), i);
    EXPECT_EQ(r.state, State::idle);
    EXPECT_EQ(r.action, Action::none);
    EXPECT_TRUE(r.pir_degraded);
}

TEST(AccessMachine, TrackReplacementWithoutFaceCannotRearmPerson)
{
    Machine m;
    grant(m);
    Input different_track;
    different_track.present_tracks = std::vector<int>{2};
    m.update(at(9000), different_track);
    m.update(at(20000), different_track);
    EXPECT_EQ(m.suppressed_people(), 1U);
    m.update(at(20001), motion());
    m.update(at(20002), sample(m, 1, Verdict::known, "person-a", 2));
    EXPECT_EQ(m.update(at(20003), sample(m, 2, Verdict::known, "person-a", 2)).action, Action::suppressed);
}
