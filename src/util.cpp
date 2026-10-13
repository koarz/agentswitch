#include "util.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace as {

// ---- paths ----------------------------------------------------------------

static fs::path env_path(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? fs::path(v) : fs::path();
}

fs::path home_dir() {
    if (auto p = env_path("HOME"); !p.empty()) return p;
    return fs::path("/");
}

fs::path claude_dir() {
    if (auto p = env_path("CLAUDE_CONFIG_DIR"); !p.empty()) return p;
    return home_dir() / ".claude";
}

fs::path codex_dir() {
    if (auto p = env_path("CODEX_HOME"); !p.empty()) return p;
    return home_dir() / ".codex";
}

fs::path data_dir() {
    if (auto p = env_path("XDG_DATA_HOME"); !p.empty()) return p / "agentswitch";
    return home_dir() / ".local" / "share" / "agentswitch";
}

fs::path config_dir() {
    if (auto p = env_path("XDG_CONFIG_HOME"); !p.empty()) return p / "agentswitch";
    return home_dir() / ".config" / "agentswitch";
}

// ---- files ----------------------------------------------------------------

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file_atomic(const fs::path& p, std::string_view content,
                       fs::perms fresh_mode, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    fs::perms mode = fresh_mode;
    if (fs::exists(p, ec)) {
        auto st = fs::status(p, ec);
        if (!ec) mode = st.permissions();
    }

    fs::path tmp = p;
    tmp += ".agentswitch.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "无法写入 " + tmp.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            err = "写入失败: " + tmp.string();
            return false;
        }
    }
    fs::permissions(tmp, mode, ec);
    fs::rename(tmp, p, ec);
    if (ec) {
        err = "替换 " + p.string() + " 失败: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void backup_file(const fs::path& p, const std::string& suffix) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    fs::path dst = p;
    dst += suffix;
    fs::copy_file(p, dst, fs::copy_options::overwrite_existing, ec);
}

// ---- time -----------------------------------------------------------------

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

int64_t parse_iso8601(std::string_view s) {
    // 2026-09-27T05:32:39.438Z
    if (s.size() < 19) return -1;
    int y, mo, d, h, mi, se;
    std::string tmp(s.substr(0, 19));
    if (std::sscanf(tmp.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
        return -1;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    return static_cast<int64_t>(timegm(&tm));
}

static std::tm local_tm(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

std::string local_day(int64_t ts) {
    std::tm tm = local_tm(ts);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    return buf;
}

std::string local_hms(int64_t ts) {
    std::tm tm = local_tm(ts);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string local_today() { return local_day(now_unix()); }

static bool parse_day(const std::string& day, std::tm& tm) {
    int y, m, d;
    if (std::sscanf(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return false;
    tm = std::tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;  // keep away from DST edges
    return true;
}

static std::string fmt_day(const std::tm& tm) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    return buf;
}

std::string add_days(const std::string& day, int delta) {
    std::tm tm;
    if (!parse_day(day, tm)) return day;
    tm.tm_mday += delta;
    timegm(&tm);  // normalises the fields
    return fmt_day(tm);
}

int weekday_of(const std::string& day) {
    std::tm tm;
    if (!parse_day(day, tm)) return 0;
    timegm(&tm);
    return tm.tm_wday;
}

std::string week_start(const std::string& day) {
    std::tm tm;
    if (!parse_day(day, tm)) return day;
    timegm(&tm);  // fills tm_wday
    int since_monday = (tm.tm_wday + 6) % 7;
    return add_days(day, -since_monday);
}

std::string month_start(const std::string& day) {
    return day.size() >= 7 ? day.substr(0, 7) + "-01" : day;
}

std::string human_ago(int64_t s) {
    if (s < 0) s = 0;
    if (s < 60) return "刚刚";
    if (s < 3600) return std::to_string(s / 60) + " 分钟前";
    if (s < 86400) return std::to_string(s / 3600) + " 小时前";
    return std::to_string(s / 86400) + " 天前";
}

// ---- formatting -----------------------------------------------------------

std::string fmt_tokens(int64_t n) {
    char buf[32];
    double v = static_cast<double>(n);
    if (n < 1000) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
    } else if (n < 1000000) {
        std::snprintf(buf, sizeof buf, "%.1fK", v / 1e3);
    } else if (n < 1000000000) {
        std::snprintf(buf, sizeof buf, "%.2fM", v / 1e6);
    } else {
        std::snprintf(buf, sizeof buf, "%.2fB", v / 1e9);
    }
    return buf;
}

std::string fmt_usd(double v) {
    if (v > 0 && v < 0.01) return "<$0.01";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    // thousands separators on the integer part
    auto dot = s.find('.');
    std::string ip = s.substr(0, dot), fp = s.substr(dot);
    std::string out;
    int cnt = 0;
    for (auto it = ip.rbegin(); it != ip.rend(); ++it) {
        if (cnt && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++cnt;
    }
    std::reverse(out.begin(), out.end());
    return "$" + out + fp;
}

std::string mask_secret(std::string_view s) {
    if (s.empty()) return "—";
    if (s.size() <= 10) return std::string(s.size(), '*');
    return std::string(s.substr(0, 5)) + "…" + std::string(s.substr(s.size() - 4));
}

std::string url_host(std::string_view url) {
    auto p = url.find("://");
    if (p != std::string_view::npos) url.remove_prefix(p + 3);
    auto slash = url.find('/');
    std::string host(url.substr(0, slash));
    std::string path = slash == std::string_view::npos ? "" : std::string(url.substr(slash));
    while (!path.empty() && path.back() == '/') path.pop_back();
    return host + path;
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

}  // namespace as
