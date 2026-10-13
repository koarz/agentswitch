#include "scanner.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "util.hpp"

namespace as {

using json = nlohmann::json;

namespace {

int64_t jint(const json& o, const char* key) {
    if (!o.is_object()) return 0;
    auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return 0;
    return it->get<int64_t>();
}

std::string jstr(const json& o, const char* key) {
    if (!o.is_object()) return {};
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

const json& jobj(const json& o, const char* key) {
    static const json null_json;
    if (!o.is_object()) return null_json;
    auto it = o.find(key);
    return it == o.end() ? null_json : *it;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

void collect_jsonl(const fs::path& root, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (fs::recursive_directory_iterator end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() == ".jsonl") out.push_back(it->path());
    }
}

// ---- Claude Code ----------------------------------------------------------
//
// ~/.claude/projects/<project>/<session>.jsonl, one JSON object per line.
// Assistant lines carry message.usage. The same API response is written
// several times (once per content block) and again when a session is resumed,
// so rows are keyed by message.id + requestId and merged with max().

void parse_claude_line(std::string_view line, std::vector<UsageEvent>& out) {
    if (!contains(line, "\"usage\"") || !contains(line, "\"assistant\"")) return;
    json o = json::parse(line.begin(), line.end(), nullptr, false);
    if (o.is_discarded() || jstr(o, "type") != "assistant") return;

    const json& msg = jobj(o, "message");
    const json& usage = jobj(msg, "usage");
    if (!usage.is_object()) return;

    std::string model = jstr(msg, "model");
    if (model.empty() || model.front() == '<') return;  // "<synthetic>"

    UsageEvent e;
    e.tool = Tool::Claude;
    e.model = std::move(model);
    e.tok.input = jint(usage, "input_tokens");
    e.tok.output = jint(usage, "output_tokens");
    e.tok.cache_read = jint(usage, "cache_read_input_tokens");
    e.tok.cache_write = jint(usage, "cache_creation_input_tokens");
    if (e.tok.total() == 0) return;

    e.ts = parse_iso8601(jstr(o, "timestamp"));
    if (e.ts < 0) return;
    e.day = local_day(e.ts);

    std::string id = jstr(msg, "id");
    std::string req = jstr(o, "requestId");
    if (!id.empty()) {
        e.key = "c:" + id + ":" + req;
    } else {
        std::string uuid = jstr(o, "uuid");
        if (uuid.empty()) return;
        e.key = "c:u:" + uuid;
    }
    out.push_back(std::move(e));
}

// ---- Codex ----------------------------------------------------------------
//
// ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl
//   turn_context                 -> payload.model (current model)
//   event_msg / token_count      -> payload.info.{total,last}_token_usage
// input_tokens already includes cached (and cache-written) tokens, so the
// fresh part is input - cached - cache_write. last_token_usage is the delta
// of one request; repeated token_count lines (same total) are skipped.

struct CodexState {
    std::string model;
    int64_t last_total = 0;
};

void parse_codex_line(std::string_view line, CodexState& st, std::vector<UsageEvent>& out) {
    bool is_ctx = contains(line, "\"turn_context\"");
    bool is_cnt = contains(line, "\"token_count\"");
    if (!is_ctx && !is_cnt) return;

    json o = json::parse(line.begin(), line.end(), nullptr, false);
    if (o.is_discarded()) return;
    const std::string type = jstr(o, "type");
    const json& payload = jobj(o, "payload");

    if (type == "turn_context") {
        std::string m = jstr(payload, "model");
        if (!m.empty()) st.model = std::move(m);
        return;
    }
    if (type != "event_msg" || jstr(payload, "type") != "token_count") return;

    const json& info = jobj(payload, "info");
    if (!info.is_object()) return;
    const json& total = jobj(info, "total_token_usage");
    const json& last = jobj(info, "last_token_usage");
    if (!last.is_object()) return;

    int64_t total_tokens = jint(total, "total_tokens");
    if (total_tokens != 0 && total_tokens == st.last_total) return;  // repeated report
    st.last_total = total_tokens;

    int64_t in = jint(last, "input_tokens");
    int64_t cached = jint(last, "cached_input_tokens");
    int64_t written = jint(last, "cache_write_input_tokens");

    UsageEvent e;
    e.tool = Tool::Codex;
    e.model = st.model.empty() ? "unknown" : st.model;
    e.tok.cache_read = cached;
    e.tok.cache_write = written;
    e.tok.input = std::max<int64_t>(0, in - cached - written);
    e.tok.output = jint(last, "output_tokens");
    if (e.tok.total() == 0) return;

    e.ts = parse_iso8601(jstr(o, "timestamp"));
    if (e.ts < 0) return;
    e.day = local_day(e.ts);
    // Forked / resumed sessions replay earlier events with the same timestamp
    // and totals, so the key deliberately ignores the session id.
    e.key = "x:" + jstr(o, "timestamp") + ":" + std::to_string(total_tokens) + ":" +
            std::to_string(jint(total, "input_tokens")) + ":" +
            std::to_string(jint(total, "output_tokens"));
    out.push_back(std::move(e));
}

// ---- driver ----------------------------------------------------------------

struct FileStat {
    int64_t size = 0;
    int64_t mtime = 0;
};

bool stat_file(const fs::path& p, FileStat& fs_out) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return false;
    fs_out.size = st.st_size;
    fs_out.mtime = static_cast<int64_t>(st.st_mtime);
    return true;
}

void scan_file(Store& store, const fs::path& path, Tool tool, ScanResult& res,
               std::vector<UsageEvent>& pending) {
    FileStat st;
    if (!stat_file(path, st)) return;
    ++res.files;

    const std::string key = path.string();
    Store::FileState cur = store.get_file(key);
    if (cur.found && cur.size == st.size && cur.mtime == st.mtime) return;
    if (st.size < cur.offset) cur = Store::FileState{};  // truncated: start over

    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    in.seekg(cur.offset);
    std::string buf;
    {
        std::string chunk(1 << 20, '\0');
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            buf.append(chunk.data(), static_cast<size_t>(in.gcount()));
        }
    }

