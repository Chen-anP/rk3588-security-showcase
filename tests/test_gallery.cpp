// 人脸特征比对和身份投票（第 7c 步）。
#include <gtest/gtest.h>

#include <vector>

#include "face/gallery.hpp"
#include "face/identity.hpp"

using rksec::face::Gallery;
using rksec::face::IdentityBook;
using rksec::face::IdentityOptions;
using rksec::face::IdState;
using rksec::face::Observation;
using Clock = rksec::face::IdentityBook::Clock;
using std::chrono::milliseconds;

namespace {

// 第 i 维是 1 的向量（加一点别的维度，测归一化）
std::vector<float> axis(std::size_t i, float other = 0)
{
    std::vector<float> v(rksec::face::kEmbeddingSize, other);
    v[i] = 1;
    rksec::face::l2_normalize(v);
    return v;
}

rksec::track::Track person(int id, float w = 100, int cls = 0)
{
    rksec::track::Track t;
    t.id = id;
    t.box = {0, 0, w, w * 2, 0.9F, cls};
    return t;
}

Observation seen_as(const std::string &name, float score = 0.6F)
{
    return {true, true, name, score};
}

const Observation kStrangerFace{true, false, "", 0.1F};
const Observation kNoFace{};

}  // namespace

TEST(FaceGallery, NormalizeAndCosine)
{
    std::vector<float> v{3, 4};
    EXPECT_FLOAT_EQ(rksec::face::l2_normalize(v), 5);
    EXPECT_FLOAT_EQ(v[0], 0.6F);
    EXPECT_FLOAT_EQ(rksec::face::cosine(v, v), 1);
    EXPECT_FLOAT_EQ(rksec::face::cosine(v, {-0.6F, -0.8F}), -1);
    EXPECT_FLOAT_EQ(rksec::face::cosine(v, {-0.8F, 0.6F}), 0) << "垂直";
    EXPECT_FLOAT_EQ(rksec::face::cosine(v, {1, 0, 0}), -1) << "长度不同";
    std::vector<float> zero(4, 0);
    EXPECT_FLOAT_EQ(rksec::face::l2_normalize(zero), 0);
    EXPECT_EQ(zero, std::vector<float>(4, 0));
}

TEST(FaceGallery, MatchPicksTheClosestPerson)
{
    Gallery g;
    g.add("张三", axis(0));
    g.add("李四", axis(1));
    g.add("李四", axis(2));  // 同一个人第二张照片
    g.add("王五", axis(3));
    EXPECT_EQ(g.size(), 4U);
    EXPECT_EQ(g.people(), 3U);

    // 和李四的第二张照片最像
    std::vector<float> q = axis(2, 0.02F);
    auto m = g.match(q, 0.5F);
    EXPECT_EQ(m.name, "李四");
    EXPECT_TRUE(m.matched);
    EXPECT_GT(m.score, 0.9F);
    EXPECT_LT(m.second, 0.1F) << "第二像的是另一个人（李四的另一张照片不算）";

    // 谁都不像：最像的人的分数低于阈值，不算认出来
    m = g.match(axis(100), 0.5F);
    EXPECT_FALSE(m.matched);
    EXPECT_LT(m.score, 0.5F);
}

TEST(FaceGallery, EmptyGalleryMatchesNobody)
{
    const Gallery g;
    const auto m = g.match(axis(0), 0.3F);
    EXPECT_TRUE(m.name.empty());
    EXPECT_FALSE(m.matched);
}

TEST(FaceGallery, NameFromFile)
{
    EXPECT_EQ(rksec::face::name_from_file("/home/orangepi/faces/赵小明.jpg"), "赵小明");
    EXPECT_EQ(rksec::face::name_from_file("赵小明_2.jpg"), "赵小明");
    EXPECT_EQ(rksec::face::name_from_file("demo_user.jpg"), "demo_user") << "下划线后面不是数字，是名字的一部分";
    EXPECT_EQ(rksec::face::name_from_file("demo_user_12.JPG"), "demo_user");
    EXPECT_EQ(rksec::face::name_from_file("abc"), "abc");
}

