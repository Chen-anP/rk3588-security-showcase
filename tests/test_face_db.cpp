// 人脸库和登记逻辑（第 7d 步）。人脸库用临时文件，测完删掉。
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "face/enroll.hpp"
#include "face/face_db.hpp"

using rksec::face::EnrollCandidate;
using rksec::face::EnrollOptions;
using rksec::face::FaceDb;
using rksec::face::Gallery;
using rksec::face::NewFace;

namespace {

std::vector<float> axis(std::size_t i, float other = 0)
{
    std::vector<float> v(rksec::face::kEmbeddingSize, other);
    v[i] = 1;
    rksec::face::l2_normalize(v);
    return v;
}

NewFace face_of(std::size_t i)
{
    return {axis(i), {0xFF, 0xD8, 0xFF, 0xD9}, "test"};
}

// 每个测试一个新目录（还在一个不存在的子目录里，顺便测“自动建上级目录”）
class FaceDbTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        dir =
            std::filesystem::temp_directory_path() / ("rksec_facedb_" + std::to_string(::getpid()) + "_" +
                                                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir);
        path = (dir / "sub" / "faces.db").string();
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    std::filesystem::path dir;
    std::string path;
    std::string err;
};

const std::string kModel = "mobilefacenet_112_fp16_rk3588_tk232.rknn";

}  // namespace

TEST(FaceName, Rules)
{
    std::string err;
    EXPECT_TRUE(rksec::face::check_name("赵小明", err)) << err;
    EXPECT_TRUE(rksec::face::check_name("chen pan", err)) << err;
    EXPECT_TRUE(rksec::face::check_name("一二三四五六七八九十一二三四五六", err)) << "16 个字";
    EXPECT_FALSE(rksec::face::check_name("一二三四五六七八九十一二三四五六七", err)) << "17 个字";
    EXPECT_FALSE(rksec::face::check_name("", err));
    EXPECT_FALSE(rksec::face::check_name("a/b", err));
    EXPECT_FALSE(rksec::face::check_name("a\"b", err));
    EXPECT_FALSE(rksec::face::check_name("a\nb", err));
    EXPECT_FALSE(rksec::face::check_name("-x", err)) << "会被当成命令行选项";
    EXPECT_FALSE(rksec::face::check_name(" x", err));
}

TEST(FaceName, ModelKeyIsTheFileName)
{
    EXPECT_EQ(rksec::face::model_key("/home/orangepi/models/" + kModel), kModel);
    EXPECT_EQ(rksec::face::model_key(kModel), kModel);
}

TEST_F(FaceDbTest, AddListLoadRemove)
{
    auto db = FaceDb::open(path, err);
    ASSERT_TRUE(db) << err;
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777U, 0600U) << "生物特征数据：只有自己能读写";

    ASSERT_TRUE(db->add("赵小明", {face_of(0), face_of(1)}, kModel, err)) << err;
    ASSERT_TRUE(db->add("李四", {face_of(5)}, kModel, err)) << err;
    ASSERT_TRUE(db->add("赵小明", {face_of(2)}, kModel, err)) << "同名再加：加到同一个人下面";

    std::vector<rksec::face::PersonInfo> people;
    ASSERT_TRUE(db->people(kModel, people, err)) << err;
    ASSERT_EQ(people.size(), 2U);
    EXPECT_EQ(people[0].name, "李四") << "按名字排序（UTF-8 字节序）";
    EXPECT_EQ(people[1].name, "赵小明");
    EXPECT_EQ(people[1].faces, 3);
    EXPECT_EQ(people[1].other_model, 0);
    EXPECT_FALSE(people[1].created.empty());

    Gallery g;
    int skipped = 0;
    ASSERT_TRUE(db->load(kModel, g, skipped, err)) << err;
    EXPECT_EQ(g.size(), 4U);
    EXPECT_EQ(g.people(), 2U);
    EXPECT_EQ(skipped, 0);
    EXPECT_EQ(g.match(axis(1, 0.01F), 0.5F).name, "赵小明") << "存进去、读出来的特征还能用";

    int removed = 0;
    ASSERT_TRUE(db->remove("赵小明", removed, err)) << err;
    EXPECT_EQ(removed, 3);
    ASSERT_TRUE(db->remove("王五", removed, err));
    EXPECT_EQ(removed, -1) << "没有这个人";
    ASSERT_TRUE(db->people(kModel, people, err));
    EXPECT_EQ(people.size(), 1U);
    std::vector<rksec::face::StoredFace> all;
    ASSERT_TRUE(db->faces(all, err));
    EXPECT_EQ(all.size(), 1U) << "照片跟着人一起删掉了（外键 ON DELETE CASCADE）";
}

TEST_F(FaceDbTest, PersistsAcrossReopen)
{
    {
        auto db = FaceDb::open(path, err);
        ASSERT_TRUE(db) << err;
        ASSERT_TRUE(db->add("赵小明", {face_of(3)}, kModel, err)) << err;
    }
    auto db = FaceDb::open(path, err);
    ASSERT_TRUE(db) << err;
    std::vector<rksec::face::StoredFace> all;
    ASSERT_TRUE(db->faces(all, err));
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(all[0].name, "赵小明");
    EXPECT_EQ(all[0].model, kModel);
    EXPECT_EQ(all[0].aligned, (std::vector<std::uint8_t>{0xFF, 0xD8, 0xFF, 0xD9}));
}

