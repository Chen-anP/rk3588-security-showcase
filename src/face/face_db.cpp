#include "face/face_db.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cstring>
#include <filesystem>

namespace rksec::face {

namespace {

constexpr int kBusyTimeoutMs = 2000;
constexpr std::size_t kMaxNameChars = 16;

// 每个版本要执行的建表语句：版本 0（新文件）→ 1 → 2 → 3，依次执行
const char *const kSchemaV1 = R"(
CREATE TABLE IF NOT EXISTS person (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE,
    created TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%S', 'now', 'localtime'))
);
CREATE TABLE IF NOT EXISTS face (
    id        INTEGER PRIMARY KEY,
    person_id INTEGER NOT NULL REFERENCES person(id) ON DELETE CASCADE,
    model     TEXT NOT NULL,
    embedding BLOB NOT NULL,
    aligned   BLOB,
    source    TEXT,
    created   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%S', 'now', 'localtime'))
);
CREATE INDEX IF NOT EXISTS face_person ON face(person_id);
)";

// 第 8b 步：事件录像。status：recording（正在录；程序被杀掉后留下的也是它）/ complete / incomplete
// （录到一半程序退出，文件能播但不完整）/ lost（文件找不到了）/ deleted（超配额删了文件，记录留着）
const char *const kSchemaV2 = R"(
CREATE TABLE IF NOT EXISTS event (
    id          INTEGER PRIMARY KEY,
    kind        TEXT NOT NULL,
    started     TEXT NOT NULL,
    ended       TEXT,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    tracks      TEXT NOT NULL DEFAULT '',
    file        TEXT NOT NULL DEFAULT '',
    bytes       INTEGER NOT NULL DEFAULT 0,
    snapshot    BLOB,
    status      TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS event_status ON event(status);
)";

const char *const kSchemaV3 = R"(
CREATE TABLE passage (
 id INTEGER PRIMARY KEY,
 request_id TEXT NOT NULL UNIQUE CHECK(length(request_id) BETWEEN 1 AND 128),
 person_id INTEGER REFERENCES person(id) ON DELETE SET NULL,
 person_ref INTEGER NOT NULL CHECK(person_ref > 0),
 person_name TEXT NOT NULL,
 occurred_ms INTEGER NOT NULL CHECK(occurred_ms BETWEEN 0 AND 253402300799999),
 score REAL NOT NULL CHECK(score >= 0 AND score <= 1),
 snapshot BLOB CHECK(snapshot IS NULL OR length(snapshot) <= 1048576)
);
CREATE INDEX passage_time ON passage(occurred_ms DESC,id DESC);
)";

bool exec_sql(sqlite3 *db, const std::string &sql, std::string &err)
{
    char *msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        err = std::string{"SQLite："} + (msg != nullptr ? msg : sqlite3_errmsg(db));
        sqlite3_free(msg);
        return false;
    }
    return true;
}

// 预编译语句：出作用域自动释放
class Stmt {
  public:
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    sqlite3_stmt *get() const { return s_; }
    bool ok() const { return s_ != nullptr; }

  private:
    sqlite3_stmt *s_ = nullptr;
};

std::string text_column(sqlite3_stmt *s, int col)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    return t != nullptr ? std::string{reinterpret_cast<const char *>(t)} : std::string{};
}

std::vector<std::uint8_t> blob_column(sqlite3_stmt *s, int col)
{
    const auto *p = static_cast<const std::uint8_t *>(sqlite3_column_blob(s, col));
    const int n = sqlite3_column_bytes(s, col);
    return p != nullptr && n > 0 ? std::vector<std::uint8_t>(p, p + n) : std::vector<std::uint8_t>{};
}

}  // namespace

struct FaceDb::Impl {
    sqlite3 *db = nullptr;

    ~Impl()
    {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }

    bool exec(const char *sql, std::string &err) const
    {
        char *msg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            err = std::string{"SQLite："} + (msg != nullptr ? msg : sqlite3_errmsg(db));
            sqlite3_free(msg);
            return false;
        }
        return true;
    }

    std::string error() const { return std::string{"SQLite："} + sqlite3_errmsg(db); }
};

bool check_name(const std::string &name, std::string &err)
{
    if (name.empty()) {
        err = "名字不能为空";
        return false;
    }
    std::size_t chars = 0;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c & 0xC0U) != 0x80U) {
            ++chars;  // 每个 UTF-8 字符只有第一个字节不是 10xxxxxx
        }
        if (c < 0x20 || c == 0x7F) {
            err = "名字里不能有控制字符";
            return false;
        }
        if (std::strchr("/\\\"'`<>|;", static_cast<int>(c)) != nullptr) {
            err = std::string{"名字里不能有符号 "} + static_cast<char>(c);
            return false;
        }
    }
    if (chars > kMaxNameChars) {
        err = "名字最多 " + std::to_string(kMaxNameChars) + " 个字，实际 " + std::to_string(chars) + " 个";
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '-') {
        err = "名字不能以空格或 - 开头，也不能以空格结尾";
        return false;
    }
    return true;
}

