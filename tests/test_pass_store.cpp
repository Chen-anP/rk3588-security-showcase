#include "access/pass_store.hpp"
#include "face/face_db.hpp"
#include <array>
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <sqlite3.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using rksec::access::NewPass;
using rksec::access::PassStore;
namespace {
class PassStoreTest : public ::testing::Test {
  protected:
    std::filesystem::path dir;
    std::string path, error;
    void SetUp() override
    {
        dir =
            std::filesystem::temp_directory_path() / ("rksec_pass_" + std::to_string(getpid()) + "_" +
                                                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(dir);
        path = (dir / "test.db").string();
    }
    void TearDown() override { std::filesystem::remove_all(dir); }
    void sql(const char *text)
    {
        sqlite3 *db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, text, nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
        sqlite3_close(db);
    }
    int scalar(const char *text)
    {
        sqlite3 *db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(db, text, -1, &s, nullptr);
        const int rc = sqlite3_step(s);
        const int result = rc == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
        sqlite3_finalize(s);
        sqlite3_close(db);
        return result;
    }
    std::unique_ptr<PassStore> seeded()
    {
        auto s = PassStore::open(path, error);
        if (s) {
            sql("INSERT INTO person(id,name) VALUES(1,'TestPerson')");
        }
        return s;
    }
    static NewPass record() { return {"boot-a:1", 1, 1700000000123LL, 0.75, {1, 2, 3}}; }
};
}  // namespace
TEST_F(PassStoreTest, NewDatabaseVersionAndPermissions)
{
    auto s = seeded();
    ASSERT_TRUE(s) << error;
    EXPECT_EQ(scalar("PRAGMA user_version"), 3);
    struct stat st {};
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U);
}
TEST_F(PassStoreTest, WriteRetryAndReadOnlyQuery)
{
    auto s = seeded();
    ASSERT_TRUE(s);
    std::int64_t id = 0, retry = 0;
    bool inserted = false;
    ASSERT_TRUE(s->add(record(), id, inserted, error)) << error;
    EXPECT_TRUE(inserted);
    ASSERT_TRUE(s->add(record(), retry, inserted, error)) << error;
    EXPECT_FALSE(inserted);
    EXPECT_EQ(id, retry);
    auto query = PassStore::open(path, error, true);
    ASSERT_TRUE(query);
    std::vector<rksec::access::PassRow> rows;
    ASSERT_TRUE(query->list(rows, 20, error));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].person_name, "TestPerson");
    EXPECT_EQ(rows[0].person_id, 1);
    EXPECT_TRUE(rows[0].has_snapshot);
    EXPECT_FALSE(query->add(record(), retry, inserted, error));
}
TEST_F(PassStoreTest, RequestConflictRollsBack)
{
    auto s = seeded();
    std::int64_t id = 0;
    bool inserted = false;
    ASSERT_TRUE(s->add(record(), id, inserted, error));
    auto changed = record();
    changed.score = 0.8;
    EXPECT_FALSE(s->add(changed, id, inserted, error));
    EXPECT_EQ(id, 0);
    EXPECT_FALSE(inserted);
    std::vector<rksec::access::PassRow> rows;
    ASSERT_TRUE(s->list(rows, 20, error));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_DOUBLE_EQ(rows[0].score, 0.75);
}
TEST_F(PassStoreTest, DeletePersonKeepsHistoryAndRetry)
{
    auto s = seeded();
    std::int64_t id = 0;
    bool inserted = false;
    ASSERT_TRUE(s->add(record(), id, inserted, error));
    auto faces = rksec::face::FaceDb::open(path, error);
    ASSERT_TRUE(faces);
    int removed = 0;
    ASSERT_TRUE(faces->remove("TestPerson", removed, error));
    std::vector<rksec::access::PassRow> rows;
    ASSERT_TRUE(s->list(rows, 20, error));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_FALSE(rows[0].person_id);
    EXPECT_EQ(rows[0].person_ref, 1);
    EXPECT_EQ(rows[0].person_name, "TestPerson");
    ASSERT_TRUE(s->add(record(), id, inserted, error)) << error;
    EXPECT_FALSE(inserted);
}
TEST_F(PassStoreTest, MissingPersonAndInvalidInputDoNotWrite)
{
    auto s = PassStore::open(path, error);
    ASSERT_TRUE(s);
    std::int64_t id = 0;
    bool inserted = false;
    EXPECT_FALSE(s->add(record(), id, inserted, error));
    auto bad = record();
    bad.score = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(s->add(bad, id, inserted, error));
    bad = record();
    bad.request_id = "bad\nrequest";
    EXPECT_FALSE(s->add(bad, id, inserted, error));
    bad = record();
    bad.snapshot.resize(1048577);
    EXPECT_FALSE(s->add(bad, id, inserted, error));
    EXPECT_EQ(scalar("SELECT count(*) FROM passage"), 0);
}
TEST_F(PassStoreTest, Version2ReadOnlyDoesNotMigrateAndWriteMigrates)
{
    sql("CREATE TABLE person(id INTEGER PRIMARY KEY,name TEXT); CREATE TABLE face(id INTEGER); CREATE TABLE event(id "
        "INTEGER PRIMARY KEY); INSERT INTO person VALUES(1,'OldPerson'); INSERT INTO event VALUES(8); PRAGMA "
        "user_version=2");
    auto query = PassStore::open(path, error, true);
    ASSERT_TRUE(query);
    std::vector<rksec::access::PassRow> rows;
    ASSERT_TRUE(query->list(rows, 20, error));
    EXPECT_TRUE(rows.empty());
    EXPECT_EQ(scalar("PRAGMA user_version"), 2);
    auto writer = PassStore::open(path, error);
    ASSERT_TRUE(writer) << error;
    EXPECT_EQ(scalar("PRAGMA user_version"), 3);
    EXPECT_EQ(scalar("SELECT count(*) FROM person"), 1);
    EXPECT_EQ(scalar("SELECT id FROM event"), 8);
}
TEST_F(PassStoreTest, Version1UpgradeKeepsOldRows)
{
    sql("CREATE TABLE person(id INTEGER PRIMARY KEY,name TEXT); CREATE TABLE face(id INTEGER); INSERT INTO person "
        "VALUES(1,'OldPerson'); PRAGMA user_version=1");
    auto writer = PassStore::open(path, error);
    ASSERT_TRUE(writer) << error;
    EXPECT_EQ(scalar("PRAGMA user_version"), 3);
    EXPECT_EQ(scalar("SELECT count(*) FROM person"), 1);
    EXPECT_EQ(scalar("SELECT count(*) FROM event"), 0);
}
TEST_F(PassStoreTest, MigrationFailurePreservesVersionAndRows)
{
    sql("CREATE TABLE person(id INTEGER PRIMARY KEY,name TEXT); CREATE TABLE face(id INTEGER); CREATE TABLE "
        "passage(block INTEGER); INSERT INTO person VALUES(1,'OldPerson'); PRAGMA user_version=1");
    EXPECT_FALSE(PassStore::open(path, error));
    EXPECT_EQ(scalar("PRAGMA user_version"), 1);
    EXPECT_EQ(scalar("SELECT count(*) FROM person"), 1);
    EXPECT_EQ(scalar("SELECT count(*) FROM sqlite_master WHERE name='event'"), 0);
}
TEST_F(PassStoreTest, FutureVersionRejectedAndMissingReadOnlyDoesNotCreate)
{
    EXPECT_FALSE(PassStore::open(path, error, true));
    EXPECT_FALSE(std::filesystem::exists(path));
    sql("PRAGMA user_version=4");
    EXPECT_FALSE(PassStore::open(path, error));
    EXPECT_FALSE(PassStore::open(path, error, true));
    EXPECT_EQ(scalar("PRAGMA user_version"), 4);
}
TEST_F(PassStoreTest, NewestFirstLimitAndIndependentConnections)
{
    auto s = seeded();
    auto second = PassStore::open(path, error);
    ASSERT_TRUE(second);
    std::int64_t id = 0;
    bool inserted = false;
    ASSERT_TRUE(s->add(record(), id, inserted, error));
    ASSERT_TRUE(second->add(record(), id, inserted, error));
    EXPECT_FALSE(inserted);
    auto next = record();
    next.request_id = "boot-a:2";
    next.snapshot.clear();
    ASSERT_TRUE(second->add(next, id, inserted, error));
    std::vector<rksec::access::PassRow> rows;
    ASSERT_TRUE(s->list(rows, 1, error));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].request_id, "boot-a:2");
    EXPECT_FALSE(rows[0].has_snapshot);
    EXPECT_FALSE(s->list(rows, 0, error));
    EXPECT_TRUE(rows.empty());
}

