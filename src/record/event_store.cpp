#include "record/event_store.hpp"

#include <sqlite3.h>

#include "face/face_db.hpp"

namespace rksec::record {

namespace {

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

void bind_text(sqlite3_stmt *s, int col, const std::string &v)
{
    sqlite3_bind_text(s, col, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

constexpr const char *kColumns =
    "SELECT id, kind, started, ifnull(ended, ''), duration_ms, tracks, file, bytes, status, snapshot IS NOT NULL "
    "FROM event ";

EventRow read_row(sqlite3_stmt *s)
{
    EventRow r;
    r.id = sqlite3_column_int64(s, 0);
    r.kind = text_column(s, 1);
    r.started = text_column(s, 2);
    r.ended = text_column(s, 3);
    r.duration_ms = sqlite3_column_int64(s, 4);
    r.tracks = text_column(s, 5);
    r.file = text_column(s, 6);
    r.bytes = sqlite3_column_int64(s, 7);
    r.status = text_column(s, 8);
    r.has_snapshot = sqlite3_column_int(s, 9) != 0;
    return r;
}

}  // namespace

struct EventStore::Impl {
    sqlite3 *db = nullptr;

    ~Impl()
    {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }

    std::string error() const { return std::string{"SQLite："} + sqlite3_errmsg(db); }

    // 执行一条已经绑定好参数的写语句
    bool step_done(const Stmt &s, std::string &err) const
    {
        if (!s.ok() || sqlite3_step(s.get()) != SQLITE_DONE) {
            err = error();
            return false;
        }
        return true;
    }
};

EventStore::EventStore() : impl_(std::make_unique<Impl>()) {}
EventStore::~EventStore() = default;

std::unique_ptr<EventStore> EventStore::open(const std::string &path, std::string &err)
{
    std::unique_ptr<EventStore> e{new EventStore()};
    e->impl_->db = face::open_database(path, err);
    if (e->impl_->db == nullptr) {
        return nullptr;
    }
    return e;
}

bool EventStore::begin(const std::string &kind, const std::string &started, const std::string &tracks,
                       const std::vector<std::uint8_t> &snapshot, std::int64_t &id, std::string &err)
{
    const Stmt s{impl_->db,
                 "INSERT INTO event (kind, started, tracks, snapshot, status) VALUES (?, ?, ?, ?, 'recording');"};
    if (s.ok()) {
        bind_text(s.get(), 1, kind);
        bind_text(s.get(), 2, started);
        bind_text(s.get(), 3, tracks);
        if (snapshot.empty()) {
            sqlite3_bind_null(s.get(), 4);
        } else {
            sqlite3_bind_blob(s.get(), 4, snapshot.data(), static_cast<int>(snapshot.size()), SQLITE_TRANSIENT);
        }
    }
    if (!impl_->step_done(s, err)) {
        return false;
    }
    id = sqlite3_last_insert_rowid(impl_->db);
    return true;
}

bool EventStore::set_file(std::int64_t id, const std::string &file, std::string &err)
{
    const Stmt s{impl_->db, "UPDATE event SET file = ? WHERE id = ?;"};
    if (s.ok()) {
        bind_text(s.get(), 1, file);
        sqlite3_bind_int64(s.get(), 2, id);
    }
    return impl_->step_done(s, err);
}

bool EventStore::finish(std::int64_t id, const std::string &ended, std::int64_t duration_ms, std::int64_t bytes,
                        const std::string &tracks, const std::string &status, std::string &err)
{
    const Stmt s{impl_->db,
                 "UPDATE event SET ended = ?, duration_ms = ?, bytes = ?, tracks = ?, status = ? WHERE id = ?;"};
    if (s.ok()) {
        bind_text(s.get(), 1, ended);
        sqlite3_bind_int64(s.get(), 2, duration_ms);
        sqlite3_bind_int64(s.get(), 3, bytes);
        bind_text(s.get(), 4, tracks);
        bind_text(s.get(), 5, status);
        sqlite3_bind_int64(s.get(), 6, id);
    }
    return impl_->step_done(s, err);
}

bool EventStore::mark(std::int64_t id, const std::string &status, std::string &err)
{
    const Stmt s{impl_->db, status == "deleted" ? "UPDATE event SET status = ?, snapshot = NULL WHERE id = ?;"
                                                : "UPDATE event SET status = ? WHERE id = ?;"};
    if (s.ok()) {
        bind_text(s.get(), 1, status);
        sqlite3_bind_int64(s.get(), 2, id);
    }
    return impl_->step_done(s, err);
}

bool EventStore::set_bytes(std::int64_t id, std::int64_t bytes, std::string &err)
{
    const Stmt s{impl_->db, "UPDATE event SET bytes = ? WHERE id = ?;"};
    if (s.ok()) {
        sqlite3_bind_int64(s.get(), 1, bytes);
        sqlite3_bind_int64(s.get(), 2, id);
    }
    return impl_->step_done(s, err);
}

bool EventStore::list(std::vector<EventRow> &out, std::string &err, int limit, const std::string &status)
{
    out.clear();
    const std::string sql =
        std::string{kColumns} + (status.empty() ? "" : "WHERE status = ? ") + "ORDER BY id DESC LIMIT ?;";
    const Stmt s{impl_->db, sql.c_str()};
    if (!s.ok()) {
        err = impl_->error();
        return false;
    }
    int col = 1;
    if (!status.empty()) {
        bind_text(s.get(), col++, status);
    }
    sqlite3_bind_int(s.get(), col, limit);
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        out.push_back(read_row(s.get()));
    }
    if (rc != SQLITE_DONE) {
        err = impl_->error();
        return false;
    }
    return true;
}

bool EventStore::clips(std::vector<EventRow> &out, std::string &err)
{
    out.clear();
    const std::string sql = std::string{kColumns} + "WHERE status IN ('complete', 'incomplete') ORDER BY id;";
    const Stmt s{impl_->db, sql.c_str()};
    if (!s.ok()) {
        err = impl_->error();
        return false;
    }
    int rc = 0;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        out.push_back(read_row(s.get()));
    }
    if (rc != SQLITE_DONE) {
        err = impl_->error();
        return false;
    }
    return true;
}

bool EventStore::get(std::int64_t id, EventRow &out, bool &found, std::string &err)
{
    found = false;
    const std::string sql = std::string{kColumns} + "WHERE id = ?;";
    const Stmt s{impl_->db, sql.c_str()};
    if (!s.ok()) {
        err = impl_->error();
        return false;
    }
    sqlite3_bind_int64(s.get(), 1, id);
    const int rc = sqlite3_step(s.get());
    if (rc == SQLITE_ROW) {
        out = read_row(s.get());
        found = true;
        return true;
    }
    if (rc == SQLITE_DONE) {
        return true;
    }
    err = impl_->error();
    return false;
}

bool EventStore::snapshot(std::int64_t id, std::vector<std::uint8_t> &out, std::string &err)
{
    out.clear();
    const Stmt s{impl_->db, "SELECT snapshot FROM event WHERE id = ?;"};
    if (!s.ok()) {
        err = impl_->error();
        return false;
    }
    sqlite3_bind_int64(s.get(), 1, id);
    const int rc = sqlite3_step(s.get());
    if (rc == SQLITE_ROW) {
        const auto *p = static_cast<const std::uint8_t *>(sqlite3_column_blob(s.get(), 0));
        const int n = sqlite3_column_bytes(s.get(), 0);
        if (p != nullptr && n > 0) {
            out.assign(p, p + n);
        }
        return true;
    }
    if (rc == SQLITE_DONE) {
        err = "没有事件 " + std::to_string(id);
        return false;
    }
    err = impl_->error();
    return false;
}

}  // namespace rksec::record