std::string model_key(const std::string &model_path)
{
    return std::filesystem::path{model_path}.filename().string();
}

FaceDb::FaceDb() : impl_(std::make_unique<Impl>()) {}
FaceDb::~FaceDb() = default;

sqlite3 *open_database(const std::string &path, std::string &err)
{
    std::error_code ec;
    const std::filesystem::path p{path};
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    const bool existed = std::filesystem::exists(p, ec);
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        err = "数据库 " + path + " 打不开：" + std::string{sqlite3_errmsg(db)};
        sqlite3_close(db);
        return nullptr;
    }
    if (!existed) {
        ::chmod(path.c_str(), S_IRUSR | S_IWUSR);  // 生物特征数据、录像记录：只有自己能读写
    }
    sqlite3_busy_timeout(db, kBusyTimeoutMs);
    const auto fail = [&](const std::string &why) {
        err = "数据库 " + path + "：" + why;
        sqlite3_close(db);
        return nullptr;
    };
    std::string e;
    if (!exec_sql(db, "PRAGMA foreign_keys = ON;", e)) {
        return fail(e);
    }
    int version = 0;
    {
        const Stmt s{db, "PRAGMA user_version;"};
        if (s.ok() && sqlite3_step(s.get()) == SQLITE_ROW) {
            version = sqlite3_column_int(s.get(), 0);
        }
    }
    if (version > kDatabaseVersion) {
        return fail("是更新版本的程序建的（格式版本 " + std::to_string(version) + "，这个程序只认识 " +
                    std::to_string(kDatabaseVersion) + "）");
    }
    if (version < kDatabaseVersion) {
        // 一个事务里依次升级：中途失败什么都不改
        if (!exec_sql(db, "BEGIN IMMEDIATE;", e)) {
            return fail(e);
        }
        // 获取迁移写锁后重读版本，避免两个进程同时按旧版本重复建表。
        {
            const Stmt current{db, "PRAGMA user_version;"};
            if (!current.ok() || sqlite3_step(current.get()) != SQLITE_ROW) {
                exec_sql(db, "ROLLBACK;", e);
                return fail("读取迁移版本失败");
            }
            version = sqlite3_column_int(current.get(), 0);
        }
        if (version > kDatabaseVersion) {
            exec_sql(db, "ROLLBACK;", e);
            return fail("迁移期间格式版本已变更");
        }
        std::string sql;
        if (version < 1) {
            sql += kSchemaV1;
        }
        if (version < 2) {
            sql += kSchemaV2;
        }
        if (version < 3) {
            sql += kSchemaV3;
        }
        sql += "PRAGMA user_version = " + std::to_string(kDatabaseVersion) + "; COMMIT;";
        if (!exec_sql(db, sql, e)) {
            exec_sql(db, "ROLLBACK;", e);
            return fail("建表 / 升级（版本 " + std::to_string(version) + " → " + std::to_string(kDatabaseVersion) +
                        "）失败：" + e);
        }
    }
    return db;
}

std::unique_ptr<FaceDb> FaceDb::open(const std::string &path, std::string &err)
{
    std::unique_ptr<FaceDb> f{new FaceDb()};
    f->path_ = path;
    f->impl_->db = open_database(path, err);
    if (f->impl_->db == nullptr) {
        return nullptr;
    }
    return f;
}

bool FaceDb::add(const std::string &name, const std::vector<NewFace> &faces, const std::string &model, std::string &err)
{
    if (!check_name(name, err)) {
        return false;
    }
    Impl &m = *impl_;
    if (!m.exec("BEGIN IMMEDIATE;", err)) {
        return false;
    }
    const auto fail = [&m, &err](const std::string &what) {
        err = what + "：" + m.error();
        std::string ignored;
        m.exec("ROLLBACK;", ignored);
        return false;
    };
    {
        const Stmt s{m.db, "INSERT OR IGNORE INTO person(name) VALUES (?);"};
        if (!s.ok()) {
            return fail("登记失败");
        }
        sqlite3_bind_text(s.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.get()) != SQLITE_DONE) {
            return fail("登记失败");
        }
    }
    const Stmt s{m.db, "INSERT INTO face(person_id, model, embedding, aligned, source) "
                       "SELECT id, ?, ?, ?, ? FROM person WHERE name = ?;"};
    if (!s.ok()) {
        return fail("登记失败");
    }
    for (const NewFace &f : faces) {
        sqlite3_reset(s.get());
        sqlite3_bind_text(s.get(), 1, model.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s.get(), 2, f.embedding.data(), static_cast<int>(f.embedding.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
        if (f.aligned.empty()) {
            sqlite3_bind_null(s.get(), 3);
        } else {
            sqlite3_bind_blob(s.get(), 3, f.aligned.data(), static_cast<int>(f.aligned.size()), SQLITE_TRANSIENT);
        }
        sqlite3_bind_text(s.get(), 4, f.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.get(), 5, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.get()) != SQLITE_DONE) {
            return fail("保存特征失败");
        }
    }
    return m.exec("COMMIT;", err);
}