TEST_F(PassStoreTest, ConcurrentDuplicateRequestsInsertOnce)
{
    auto first = seeded();
    auto second = PassStore::open(path, error);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    std::array<std::int64_t, 2> ids{};
    std::array<bool, 2> added{};
    std::array<bool, 2> ok{};
    std::array<std::string, 2> errors;
    std::atomic<int> ready{0};
    const auto run = [&](int i, PassStore &store) {
        ++ready;
        while (ready.load() < 2) {
            std::this_thread::yield();
        }
        const auto n = static_cast<std::size_t>(i);
        ok[n] = store.add(record(), ids[n], added[n], errors[n]);
    };
    std::thread a(run, 0, std::ref(*first));
    std::thread b(run, 1, std::ref(*second));
    a.join();
    b.join();
    ASSERT_TRUE(ok[0]) << errors[0];
    ASSERT_TRUE(ok[1]) << errors[1];
    EXPECT_EQ(ids[0], ids[1]);
    EXPECT_NE(added[0], added[1]);
    EXPECT_EQ(scalar("SELECT count(*) FROM passage"), 1);
}
TEST_F(PassStoreTest, ConcurrentMigrationRechecksVersionUnderLock)
{
    sql("CREATE TABLE person(id INTEGER PRIMARY KEY,name TEXT); CREATE TABLE face(id INTEGER); CREATE TABLE event(id "
        "INTEGER); PRAGMA user_version=2");
    std::atomic<int> ready{0};
    std::array<bool, 2> ok{};
    std::array<std::string, 2> errors;
    const auto run = [&](std::size_t i) {
        ++ready;
        while (ready.load() < 2) {
            std::this_thread::yield();
        }
        ok[i] = static_cast<bool>(PassStore::open(path, errors[i]));
    };
    std::thread a(run, 0U);
    std::thread b(run, 1U);
    a.join();
    b.join();
    EXPECT_TRUE(ok[0]) << errors[0];
    EXPECT_TRUE(ok[1]) << errors[1];
    EXPECT_EQ(scalar("PRAGMA user_version"), 3);
}
