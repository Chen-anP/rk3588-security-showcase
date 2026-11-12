#include "access/pass_store.hpp"
#include "face/face_db.hpp"
#include <cmath>
#include <sqlite3.h>
namespace rksec::access {
namespace {
class Stmt {
  public:
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    sqlite3_stmt *get() const { return s_; }

  private:
    sqlite3_stmt *s_ = nullptr;
};
struct Transaction {
    sqlite3 *db;
    ~Transaction() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};
std::string text(sqlite3_stmt *s, int column)
{
    const auto *p = sqlite3_column_text(s, column);
    return p ? std::string(reinterpret_cast<const char *>(p)) : std::string{};
}
bool bind_text(sqlite3_stmt *s, int index, const std::string &value)
{
    return sqlite3_bind_text(s, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}
bool bind_blob(sqlite3_stmt *s, int index, const std::vector<std::uint8_t> &value)
{
    return (value.empty() ? sqlite3_bind_null(s, index)
                          : sqlite3_bind_blob(s, index, value.data(), static_cast<int>(value.size()),
                                              SQLITE_TRANSIENT)) == SQLITE_OK;
}
}  // namespace
struct PassStore::Impl {
    sqlite3 *db = nullptr;
    bool read_only = false;
    bool old_schema = false;
    ~Impl()
    {
        if (db) {
            sqlite3_close(db);
        }
    }
};
PassStore::PassStore() : impl_(std::make_unique<Impl>()) {}
PassStore::~PassStore() = default;
std::unique_ptr<PassStore> PassStore::open(const std::string &path, std::string &error, bool read_only)
{
    error.clear();
    std::unique_ptr<PassStore> store{new PassStore()};
    auto &impl = *store->impl_;
    impl.read_only = read_only;
    if (!read_only) {
        impl.db = face::open_database(path, error);
    } else {
        if (sqlite3_open_v2(path.c_str(), &impl.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            error = "cannot open database read-only";
            return {};
        }
        sqlite3_busy_timeout(impl.db, 2000);
        const Stmt version{impl.db, "PRAGMA user_version"};
        if (!version.get() || sqlite3_step(version.get()) != SQLITE_ROW) {
            error = "cannot read database version";
            return {};
        }
        const int n = sqlite3_column_int(version.get(), 0);
        if (n < 1 || n > face::kDatabaseVersion) {
            error = "unsupported database version";
            return {};
        }
        impl.old_schema = n < 3;
    }
    if (!impl.db) {
        return {};
    }
    return store;
}
bool PassStore::add(const NewPass &pass, std::int64_t &id, bool &inserted, std::string &error)
{
    id = 0;
    inserted = false;
    error.clear();
    if (impl_->read_only || pass.person_id <= 0 || pass.request_id.empty() || pass.request_id.size() > 128 ||
        pass.request_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.") !=
            std::string::npos ||
        pass.occurred_ms < 0 || pass.occurred_ms > 253402300799999LL || !std::isfinite(pass.score) || pass.score < 0 ||
        pass.score > 1 || pass.snapshot.size() > std::size_t{1048576}) {
        error = "invalid passage or read-only store";
        return false;
    }
    auto *db = impl_->db;
    const auto fail = [&] {
        error = sqlite3_errmsg(db);
        return false;
    };
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail();
    }
    const Transaction tx{db};
    std::int64_t result_id = 0;
    {
        const Stmt existing{
            db,
            "SELECT id,person_ref=? AND occurred_ms=? AND score=? AND snapshot IS ? FROM passage WHERE request_id=?"};
        if (!existing.get() || sqlite3_bind_int64(existing.get(), 1, pass.person_id) != SQLITE_OK ||
            sqlite3_bind_int64(existing.get(), 2, pass.occurred_ms) != SQLITE_OK ||
            sqlite3_bind_double(existing.get(), 3, pass.score) != SQLITE_OK ||
            !bind_blob(existing.get(), 4, pass.snapshot) || !bind_text(existing.get(), 5, pass.request_id)) {
            return fail();
        }
        const int rc = sqlite3_step(existing.get());
        if (rc == SQLITE_ROW) {
            if (!sqlite3_column_int(existing.get(), 1)) {
                error = "request_id conflict";
                return false;
            }
            result_id = sqlite3_column_int64(existing.get(), 0);
        } else if (rc != SQLITE_DONE) {
            return fail();
        }
    }
    bool added = false;
    if (result_id == 0) {
        const Stmt s{db, "INSERT INTO passage(request_id,person_id,person_ref,person_name,occurred_ms,score,snapshot) "
                         "SELECT ?,id,id,name,?,?,? FROM person WHERE id=?"};
        if (!s.get() || !bind_text(s.get(), 1, pass.request_id) ||
            sqlite3_bind_int64(s.get(), 2, pass.occurred_ms) != SQLITE_OK ||
            sqlite3_bind_double(s.get(), 3, pass.score) != SQLITE_OK || !bind_blob(s.get(), 4, pass.snapshot) ||
            sqlite3_bind_int64(s.get(), 5, pass.person_id) != SQLITE_OK || sqlite3_step(s.get()) != SQLITE_DONE) {
            return fail();
        }
        if (sqlite3_changes(db) != 1) {
            error = "person does not exist";
            return false;
        }
        result_id = sqlite3_last_insert_rowid(db);
        added = true;
    }
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail();
    }
    id = result_id;
    inserted = added;
    return true;
}
bool PassStore::list(std::vector<PassRow> &rows, int limit, std::string &error)
{
    rows.clear();
    error.clear();
    if (limit < 1 || limit > 1000) {
        error = "limit must be 1..1000";
        return false;
    }
    if (impl_->old_schema) {
        return true;
    }
    const Stmt s{impl_->db, "SELECT id,request_id,person_id,person_ref,person_name,occurred_ms,score,snapshot IS NOT "
                            "NULL FROM passage ORDER BY occurred_ms DESC,id DESC LIMIT ?"};
    if (!s.get() || sqlite3_bind_int(s.get(), 1, limit) != SQLITE_OK) {
        error = sqlite3_errmsg(impl_->db);
        return false;
    }
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        PassRow row;
        row.id = sqlite3_column_int64(s.get(), 0);
        row.request_id = text(s.get(), 1);
        if (sqlite3_column_type(s.get(), 2) != SQLITE_NULL) {
            row.person_id = sqlite3_column_int64(s.get(), 2);
        }
        row.person_ref = sqlite3_column_int64(s.get(), 3);
        row.person_name = text(s.get(), 4);
        row.occurred_ms = sqlite3_column_int64(s.get(), 5);
        row.score = sqlite3_column_double(s.get(), 6);
        row.has_snapshot = sqlite3_column_int(s.get(), 7) != 0;
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        rows.clear();
        error = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}
}  // namespace rksec::access
