#include "pricing.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <nlohmann/json.hpp>

#include "util.hpp"

namespace as {

using json = nlohmann::json;

// ---- name matching ----------------------------------------------------------

namespace {

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](unsigned char c) { return std::isdigit(c); });
}

// "-20250929" or "-2024-08-06" suffix
bool strip_date_suffix(std::string& s) {
    if (s.size() > 9 && s[s.size() - 9] == '-' && all_digits(std::string_view(s).substr(s.size() - 8))) {
        s.resize(s.size() - 9);
        return true;
    }
    if (s.size() > 11) {
        std::string_view t(s);
        t = t.substr(t.size() - 11);
        if (t[0] == '-' && all_digits(t.substr(1, 4)) && t[5] == '-' &&
            all_digits(t.substr(6, 2)) && t[8] == '-' && all_digits(t.substr(9, 2))) {
            s.resize(s.size() - 11);
            return true;
        }
    }
    return false;
}

bool strip_suffix(std::string& s, std::string_view suf) {
    if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
        s.resize(s.size() - suf.size());
        return true;
    }
    return false;
}

// 4.5 -> 4-5 and 4-5 -> 4.5 (only between single digits)
std::string swap_version_sep(const std::string& s, char from, char to) {
    std::string r = s;
    for (size_t i = 1; i + 1 < r.size(); ++i) {
        if (r[i] == from && std::isdigit(static_cast<unsigned char>(r[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(r[i + 1])))
            r[i] = to;
    }
    return r;
}

std::vector<std::string> candidates(const std::string& model) {
    std::string m = to_lower(model);
    // provider prefixes: "anthropic/claude-x", "openai/gpt-x"
    if (auto p = m.rfind('/'); p != std::string::npos) m = m.substr(p + 1);
    // "[1m]" context markers and ":thinking" style tags
    if (auto p = m.find('['); p != std::string::npos) m.resize(p);
    if (auto p = m.find(':'); p != std::string::npos) m.resize(p);

    std::vector<std::string> out;
    auto add = [&](const std::string& s) {
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    add(m);
    std::string s = m;
    strip_suffix(s, "-latest");
    add(s);
    std::string d = s;
    if (strip_date_suffix(d)) add(d);
    // version separator variants of everything collected so far
    std::vector<std::string> base = out;
    for (const auto& b : base) {
        add(swap_version_sep(b, '.', '-'));
        add(swap_version_sep(b, '-', '.'));
    }
    return out;
}

}  // namespace

void PriceBook::load(const std::vector<PriceRow>& rows) {
    by_id_.clear();
    memo_.clear();
    for (const auto& r : rows) by_id_[to_lower(r.model)] = r;
}

const PriceRow* PriceBook::resolve(const std::string& model) const {
    if (auto it = memo_.find(model); it != memo_.end()) return it->second;

    const PriceRow* found = nullptr;
    auto cands = candidates(model);
    for (const auto& c : cands) {
        if (auto it = by_id_.find(c); it != by_id_.end()) {
            found = &it->second;
            break;
        }
    }
    if (!found) {
        // Longest known id that is a '-'-delimited prefix, e.g. a relay's
        // "gpt-5-codex-high" -> "gpt-5-codex".
        size_t best = 0;
        for (const auto& c : cands) {
            for (const auto& [id, row] : by_id_) {
                if (id.size() > best && c.size() > id.size() &&
                    c.compare(0, id.size(), id) == 0 && c[id.size()] == '-') {
                    best = id.size();
                    found = &row;
                }
            }
        }
    }
    memo_[model] = found;
    return found;
}

double PriceBook::cost(const PriceRow& p, const Tokens& t) {
    double cr = p.cache_read >= 0 ? p.cache_read : p.input;
    double cw = p.cache_write >= 0 ? p.cache_write : p.input;
    return (static_cast<double>(t.input) * p.input + static_cast<double>(t.output) * p.output +
            static_cast<double>(t.cache_read) * cr + static_cast<double>(t.cache_write) * cw) /
           1e6;
}

// ---- models.dev --------------------------------------------------------------

namespace {

size_t curl_write(char* p, size_t size, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, size * n);
    return size * n;
}

std::once_flag g_curl_once;

}  // namespace

FetchResult fetch_models_dev() {
    FetchResult res;
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* c = curl_easy_init();
    if (!c) {
        res.error = "curl 初始化失败";
        return res;
    }
    std::string body;
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, "https://models.dev/api.json");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "agentswitch/0.1");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        res.error = std::string("下载 models.dev 失败: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return res;
    }

    json doc = json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        res.error = "models.dev 返回了无法解析的数据";
        return res;
    }

    for (const char* provider : {"anthropic", "openai"}) {
        auto pit = doc.find(provider);
        if (pit == doc.end() || !pit->is_object()) continue;
        auto mit = pit->find("models");
        if (mit == pit->end() || !mit->is_object()) continue;
        for (auto& [id, m] : mit->items()) {
            if (!m.is_object()) continue;
            auto cit = m.find("cost");
            if (cit == m.end() || !cit->is_object()) continue;
            auto num = [&](const char* k, double dflt) {
                auto it = cit->find(k);
                return (it != cit->end() && it->is_number()) ? it->get<double>() : dflt;
            };
            PriceRow r;
            r.provider = provider;
            r.model = id;
            if (auto nit = m.find("name"); nit != m.end() && nit->is_string())
                r.name = nit->get<std::string>();
            r.input = num("input", -1);
            r.output = num("output", -1);
            if (r.input < 0 || r.output < 0) continue;
            r.cache_read = num("cache_read", -1);
            r.cache_write = num("cache_write", -1);
            res.rows.push_back(std::move(r));
        }
    }

    if (res.rows.empty()) {
        res.error = "models.dev 数据中没有找到 anthropic / openai 的价格";
        return res;
    }
    res.ok = true;
    return res;
}

}  // namespace as
