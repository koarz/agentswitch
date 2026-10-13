#include "store.hpp"

#include <duckdb.h>

#include <stdexcept>

namespace as {

namespace {

// Owns a duckdb_result.
class Result {
public:
    Result() = default;
    ~Result() { duckdb_destroy_result(&r_); }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    duckdb_result* get() { return &r_; }

    idx_t rows() { return duckdb_row_count(&r_); }

    std::string str(idx_t col, idx_t row) {
        char* v = duckdb_value_varchar(&r_, col, row);
        std::string s = v ? v : "";
        if (v) duckdb_free(v);
        return s;
    }
    int64_t i64(idx_t col, idx_t row) { return duckdb_value_int64(&r_, col, row); }
    double f64(idx_t col, idx_t row) { return duckdb_value_double(&r_, col, row); }

private:
    duckdb_result r_{};
};

}  // namespace

struct Store::Impl {
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;

    void exec(const char* sql) {
        Result r;
        if (duckdb_query(con, sql, r.get()) != DuckDBSuccess) {
            throw std::runtime_error(std::string("DuckDB: ") + duckdb_result_error(r.get()));
        }
    }

    void query(const char* sql, Result& r) {
        if (duckdb_query(con, sql, r.get()) != DuckDBSuccess) {
            throw std::runtime_error(std::string("DuckDB: ") + duckdb_result_error(r.get()));
        }
    }

    // Prepared statement with positional string / int64 / double binds.
    struct Stmt {
        duckdb_prepared_statement s = nullptr;
        Stmt(duckdb_connection con, const char* sql) {
            if (duckdb_prepare(con, sql, &s) != DuckDBSuccess) {
                std::string msg = duckdb_prepare_error(s) ? duckdb_prepare_error(s) : "prepare failed";
                duckdb_destroy_prepare(&s);
                throw std::runtime_error("DuckDB: " + msg);
            }
        }
        ~Stmt() { duckdb_destroy_prepare(&s); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        void bind(idx_t i, const std::string& v) { duckdb_bind_varchar(s, i, v.c_str()); }
        void bind(idx_t i, int64_t v) { duckdb_bind_int64(s, i, v); }
        void run(Result& r) {
            if (duckdb_execute_prepared(s, r.get()) != DuckDBSuccess) {
                throw std::runtime_error(std::string("DuckDB: ") + duckdb_result_error(r.get()));
            }
        }
    };
};

Store::Store(const std::filesystem::path& db_path) : p_(std::make_unique<Impl>()) {
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);

    char* err = nullptr;
    if (duckdb_open_ext(db_path.c_str(), &p_->db, nullptr, &err) != DuckDBSuccess) {
        std::string msg = err ? err : "unknown error";
        if (err) duckdb_free(err);
        throw std::runtime_error("无法打开数据库 " + db_path.string() + ": " + msg);
    }
    if (duckdb_connect(p_->db, &p_->con) != DuckDBSuccess) {
        duckdb_close(&p_->db);
        throw std::runtime_error("无法连接 DuckDB");
    }