    // Only whole lines; a half-written trailing line is picked up next time.
    size_t end = buf.rfind('\n');
    size_t usable = end == std::string::npos ? 0 : end + 1;

    CodexState cx{cur.model, cur.last_total};
    size_t before = pending.size();
    size_t pos = 0;
    while (pos < usable) {
        size_t nl = buf.find('\n', pos);
        std::string_view line(buf.data() + pos, nl - pos);
        if (!line.empty()) {
            if (tool == Tool::Claude) {
                parse_claude_line(line, pending);
            } else {
                parse_codex_line(line, cx, pending);
            }
        }
        pos = nl + 1;
    }

    res.bytes += static_cast<int64_t>(usable);
    res.events += static_cast<int64_t>(pending.size() - before);
    if (usable > 0 || !cur.found) ++res.changed;

    Store::FileState next;
    next.found = true;
    next.offset = cur.offset + static_cast<int64_t>(usable);
    next.size = st.size;
    next.mtime = st.mtime;
    next.model = cx.model;
    next.last_total = cx.last_total;
    store.put_file(key, next);
}

}  // namespace

ScanResult scan_all(Store& store) {
    ScanResult res;

    std::vector<fs::path> claude_files, codex_files;
    collect_jsonl(claude_dir() / "projects", claude_files);
    if (!std::getenv("CLAUDE_CONFIG_DIR")) {
        collect_jsonl(home_dir() / ".config" / "claude" / "projects", claude_files);
    }
    collect_jsonl(codex_dir() / "sessions", codex_files);
    collect_jsonl(codex_dir() / "archived_sessions", codex_files);

    std::sort(claude_files.begin(), claude_files.end());
    std::sort(codex_files.begin(), codex_files.end());

    // Events and file cursors are committed together, so a crash can never
    // advance a cursor without its events.
    std::vector<UsageEvent> pending;
    store.begin();
    for (const auto& p : claude_files) scan_file(store, p, Tool::Claude, res, pending);
    for (const auto& p : codex_files) scan_file(store, p, Tool::Codex, res, pending);
    store.upsert_events(pending);
    store.commit();
    return res;
}

}  // namespace as
