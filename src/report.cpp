#include "report.hpp"

#include <algorithm>
#include <map>

#include "util.hpp"

namespace as {

Report build_report(const std::vector<DayRow>& rows, const PriceBook& book,
                    const std::string& today) {
    Report rep;
    rep.today = today;
    rep.built_at = now_unix();
    rep.price_models = book.size();

    const std::string wk = week_start(today);
    const std::string mo = month_start(today);

    std::map<std::pair<int, std::string>, ModelLine> models[kPeriodCount];

    auto add = [](Sum& s, const Tokens& t, double usd, bool priced) {
        s.tok += t;
        s.usd += usd;
        if (!priced) s.unpriced_tokens += t.total();
    };

    for (const auto& r : rows) {
        const PriceRow* price = book.resolve(r.model);
        const double usd = price ? PriceBook::cost(*price, r.tok) : 0.0;
        const bool priced = price != nullptr;
        const int ti = static_cast<int>(r.tool);

        bool in[kPeriodCount] = {r.day == today, r.day >= wk && r.day <= today,
                                 r.day >= mo && r.day <= today, true};
        for (int p = 0; p < kPeriodCount; ++p) {
            if (!in[p]) continue;
            add(rep.sums[p][ti], r.tok, usd, priced);
            add(rep.sums[p][2], r.tok, usd, priced);

            auto& line = models[p][{ti, r.model}];
            line.tool = r.tool;
            line.model = r.model;
            line.tok += r.tok;
            line.usd += usd;
            if (priced) line.priced_as = price->model;
            if (!priced) rep.has_unpriced = true;
        }

        DailyRow dr;
        dr.day = r.day;
        dr.tool = r.tool;
        dr.model = r.model;
        dr.tok = r.tok;
        dr.usd = usd;
        dr.priced = priced;
        rep.daily.push_back(std::move(dr));
        rep.events += r.events;
    }

    for (int p = 0; p < kPeriodCount; ++p) {
        for (auto& [k, v] : models[p]) rep.models[p].push_back(std::move(v));
        std::sort(rep.models[p].begin(), rep.models[p].end(),
                  [](const ModelLine& a, const ModelLine& b) {
                      if (a.usd != b.usd) return a.usd > b.usd;
                      return a.tok.total() > b.tok.total();
                  });
    }
    return rep;
}

}  // namespace as
