#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace as {

// In-memory price table (USD per million tokens) with tolerant model-name
// matching: logs say "claude-sonnet-4-5-20250929" while models.dev may only
// know "claude-sonnet-4-5", relays add prefixes, and so on.
class PriceBook {
public:
    void load(const std::vector<PriceRow>& rows);
    size_t size() const { return by_id_.size(); }
    bool empty() const { return by_id_.empty(); }

    // nullptr when the model cannot be matched to any known price.
    const PriceRow* resolve(const std::string& model) const;

    static double cost(const PriceRow& p, const Tokens& t);

private:
    std::unordered_map<std::string, PriceRow> by_id_;
    mutable std::unordered_map<std::string, const PriceRow*> memo_;
};

struct FetchResult {
    bool ok = false;
    std::string error;
    std::vector<PriceRow> rows;
};

// Downloads https://models.dev/api.json and keeps the anthropic + openai
// providers only.
FetchResult fetch_models_dev();

}  // namespace as