TEST_F(FaceDbTest, OtherModelIsSkippedUntilReembedded)
{
    auto db = FaceDb::open(path, err);
    ASSERT_TRUE(db) << err;
    ASSERT_TRUE(db->add("赵小明", {face_of(0), face_of(1)}, "old_model.rknn", err)) << err;
    Gallery g;
    int skipped = 0;
    ASSERT_TRUE(db->load(kModel, g, skipped, err));
    EXPECT_TRUE(g.empty()) << "别的模型算的特征不能和现在的比";
    EXPECT_EQ(skipped, 2);
    std::vector<rksec::face::PersonInfo> people;
    ASSERT_TRUE(db->people(kModel, people, err));
    EXPECT_EQ(people[0].other_model, 2);

    std::vector<rksec::face::StoredFace> all;
    ASSERT_TRUE(db->faces(all, err));
    for (const auto &f : all) {
        ASSERT_TRUE(db->update(f.id, axis(9), kModel, err)) << err;
    }
    ASSERT_TRUE(db->load(kModel, g, skipped, err));
    EXPECT_EQ(g.size(), 2U);
    EXPECT_EQ(skipped, 0);
}

TEST_F(FaceDbTest, BadNameIsRejectedAndNothingIsWritten)
{
    auto db = FaceDb::open(path, err);
    ASSERT_TRUE(db) << err;
    EXPECT_FALSE(db->add("a/b", {face_of(0)}, kModel, err));
    std::vector<rksec::face::PersonInfo> people;
    ASSERT_TRUE(db->people(kModel, people, err));
    EXPECT_TRUE(people.empty());
}

TEST_F(FaceDbTest, NewerFormatIsRefused)
{
    {
        auto db = FaceDb::open(path, err);
        ASSERT_TRUE(db) << err;
    }
    // 模拟更新版本的程序改过格式
    sqlite3 *raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "PRAGMA user_version = 9;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_FALSE(FaceDb::open(path, err));
    EXPECT_NE(err.find("更新版本"), std::string::npos) << err;
}

TEST(FaceEnroll, PickFramesSkipsBadOnesAndKeepsThemApart)
{
    EnrollOptions o;
    o.want = 3;
    o.min_gap = 10;
    std::vector<EnrollCandidate> c;
    for (int f = 0; f < 40; ++f) {
        EnrollCandidate e;
        e.frame = f;
        e.faces = 1;
        e.width = 200;
        e.score = 0.95F + static_cast<float>(f % 7) * 0.001F;
        c.push_back(e);
    }
    c[6].score = 0.999F;  // 最好的
    c[7].score = 0.998F;  // 第二好，但离 6 太近
    c[20].faces = 2;      // 有别人
    c[20].score = 1.0F;
    c[30].width = 50;  // 太远
    c[30].score = 1.0F;
    const auto p = rksec::face::pick_enroll_frames(c, o);
    ASSERT_EQ(p.size(), 3U);
    EXPECT_EQ(c[p[0]].frame, 6);
    for (std::size_t i = 0; i < p.size(); ++i) {
        EXPECT_NE(c[p[i]].frame, 7);
        EXPECT_NE(c[p[i]].frame, 20);
        EXPECT_NE(c[p[i]].frame, 30);
        if (i > 0) {
            EXPECT_GE(c[p[i]].frame - c[p[i - 1]].frame, o.min_gap) << "按帧号排好、相互隔开";
        }
    }
    EXPECT_NE(rksec::face::reject_reason(c[20], o).find("2 张脸"), std::string::npos);
    EXPECT_NE(rksec::face::reject_reason(c[30], o).find("太小"), std::string::npos);
    EXPECT_TRUE(rksec::face::reject_reason(c[6], o).empty());
}

TEST(FaceEnroll, NotEnoughGoodFrames)
{
    EnrollOptions o;
    std::vector<EnrollCandidate> c(5);  // 都没有脸
    EXPECT_TRUE(rksec::face::pick_enroll_frames(c, o).empty());
    EXPECT_EQ(rksec::face::reject_reason(c[0], o), "没检测到脸");
}

TEST(FaceEnroll, ChecksSamePersonAndNotSomeoneElse)
{
    EnrollOptions o;
    Gallery g;
    g.add("李四", axis(5));
    g.add("赵小明", axis(0));
    // 几张都像，和李四不像：通过
    auto r = rksec::face::check_enroll({axis(0, 0.02F), axis(0, 0.03F)}, "赵小明", g, 0.4F, o);
    EXPECT_TRUE(r.ok) << r.reason << "（和自己已有的照片像不算问题：给自己加照片）";
    EXPECT_EQ(r.closest_other, "李四");
    // 中途换了人
    r = rksec::face::check_enroll({axis(0), axis(7)}, "王五", g, 0.4F, o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("不像同一个人"), std::string::npos) << r.reason;
    // 李四想用另一个名字登记
    r = rksec::face::check_enroll({axis(5, 0.01F)}, "李四2", g, 0.4F, o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("李四"), std::string::npos) << r.reason;
    EXPECT_FALSE(rksec::face::check_enroll({}, "x", g, 0.4F, o).ok);
}

TEST_F(FaceDbTest, GalleryContainsDatabasePersonId)
{
    auto db = FaceDb::open(path, err);
    ASSERT_TRUE(db);
    ASSERT_TRUE(db->add("TestPerson", {face_of(0)}, kModel, err));
    Gallery gallery;
    int skipped = 0;
    ASSERT_TRUE(db->load(kModel, gallery, skipped, err));
    std::vector<rksec::face::PersonInfo> people;
    ASSERT_TRUE(db->people(kModel, people, err));
    ASSERT_EQ(people.size(), 1U);
    EXPECT_EQ(gallery.match(axis(0), 0.5F).person_id, people[0].id);
}
