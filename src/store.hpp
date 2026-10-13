#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "types.hpp"

namespace as {

// Thin DuckDB wrapper (C API). One instance owns one connection and must be
// used from a single thread.
class Store {
public:
    struct FileState {
        bool found = false;
        int64_t offset = 0;
        int64_t size = 0;
        int64_t mtime = 0;
        std::string model;      // codex: model of the last turn_context
        int64_t last_total = 0; // codex: last seen total_tokens
    };

    // Throws std::runtime_error (message is user presentable).
    explicit Store(const std::filesystem::path& db_path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void begin();
    void commit();

    FileState get_file(const std::string& path);
    void put_file(const std::string& path, const FileState& st);

    // Inserts new events; on key conflict keeps the per-field maximum.
    void upsert_events(const std::vector<UsageEvent>& events);
    // Token sums grouped by (day, tool, model).
    std::vector<DayRow> daily();
    int64_t event_count();
    int64_t last_event_ts();

    void replace_prices(const std::vector<PriceRow>& rows, int64_t fetched_at);
    std::vector<PriceRow> prices();
    int64_t prices_fetched_at();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace as
