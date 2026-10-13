#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace as {

namespace fs = std::filesystem;

// ---- paths ----------------------------------------------------------------
fs::path home_dir();
fs::path claude_dir();  // $CLAUDE_CONFIG_DIR or ~/.claude
fs::path codex_dir();   // $CODEX_HOME or ~/.codex
fs::path data_dir();    // $XDG_DATA_HOME/agentswitch
fs::path config_dir();  // $XDG_CONFIG_HOME/agentswitch

// ---- files ----------------------------------------------------------------
std::optional<std::string> read_file(const fs::path& p);
// Writes through a temp file + rename. Existing permission bits are kept when
// the target exists, otherwise `fresh_mode` is used.
bool write_file_atomic(const fs::path& p, std::string_view content,
                       fs::perms fresh_mode, std::string& err);
// Copies `p` to `p + suffix` (best effort, keeps a single rolling backup).
void backup_file(const fs::path& p, const std::string& suffix);

// ---- time -----------------------------------------------------------------
int64_t now_unix();
int64_t parse_iso8601(std::string_view s);  // UTC seconds, -1 on failure
std::string local_day(int64_t ts);          // YYYY-MM-DD in local time
std::string local_hms(int64_t ts);          // HH:MM:SS in local time
std::string local_today();
std::string week_start(const std::string& day);   // Monday of that week
std::string month_start(const std::string& day);  // YYYY-MM-01
std::string add_days(const std::string& day, int delta);
int weekday_of(const std::string& day);  // 0 = Sunday .. 6 = Saturday
std::string human_ago(int64_t seconds);

// ---- formatting -----------------------------------------------------------
std::string fmt_tokens(int64_t n);  // 12.3K / 1.23M / 4.5B
std::string fmt_usd(double v);      // $1,234.56
std::string mask_secret(std::string_view s);
std::string url_host(std::string_view url);
std::string to_lower(std::string_view s);

}  // namespace as
