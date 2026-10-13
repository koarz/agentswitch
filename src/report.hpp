#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pricing.hpp"
#include "types.hpp"

namespace as {

enum Period : int { kToday = 0, kWeek = 1, kMonth = 2, kTotal = 3, kPeriodCount = 4 };

inline const char* period_label(int p) {
    static const char* names[] = {"今日", "本周", "本月", "总计"};
    return names[p];
}

struct Sum {
    Tokens tok;
    double usd = 0;
    int64_t unpriced_tokens = 0;  // tokens whose model has no known price
};

struct ModelLine {
    Tool tool = Tool::Claude;
    std::string model;
    std::string priced_as;  // models.dev id the price came from; empty = unpriced
    Tokens tok;
    double usd = 0;
};

// One (day, tool, model) bucket of the whole history, already priced.
struct DailyRow {
    std::string day;  // local YYYY-MM-DD
    Tool tool = Tool::Claude;
    std::string model;
    Tokens tok;
    double usd = 0;
    bool priced = false;
};

// Everything the UI / CLI needs, already priced. Index 2 of `sums` is the
// combined total of both tools.
struct Report {
    std::string today;
    Sum sums[kPeriodCount][3];
    std::vector<ModelLine> models[kPeriodCount];  // sorted by cost, then tokens
    std::vector<DailyRow> daily;                  // whole history, oldest first
    int64_t events = 0;
    int64_t last_event_ts = 0;
    int64_t built_at = 0;
    size_t price_models = 0;
    int64_t prices_at = 0;
    bool has_unpriced = false;
};

Report build_report(const std::vector<DayRow>& rows, const PriceBook& book,
                    const std::string& today);

}  // namespace as
