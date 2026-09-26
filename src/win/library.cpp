#include "library.h"

#include <ctime>

#include "../../third_party/sqlite/sqlite3.h"

namespace {

// RAII wrapper for prepared statements.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
    }
    ~Stmt() {
        if (s_) sqlite3_finalize(s_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s_ != nullptr; }
    sqlite3_stmt* get() const { return s_; }

    void bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); }
    void bind(int i, double v) { sqlite3_bind_double(s_, i, v); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT); }
    void bind(int i, const std::wstring& v) { bind(i, wu::narrow(v)); }
    void bindBlob(int i, const Bytes& v) { sqlite3_bind_blob(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT); }

    bool step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    bool run() {
        if (!s_) return false;
        int rc = sqlite3_step(s_);
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
    int64_t i64(int col) { return sqlite3_column_int64(s_, col); }
    std::string text(int col) {
        const unsigned char* t = sqlite3_column_text(s_, col);
        return t ? std::string(reinterpret_cast<const char*>(t), size_t(sqlite3_column_bytes(s_, col))) : std::string();
    }
    Bytes blob(int col) {
        const void* b = sqlite3_column_blob(s_, col);
        return b ? Bytes(static_cast<const char*>(b), size_t(sqlite3_column_bytes(s_, col))) : Bytes();
    }

private:
    sqlite3_stmt* s_ = nullptr;
};

std::string hashBytes(const Bytes& data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[40];
    snprintf(buf, sizeof buf, "%016llx-%zx", static_cast<unsigned long long>(h), data.size());
    return buf;
}

int64_t now() { return int64_t(std::time(nullptr)); }

}  // namespace

Library::~Library() { close(); }