TEST(FaceIdentity, NewPeopleFirstThenWaitBeforeRetrying)
{
    IdentityOptions o;
    IdentityBook book{o};
    const auto t0 = Clock::now();
    const std::vector<rksec::track::Track> tracks{person(1), person(2)};
    auto i = book.pick(tracks, t0);
    ASSERT_TRUE(i);
    book.attempted(tracks[*i].id, t0);
    auto j = book.pick(tracks, t0);
    ASSERT_TRUE(j);
    EXPECT_NE(tracks[*j].id, tracks[*i].id) << "另一个人还没试过，先试他";
    book.attempted(tracks[*j].id, t0);
    EXPECT_FALSE(book.pick(tracks, t0 + milliseconds(100))) << "都试过了，还没到 retry";
    EXPECT_TRUE(book.pick(tracks, t0 + o.retry));
}

TEST(FaceIdentity, OnlyPeopleAndLargerBoxFirst)
{
    IdentityBook book;
    const std::vector<rksec::track::Track> tracks{person(1, 50), person(2, 200, 2), person(3, 120)};
    const auto i = book.pick(tracks, Clock::now());
    ASSERT_TRUE(i);
    EXPECT_EQ(tracks[*i].id, 3) << "都没试过：选框大的；编号 2 不是人（类别 2）";
}

TEST(FaceIdentity, NeedsTheSameResultTwice)
{
    IdentityBook book;
    const auto t = Clock::now();
    book.record(1, seen_as("张三"), t);
    EXPECT_EQ(book.get(1).state, IdState::pending) << "一次不够";
    book.record(1, kNoFace, t);  // 没检测到脸不打断
    book.record(1, seen_as("张三", 0.7F), t);
    EXPECT_EQ(book.get(1).state, IdState::known);
    EXPECT_EQ(book.get(1).name, "张三");
    EXPECT_FLOAT_EQ(book.get(1).score, 0.7F);

    // 认出来以后，单独一次认成别人不改
    book.record(1, seen_as("李四"), t);
    EXPECT_EQ(book.get(1).name, "张三");
    book.record(1, seen_as("张三"), t);
    book.record(1, seen_as("李四"), t);
    EXPECT_EQ(book.get(1).name, "张三") << "李四没有连续两次";
    book.record(1, seen_as("李四"), t);
    EXPECT_EQ(book.get(1).name, "李四");
    EXPECT_EQ(book.stats().adopted, 2U);
}

TEST(FaceIdentity, StrangerAndRecheckInterval)
{
    IdentityOptions o;
    IdentityBook book{o};
    const auto t0 = Clock::now();
    const std::vector<rksec::track::Track> tracks{person(5)};
    book.attempted(5, t0);
    book.record(5, kStrangerFace, t0);
    book.record(5, kStrangerFace, t0);
    EXPECT_EQ(book.get(5).state, IdState::stranger);
    EXPECT_TRUE(book.get(5).name.empty());
    EXPECT_FALSE(book.pick(tracks, t0 + o.retry)) << "有结论了：按 recheck 复查，不按 retry";
    EXPECT_TRUE(book.pick(tracks, t0 + o.recheck));
    const auto s = book.stats();
    EXPECT_EQ(s.attempts, 1U);
    EXPECT_EQ(s.unmatched, 2U);
}

TEST(FaceIdentity, ForgetsTracksThatAreGone)
{
    IdentityOptions o;
    IdentityBook book{o};
    const auto t0 = Clock::now();
    book.record(7, seen_as("张三"), t0);
    book.record(7, seen_as("张三"), t0);
    book.seen({person(7)}, t0);
    book.seen({}, t0 + o.forget);
    EXPECT_EQ(book.get(7).state, IdState::known) << "刚好到时间，还留着";
    book.seen({}, t0 + o.forget + milliseconds(1));
    EXPECT_EQ(book.get(7).state, IdState::pending) << "删掉了";
}

TEST(GalleryAccess, StablePersonIdAndLegacyGallery)
{
    Gallery g;
    g.add("TestPerson", axis(0), 42);
    EXPECT_EQ(g.match(axis(0), 0.5F).person_id, 42);
    Gallery legacy;
    legacy.add("TestPerson", axis(0));
    EXPECT_EQ(legacy.match(axis(0), 0.5F).person_id, 0);
}