    p_->exec(R"sql(
        CREATE TABLE IF NOT EXISTS events(
            key VARCHAR PRIMARY KEY,
            ts BIGINT, day VARCHAR, tool VARCHAR, model VARCHAR,
            input BIGINT, output BIGINT, cache_read BIGINT, cache_write BIGINT);
        CREATE TABLE IF NOT EXISTS stage(
            key VARCHAR, ts BIGINT, day VARCHAR, tool VARCHAR, model VARCHAR,
            input BIGINT, output BIGINT, cache_read BIGINT, cache_write BIGINT);
        CREATE TABLE IF NOT EXISTS files(
            path VARCHAR PRIMARY KEY,
            byte_offset BIGINT, size BIGINT, mtime BIGINT,
            model VARCHAR, last_total BIGINT);
        CREATE TABLE IF NOT EXISTS prices(
            provider VARCHAR, model VARCHAR PRIMARY KEY, name VARCHAR,
            input DOUBLE, output DOUBLE, cache_read DOUBLE, cache_write DOUBLE);
        CREATE TABLE IF NOT EXISTS meta(k VARCHAR PRIMARY KEY, v VARCHAR);
        DELETE FROM stage;
    )sql");
}

Store::~Store() {
    if (p_) {
        if (p_->con) duckdb_disconnect(&p_->con);
        if (p_->db) duckdb_close(&p_->db);
    }
}

void Store::begin() { p_->exec("BEGIN TRANSACTION"); }
void Store::commit() { p_->exec("COMMIT"); }

Store::FileState Store::get_file(const std::string& path) {
    Impl::Stmt st(p_->con,
                  "SELECT byte_offset, size, mtime, model, last_total FROM files WHERE path = ?");
    st.bind(1, path);
    Result r;
    st.run(r);
    FileState f;
    if (r.rows() == 0) return f;
    f.found = true;
    f.offset = r.i64(0, 0);
    f.size = r.i64(1, 0);
    f.mtime = r.i64(2, 0);
    f.model = r.str(3, 0);
    f.last_total = r.i64(4, 0);
    return f;
}

void Store::put_file(const std::string& path, const FileState& s) {
    Impl::Stmt st(p_->con, "INSERT OR REPLACE INTO files VALUES (?, ?, ?, ?, ?, ?)");
    st.bind(1, path);
    st.bind(2, s.offset);
    st.bind(3, s.size);
    st.bind(4, s.mtime);
    st.bind(5, s.model);
    st.bind(6, s.last_total);
    Result r;
    st.run(r);
}

void Store::upsert_events(const std::vector<UsageEvent>& events) {
    if (events.empty()) return;

    duckdb_appender app = nullptr;
    if (duckdb_appender_create(p_->con, nullptr, "stage", &app) != DuckDBSuccess) {
        throw std::runtime_error("DuckDB: 无法创建 appender");
    }
    for (const auto& e : events) {
        duckdb_append_varchar(app, e.key.c_str());
        duckdb_append_int64(app, e.ts);
        duckdb_append_varchar(app, e.day.c_str());
        duckdb_append_varchar(app, tool_id(e.tool));
        duckdb_append_varchar(app, e.model.c_str());
        duckdb_append_int64(app, e.tok.input);
        duckdb_append_int64(app, e.tok.output);
        duckdb_append_int64(app, e.tok.cache_read);
        duckdb_append_int64(app, e.tok.cache_write);
        duckdb_appender_end_row(app);
    }
    duckdb_appender_close(app);
    duckdb_appender_destroy(&app);

    p_->exec(R"sql(
        INSERT INTO events
        SELECT key, any_value(ts), any_value(day), any_value(tool), any_value(model),
               max(input), max(output), max(cache_read), max(cache_write)
        FROM stage GROUP BY key
        ON CONFLICT (key) DO UPDATE SET
            input       = greatest(events.input,       excluded.input),
            output      = greatest(events.output,      excluded.output),
            cache_read  = greatest(events.cache_read,  excluded.cache_read),
            cache_write = greatest(events.cache_write, excluded.cache_write);
        DELETE FROM stage;
    )sql");
}

std::vector<DayRow> Store::daily() {
    Result r;
    p_->query(R"sql(
        SELECT day, tool, model,
               CAST(sum(input) AS BIGINT), CAST(sum(output) AS BIGINT),
               CAST(sum(cache_read) AS BIGINT), CAST(sum(cache_write) AS BIGINT),
               CAST(count(*) AS BIGINT)
        FROM events GROUP BY day, tool, model ORDER BY day, tool, model)sql",
              r);
    std::vector<DayRow> out;
    idx_t n = r.rows();
    out.reserve(n);
    for (idx_t i = 0; i < n; ++i) {
        DayRow d;
        d.day = r.str(0, i);
        d.tool = r.str(1, i) == "codex" ? Tool::Codex : Tool::Claude;
        d.model = r.str(2, i);
        d.tok.input = r.i64(3, i);
        d.tok.output = r.i64(4, i);
        d.tok.cache_read = r.i64(5, i);
        d.tok.cache_write = r.i64(6, i);
        d.events = r.i64(7, i);
        out.push_back(std::move(d));
    }
    return out;
}

int64_t Store::event_count() {
    Result r;
    p_->query("SELECT CAST(count(*) AS BIGINT) FROM events", r);
    return r.rows() ? r.i64(0, 0) : 0;
}

int64_t Store::last_event_ts() {
    Result r;
    p_->query("SELECT CAST(coalesce(max(ts), 0) AS BIGINT) FROM events", r);
    return r.rows() ? r.i64(0, 0) : 0;
}

void Store::replace_prices(const std::vector<PriceRow>& rows, int64_t fetched_at) {
    begin();
    try {
        p_->exec("DELETE FROM prices");
        duckdb_appender app = nullptr;
        if (duckdb_appender_create(p_->con, nullptr, "prices", &app) != DuckDBSuccess) {
            throw std::runtime_error("DuckDB: 无法创建 appender");
        }
        for (const auto& r : rows) {
            duckdb_append_varchar(app, r.provider.c_str());
            duckdb_append_varchar(app, r.model.c_str());
            duckdb_append_varchar(app, r.name.c_str());
            duckdb_append_double(app, r.input);
            duckdb_append_double(app, r.output);
            duckdb_append_double(app, r.cache_read);
            duckdb_append_double(app, r.cache_write);
            duckdb_appender_end_row(app);
        }
        duckdb_appender_close(app);
        duckdb_appender_destroy(&app);

        Impl::Stmt st(p_->con, "INSERT OR REPLACE INTO meta VALUES ('prices_fetched_at', ?)");
        st.bind(1, std::to_string(fetched_at));
        Result r;
        st.run(r);
    } catch (...) {
        try {
            p_->exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
    commit();
}

std::vector<PriceRow> Store::prices() {
    Result r;
    p_->query("SELECT provider, model, name, input, output, cache_read, cache_write FROM prices", r);
    std::vector<PriceRow> out;
    for (idx_t i = 0, n = r.rows(); i < n; ++i) {
        PriceRow p;
        p.provider = r.str(0, i);
        p.model = r.str(1, i);
        p.name = r.str(2, i);
        p.input = r.f64(3, i);
        p.output = r.f64(4, i);
        p.cache_read = r.f64(5, i);
        p.cache_write = r.f64(6, i);
        out.push_back(std::move(p));
    }
    return out;
}

int64_t Store::prices_fetched_at() {
    Result r;
    p_->query("SELECT v FROM meta WHERE k = 'prices_fetched_at'", r);
    if (r.rows() == 0) return 0;
    try {
        return std::stoll(r.str(0, 0));
    } catch (...) {
        return 0;
    }
}

}  // namespace as