bool Library::open(const std::wstring& path, std::wstring* error) {
    close();
    if (sqlite3_open_v2(wu::narrow(path).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        if (error) *error = db_ ? wu::widen(sqlite3_errmsg(db_)) : L"sqlite3_open";
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    bool ok = exec("PRAGMA synchronous=NORMAL;") &&
              exec("CREATE TABLE IF NOT EXISTS images("
                   " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                   " name TEXT NOT NULL,"
                   " source TEXT,"
                   " hash TEXT UNIQUE,"
                   " width INTEGER, height INTEGER,"
                   " added INTEGER NOT NULL,"
                   " last_used INTEGER NOT NULL,"
                   " times_drawn INTEGER NOT NULL DEFAULT 0,"
                   " settings TEXT,"
                   " thumb BLOB,"
                   " data BLOB NOT NULL);") &&
              exec("CREATE INDEX IF NOT EXISTS images_last_used ON images(last_used DESC);") &&
              exec("CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);") &&
              exec("CREATE TABLE IF NOT EXISTS history("
                   " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                   " image_id INTEGER, drawn_at INTEGER, style TEXT,"
                   " strokes INTEGER, seconds REAL, completed INTEGER);");
    if (!ok) {
        if (error) *error = wu::widen(sqlite3_errmsg(db_));
        close();
        return false;
    }
    return true;
}

void Library::close() {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

bool Library::exec(const char* sql) { return db_ && sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

std::vector<LibItem> Library::list(const std::wstring& filter) {
    std::vector<LibItem> items;
    Stmt st(db_,
            "SELECT id, name, width, height, added, last_used, times_drawn FROM images"
            " WHERE ?1 = '' OR instr(lower(name), lower(?1)) > 0"
            " ORDER BY last_used DESC, id DESC;");
    if (!st) return items;
    st.bind(1, filter);
    while (st.step()) {
        LibItem it;
        it.id = st.i64(0);
        it.name = wu::widen(st.text(1));
        it.width = int(st.i64(2));
        it.height = int(st.i64(3));
        it.added = st.i64(4);
        it.lastUsed = st.i64(5);
        it.timesDrawn = int(st.i64(6));
        items.push_back(std::move(it));
    }
    return items;
}

int64_t Library::add(const std::wstring& name, const std::wstring& source, const Bytes& data, int w, int h,
                     const Bytes& thumbPng, bool* existed) {
    if (existed) *existed = false;
    const std::string hash = hashBytes(data);
    {
        Stmt st(db_, "SELECT id FROM images WHERE hash = ?1;");
        st.bind(1, hash);
        if (st.step()) {
            int64_t id = st.i64(0);
            if (existed) *existed = true;
            touch(id);
            return id;
        }
    }
    Stmt st(db_,
            "INSERT INTO images(name, source, hash, width, height, added, last_used, thumb, data)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7, ?8);");
    if (!st) return 0;
    st.bind(1, name);
    st.bind(2, source);
    st.bind(3, hash);
    st.bind(4, int64_t(w));
    st.bind(5, int64_t(h));
    st.bind(6, now());
    st.bindBlob(7, thumbPng);
    st.bindBlob(8, data);
    if (!st.run()) return 0;
    return sqlite3_last_insert_rowid(db_);
}

bool Library::data(int64_t id, Bytes& out) {
    Stmt st(db_, "SELECT data FROM images WHERE id = ?1;");
    st.bind(1, id);
    if (!st.step()) return false;
    out = st.blob(0);
    return !out.empty();
}

bool Library::thumb(int64_t id, Bytes& out) {
    Stmt st(db_, "SELECT thumb FROM images WHERE id = ?1;");
    st.bind(1, id);
    if (!st.step()) return false;
    out = st.blob(0);
    return !out.empty();
}

bool Library::item(int64_t id, LibItem& it) {
    Stmt st(db_, "SELECT id, name, width, height, added, last_used, times_drawn FROM images WHERE id = ?1;");
    st.bind(1, id);
    if (!st.step()) return false;
    it.id = st.i64(0);
    it.name = wu::widen(st.text(1));
    it.width = int(st.i64(2));
    it.height = int(st.i64(3));
    it.added = st.i64(4);
    it.lastUsed = st.i64(5);
    it.timesDrawn = int(st.i64(6));
    return true;
}

void Library::rename(int64_t id, const std::wstring& name) {
    Stmt st(db_, "UPDATE images SET name = ?2 WHERE id = ?1;");
    st.bind(1, id);
    st.bind(2, name);
    st.run();
}

void Library::remove(int64_t id) {
    Stmt st(db_, "DELETE FROM images WHERE id = ?1;");
    st.bind(1, id);
    st.run();
}

void Library::touch(int64_t id) {
    Stmt st(db_, "UPDATE images SET last_used = ?2 WHERE id = ?1;");
    st.bind(1, id);
    st.bind(2, now());
    st.run();
}

void Library::recordDrawing(int64_t id, const std::string& style, int strokes, double seconds, bool completed) {
    {
        Stmt st(db_,
                "INSERT INTO history(image_id, drawn_at, style, strokes, seconds, completed)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
        st.bind(1, id);
        st.bind(2, now());
        st.bind(3, style);
        st.bind(4, int64_t(strokes));
        st.bind(5, seconds);
        st.bind(6, int64_t(completed ? 1 : 0));
        st.run();
    }
    if (id && completed) {
        Stmt st(db_, "UPDATE images SET times_drawn = times_drawn + 1, last_used = ?2 WHERE id = ?1;");
        st.bind(1, id);
        st.bind(2, now());
        st.run();
    }
}

std::string Library::imageSettings(int64_t id) {
    Stmt st(db_, "SELECT settings FROM images WHERE id = ?1;");
    st.bind(1, id);
    return st.step() ? st.text(0) : std::string();
}

void Library::setImageSettings(int64_t id, const std::string& s) {
    Stmt st(db_, "UPDATE images SET settings = ?2 WHERE id = ?1;");
    st.bind(1, id);
    st.bind(2, s);
    st.run();
}

std::string Library::setting(const char* key, const std::string& def) {
    Stmt st(db_, "SELECT value FROM settings WHERE key = ?1;");
    st.bind(1, std::string(key));
    return st.step() ? st.text(0) : def;
}

void Library::setSetting(const char* key, const std::string& value) {
    Stmt st(db_, "INSERT INTO settings(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = ?2;");
    st.bind(1, std::string(key));
    st.bind(2, value);
    st.run();
}
