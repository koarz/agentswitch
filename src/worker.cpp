#include "worker.hpp"

#include <chrono>

#include "scanner.hpp"
#include "util.hpp"

namespace as {

namespace {

constexpr int64_t kPriceTtl = 24 * 3600;
constexpr int64_t kPriceRetry = 10 * 60;
constexpr auto kPollInterval = std::chrono::seconds(4);

// Refreshes prices when stale. Returns true when the book changed.
bool maybe_fetch_prices(Store& store, PriceBook& book, bool force, int64_t& retry_after,
                        std::string& error) {
    const int64_t now = now_unix();
    const int64_t at = store.prices_fetched_at();
    const bool stale = book.empty() || now - at > kPriceTtl;
    if (!(force || (stale && now >= retry_after))) return false;

    FetchResult fr = fetch_models_dev();
    if (!fr.ok) {
        error = fr.error;
        retry_after = now + kPriceRetry;
        return false;
    }
    store.replace_prices(fr.rows, now);
    book.load(fr.rows);
    error.clear();
    return true;
}

}  // namespace

Worker::Worker(std::unique_ptr<Store> store, Notify notify)
    : store_(std::move(store)), notify_(std::move(notify)) {}

Worker::~Worker() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Worker::start() { th_ = std::thread([this] { loop(); }); }

void Worker::refresh(bool force_prices) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_refresh_ = true;
        force_prices_ = force_prices_ || force_prices;
    }
    cv_.notify_all();
}

Worker::Snapshot Worker::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void Worker::loop() {
    int64_t price_retry_after = 0;
    std::string price_error;
    std::string last_day;
    bool dirty = true;

    book_.load(store_->prices());

    for (;;) {
        bool force = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            force = force_prices_;
            force_prices_ = false;
            want_refresh_ = false;
            snap_.busy = true;
        }
        if (notify_) notify_();

        std::string status;
        try {
            if (maybe_fetch_prices(*store_, book_, force, price_retry_after, price_error)) {
                dirty = true;
            }
            ScanResult sr = scan_all(*store_);
            const std::string today = local_today();
            if (sr.events > 0 || today != last_day || dirty) {
                auto rows = store_->daily();
                auto rep = std::make_shared<Report>(build_report(rows, book_, today));
                rep->events = store_->event_count();
                rep->last_event_ts = store_->last_event_ts();
                rep->prices_at = store_->prices_fetched_at();
                last_day = today;
                dirty = false;
                std::lock_guard<std::mutex> lk(mu_);
                snap_.report = std::move(rep);
            }
            status = sr.events > 0 ? "已同步 +" + std::to_string(sr.events) + " 条记录"
                                   : "已是最新";
        } catch (const std::exception& e) {
            status = std::string("错误: ") + e.what();
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.busy = false;
            snap_.status = status;
            snap_.price_error = price_error;
            snap_.scanned_at = now_unix();
        }
        if (notify_) notify_();

        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, kPollInterval, [this] { return quit_ || want_refresh_; });
        if (quit_) return;
    }
}

Report Worker::run_once(Store& store, PriceBook& book, bool force_prices, std::string* note) {
    int64_t retry_after = 0;
    std::string err;
    book.load(store.prices());
    maybe_fetch_prices(store, book, force_prices, retry_after, err);
    ScanResult sr = scan_all(store);

    auto rows = store.daily();
    Report rep = build_report(rows, book, local_today());
    rep.events = store.event_count();
    rep.last_event_ts = store.last_event_ts();
    rep.prices_at = store.prices_fetched_at();
    if (note) {
        *note = "files=" + std::to_string(sr.files) + " changed=" + std::to_string(sr.changed) +
                " new_events=" + std::to_string(sr.events);
        if (!err.empty()) *note += "\n价格: " + err;
    }
    return rep;
}

}  // namespace as
