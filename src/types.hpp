#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace as {

enum class Tool : int { Claude = 0, Codex = 1 };

inline const char* tool_id(Tool t) { return t == Tool::Claude ? "claude" : "codex"; }
inline const char* tool_title(Tool t) { return t == Tool::Claude ? "Claude Code" : "Codex"; }

// All counters are *disjoint* buckets, so they can be priced independently:
//   input       fresh (uncached) input tokens
//   output      output tokens (includes reasoning / thinking)
//   cache_read  input tokens served from cache
//   cache_write input tokens written to cache
struct Tokens {
    int64_t input = 0;
    int64_t output = 0;
    int64_t cache_read = 0;
    int64_t cache_write = 0;

    int64_t total() const { return input + output + cache_read + cache_write; }
    Tokens& operator+=(const Tokens& o) {
        input += o.input;
        output += o.output;
        cache_read += o.cache_read;
        cache_write += o.cache_write;
        return *this;
    }
};

// One billable API response, as found in a local session log.
struct UsageEvent {
    std::string key;  // globally unique, used for de-duplication
    int64_t ts = 0;   // unix seconds, UTC
    std::string day;  // local YYYY-MM-DD
    Tool tool = Tool::Claude;
    std::string model;
    Tokens tok;
};

// Aggregated row coming back from the store.
struct DayRow {
    std::string day;
    Tool tool = Tool::Claude;
    std::string model;
    Tokens tok;
    int64_t events = 0;
};

// USD per million tokens (models.dev convention). Negative = not published.
struct PriceRow {
    std::string provider;  // anthropic | openai
    std::string model;     // models.dev id
    std::string name;
    double input = 0;
    double output = 0;
    double cache_read = -1;
    double cache_write = -1;
};

}  // namespace as