bool FaceDb::remove(const std::string &name, int &removed, std::string &err)
{
    Impl &m = *impl_;
    removed = 0;
    {
        const Stmt s{m.db, "SELECT count(*) FROM face JOIN person ON person.id = face.person_id WHERE name = ?;"};
        sqlite3_bind_text(s.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.get()) == SQLITE_ROW) {
            removed = sqlite3_column_int(s.get(), 0);
        }
    }
    const Stmt s{m.db, "DELETE FROM person WHERE name = ?;"};  // 照片由 ON DELETE CASCADE 一起删
    sqlite3_bind_text(s.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        err = "删除失败：" + m.error();
        return false;
    }
    if (sqlite3_changes(m.db) == 0) {
        removed = -1;
    }
    return true;
}

bool FaceDb::people(const std::string &model, std::vector<PersonInfo> &out, std::string &err)
{
    Impl &m = *impl_;
    const Stmt s{m.db, "SELECT person.id, name, person.created, count(face.id), "
                       "       sum(CASE WHEN face.id IS NOT NULL AND face.model <> ? THEN 1 ELSE 0 END) "
                       "FROM person LEFT JOIN face ON face.person_id = person.id "
                       "GROUP BY person.id ORDER BY name;"};
    if (!s.ok()) {
        err = m.error();
        return false;
    }
    sqlite3_bind_text(s.get(), 1, model.c_str(), -1, SQLITE_TRANSIENT);
    out.clear();
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        PersonInfo p;
        p.id = sqlite3_column_int64(s.get(), 0);
        p.name = text_column(s.get(), 1);
        p.created = text_column(s.get(), 2);
        p.faces = sqlite3_column_int(s.get(), 3);
        p.other_model = sqlite3_column_int(s.get(), 4);
        out.push_back(std::move(p));
    }
    if (rc != SQLITE_DONE) {
        err = m.error();
        return false;
    }
    return true;
}

bool FaceDb::load(const std::string &model, Gallery &gallery, int &skipped, std::string &err)
{
    Impl &m = *impl_;
    const Stmt s{m.db, "SELECT name, model, embedding, person.id FROM face JOIN person ON person.id = face.person_id "
                       "ORDER BY face.id;"};
    if (!s.ok()) {
        err = m.error();
        return false;
    }
    skipped = 0;
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        const std::vector<std::uint8_t> blob = blob_column(s.get(), 2);
        if (text_column(s.get(), 1) != model || blob.size() != kEmbeddingSize * sizeof(float)) {
            ++skipped;
            continue;
        }
        std::vector<float> e(kEmbeddingSize);
        std::memcpy(e.data(), blob.data(), blob.size());
        gallery.add(text_column(s.get(), 0), std::move(e), sqlite3_column_int64(s.get(), 3));
    }
    if (rc != SQLITE_DONE) {
        err = m.error();
        return false;
    }
    return true;
}

bool FaceDb::faces(std::vector<StoredFace> &out, std::string &err)
{
    Impl &m = *impl_;
    const Stmt s{m.db, "SELECT face.id, name, model, aligned FROM face JOIN person ON person.id = face.person_id "
                       "ORDER BY face.id;"};
    if (!s.ok()) {
        err = m.error();
        return false;
    }
    out.clear();
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        StoredFace f;
        f.id = sqlite3_column_int64(s.get(), 0);
        f.name = text_column(s.get(), 1);
        f.model = text_column(s.get(), 2);
        f.aligned = blob_column(s.get(), 3);
        out.push_back(std::move(f));
    }
    if (rc != SQLITE_DONE) {
        err = m.error();
        return false;
    }
    return true;
}

bool FaceDb::update(std::int64_t face_id, const std::vector<float> &embedding, const std::string &model,
                    std::string &err)
{
    Impl &m = *impl_;
    const Stmt s{m.db, "UPDATE face SET embedding = ?, model = ? WHERE id = ?;"};
    sqlite3_bind_blob(s.get(), 1, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s.get(), 2, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.get(), 3, face_id);
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        err = "更新特征失败：" + m.error();
        return false;
    }
    return true;
}

}  // namespace rksec::face
