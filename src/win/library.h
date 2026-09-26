// Photo library stored in a SQLite database (images, thumbnails, settings, history).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "winutil.h"

struct sqlite3;

struct LibItem {
    int64_t id = 0;
    std::wstring name;
    int width = 0;
    int height = 0;
    int64_t added = 0;
    int64_t lastUsed = 0;
    int timesDrawn = 0;
};

class Library {
public:
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library();

    bool open(const std::wstring& path, std::wstring* error);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // Most recently used first; `filter` matches the name (case-insensitive).
    std::vector<LibItem> list(const std::wstring& filter);
    // Stores an image; identical bytes are not duplicated (the existing id is returned).
    int64_t add(const std::wstring& name, const std::wstring& source, const Bytes& data, int w, int h,
                const Bytes& thumbPng, bool* existed);
    bool data(int64_t id, Bytes& out);
    bool thumb(int64_t id, Bytes& out);
    bool item(int64_t id, LibItem& out);
    void rename(int64_t id, const std::wstring& name);
    void remove(int64_t id);
    void touch(int64_t id);
    void recordDrawing(int64_t id, const std::string& style, int strokes, double seconds, bool completed);

    std::string imageSettings(int64_t id);
    void setImageSettings(int64_t id, const std::string& s);
    std::string setting(const char* key, const std::string& def = std::string());
    void setSetting(const char* key, const std::string& value);

    // Groups several writes into one transaction.
    void begin() { exec("BEGIN;"); }
    void commit() { exec("COMMIT;"); }

private:
    bool exec(const char* sql);
    sqlite3* db_ = nullptr;
};
