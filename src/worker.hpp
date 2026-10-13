#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pricing.hpp"
#include "report.hpp"
#include "store.hpp"

namespace as {

// Background thread that owns the DuckDB connection: keeps prices fresh,
// ingests new log lines and publishes priced reports for the UI.
class Worker {
public:
    struct Snapshot {
        std::shared_ptr<const Report> report;  // null until the first scan finished
        bool busy = true;
        std::string status;       // short human readable line
        std::string price_error;  // last models.dev failure, empty when fine
        int64_t scanned_at = 0;
    };

    using Notify = std::function<void()>;

    Worker(std::unique_ptr<Store> store, Notify notify);
    ~Worker();

    void start();
    // Rescan right away; `force_prices` also re-downloads models.dev.
    void refresh(bool force_prices);
    Snapshot snapshot() const;

    // Synchronous single pass used by the non-interactive CLI commands.
    static Report run_once(Store& store, PriceBook& book, bool force_prices, std::string* note);

private:
    void loop();

    std::unique_ptr<Store> store_;
    Notify notify_;
    PriceBook book_;

    std::thread th_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool want_refresh_ = false;
    bool force_prices_ = false;
    Snapshot snap_;
};

}  // namespace as
