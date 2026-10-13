#include "switcher.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

#include "util.hpp"

namespace as {

// ============================================================================
// Claude Code
// ============================================================================
namespace claude {

using ojson = nlohmann::ordered_json;

namespace {

fs::path profiles_path() { return config_dir() / "profiles.json"; }
fs::path settings_path() { return claude_dir() / "settings.json"; }

std::string norm_url(std::string u) {
    while (!u.empty() && u.back() == '/') u.pop_back();
    return to_lower(u);
}

std::string get_str(const ojson& o, const char* k) {
    if (!o.is_object()) return {};
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace

State load() {
    State st;

    if (auto txt = read_file(profiles_path())) {
        ojson j = ojson::parse(*txt, nullptr, false);
        if (j.is_object() && j.contains("claude") && j["claude"].is_array()) {
            for (const auto& e : j["claude"]) {
                Profile p;
                p.name = get_str(e, "name");
                p.base_url = get_str(e, "base_url");
                p.token = get_str(e, "token");
                if (!p.name.empty()) st.profiles.push_back(std::move(p));
            }
        }
    }

    if (auto txt = read_file(settings_path())) {
        ojson j = ojson::parse(*txt, nullptr, false);
        if (j.is_object() && j.contains("env") && j["env"].is_object()) {
            const auto& env = j["env"];
            st.active.base_url = get_str(env, "ANTHROPIC_BASE_URL");
            st.active.token = get_str(env, "ANTHROPIC_AUTH_TOKEN");
            if (st.active.token.empty()) st.active.token = get_str(env, "ANTHROPIC_API_KEY");
            st.active.has_override = !st.active.base_url.empty();
        }
    }

    if (st.active.has_override) {
        for (size_t i = 0; i < st.profiles.size(); ++i) {
            const auto& p = st.profiles[i];
            if (norm_url(p.base_url) == norm_url(st.active.base_url) &&
                (p.token == st.active.token || st.active.token.empty())) {
                st.active_index = static_cast<int>(i);
                break;
            }
        }
    }
    return st;
}

bool save_profiles(const std::vector<Profile>& profiles, std::string& err) {
    ojson j = ojson::object();
    ojson arr = ojson::array();
    for (const auto& p : profiles) {
        arr.push_back({{"name", p.name}, {"base_url", p.base_url}, {"token", p.token}});
    }
    j["claude"] = std::move(arr);
    return write_file_atomic(profiles_path(), j.dump(2) + "\n",
                             fs::perms::owner_read | fs::perms::owner_write, err);
}

bool apply(const Profile* profile, std::string& err) {
    const fs::path path = settings_path();
    ojson j = ojson::object();
    if (auto txt = read_file(path)) {
        j = ojson::parse(*txt, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            err = path.string() + " 不是合法的 JSON，已放弃修改";
            return false;
        }
    }

    if (!j.contains("env") || !j["env"].is_object()) j["env"] = ojson::object();
    ojson& env = j["env"];

    if (profile) {
        env["ANTHROPIC_BASE_URL"] = profile->base_url;
        if (!profile->token.empty()) {
            env["ANTHROPIC_AUTH_TOKEN"] = profile->token;
            env.erase("ANTHROPIC_API_KEY");  // would conflict with the token
        }
    } else {
        env.erase("ANTHROPIC_BASE_URL");
        env.erase("ANTHROPIC_AUTH_TOKEN");
    }
    if (env.empty()) j.erase("env");

    backup_file(path, ".agentswitch.bak");
    return write_file_atomic(path, j.dump(2) + "\n",
                             fs::perms::owner_read | fs::perms::owner_write, err);
}

}  // namespace claude

// ============================================================================
// Codex
// ============================================================================
namespace codex {

namespace {

fs::path config_path() { return codex_dir() / "config.toml"; }

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

struct Doc {
    std::vector<std::string> lines;

    static Doc parse(const std::string& text) {
        Doc d;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                d.lines.push_back(text.substr(pos));
                break;
            }
            d.lines.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return d;
    }

    std::string str() const {
        std::string out;
        for (const auto& l : lines) {
            out += l;
            out += '\n';
        }
        return out;
    }
};

// "[table.name]" -> "table.name"; empty when the line is not a table header.
std::string header_name(const std::string& line) {
    std::string t = trim(line);
    if (t.size() < 3 || t[0] != '[') return {};
    bool array = t.size() > 1 && t[1] == '[';
    size_t close = t.find(array ? "]]" : "]");
    if (close == std::string::npos) return {};
    return trim(t.substr(array ? 2 : 1, close - (array ? 2 : 1)));
}

bool is_header(const std::string& line) {
    std::string t = trim(line);
    return !t.empty() && t[0] == '[';
}

std::string unquote_key(std::string k) {
    k = trim(k);
    if (k.size() >= 2 && ((k.front() == '"' && k.back() == '"') || (k.front() == '\'' && k.back() == '\'')))
        return k.substr(1, k.size() - 2);
    return k;
}

// Reads `key = value` from a line; value is unquoted for strings.
bool parse_kv(const std::string& line, std::string& key, std::string& val) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '[') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = trim(t.substr(0, eq));
    std::string v = trim(t.substr(eq + 1));
    if (!v.empty() && v[0] == '"') {
        std::string out;
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size()) {
                char n = v[++i];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    default: out += n; break;
                }
            } else if (v[i] == '"') {
                break;
            } else {
                out += v[i];
            }
        }
        val = out;
    } else if (!v.empty() && v[0] == '\'') {
        size_t e = v.find('\'', 1);
        val = v.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    } else {
        size_t h = v.find('#');
        val = trim(h == std::string::npos ? v : v.substr(0, h));
    }
    return true;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// Index of the first table header (end of the top-level key area).
size_t top_level_end(const Doc& d) {
    for (size_t i = 0; i < d.lines.size(); ++i)
        if (is_header(d.lines[i])) return i;
    return d.lines.size();
}

bool key_is(const std::string& line, const char* key) {
    std::string k, v;
    return parse_kv(line, k, v) && k == key;
}

struct Block {
    size_t begin = 0;  // header line
    size_t end = 0;    // one past the last line of the block
};

bool find_provider_block(const Doc& d, const std::string& id, Block& out) {
    for (size_t i = 0; i < d.lines.size(); ++i) {
        std::string name = header_name(d.lines[i]);
        if (name.rfind("model_providers.", 0) != 0) continue;
        if (unquote_key(name.substr(16)) != id) continue;
        out.begin = i;
        out.end = i + 1;
        while (out.end < d.lines.size() && !is_header(d.lines[out.end])) ++out.end;
        return true;
    }
    return false;
}

void set_block_key(Doc& d, Block& b, const std::string& key, const std::string& value) {
    std::string line = key + " = " + quote(value);
    for (size_t i = b.begin + 1; i < b.end; ++i) {
        if (key_is(d.lines[i], key.c_str())) {
            d.lines[i] = line;
            return;
        }
    }
    // append after the last non-blank line of the block
    size_t at = b.end;
    while (at > b.begin + 1 && trim(d.lines[at - 1]).empty()) --at;
    d.lines.insert(d.lines.begin() + static_cast<long>(at), line);
    ++b.end;
}

bool save(const Doc& d, std::string& err) {
    backup_file(config_path(), ".agentswitch.bak");
    return write_file_atomic(config_path(), d.str(),
                             fs::perms::owner_read | fs::perms::owner_write, err);
}

bool load_doc(Doc& d, std::string& err) {
    auto txt = read_file(config_path());
    if (!txt) {
        err = "找不到 " + config_path().string();
        return false;
    }
    d = Doc::parse(*txt);
    return true;
}

}  // namespace

std::string slugify(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

State load() {
    State st;
    auto txt = read_file(config_path());
    if (!txt) return st;
    st.file_exists = true;
    Doc d = Doc::parse(*txt);

    size_t top = top_level_end(d);
    for (size_t i = 0; i < top; ++i) {
        std::string k, v;
        if (parse_kv(d.lines[i], k, v) && k == "model_provider") st.active_id = v;
    }

    for (size_t i = 0; i < d.lines.size(); ++i) {
        std::string name = header_name(d.lines[i]);
        if (name.rfind("model_providers.", 0) != 0) continue;
        std::string rest = name.substr(16);
        if (rest.find('.') != std::string::npos && rest.front() != '"') continue;  // sub-table
        Provider p;
        p.id = unquote_key(rest);
        for (size_t j = i + 1; j < d.lines.size() && !is_header(d.lines[j]); ++j) {
            std::string k, v;
            if (!parse_kv(d.lines[j], k, v)) continue;
            if (k == "name") p.name = v;
            else if (k == "base_url") p.base_url = v;
            else if (k == "experimental_bearer_token") p.token = v;
            else if (k == "env_key") p.env_key = v;
        }
        if (p.name.empty()) p.name = p.id;
        st.providers.push_back(std::move(p));
    }
    return st;
}

bool use(const std::string& id, std::string& err) {
    Doc d;
    if (!load_doc(d, err)) return false;

    size_t top = top_level_end(d);
    size_t active = std::string::npos, commented = std::string::npos;
    for (size_t i = 0; i < top; ++i) {
        if (key_is(d.lines[i], "model_provider")) {
            active = i;
            break;
        }
        std::string t = trim(d.lines[i]);
        if (commented == std::string::npos && !t.empty() && t[0] == '#') {
            std::string rest = trim(t.substr(1));
            if (rest.rfind("model_provider", 0) == 0 && rest.find('=') != std::string::npos &&
                rest.rfind("model_providers", 0) != 0)
                commented = i;
        }
    }

    if (id.empty()) {
        if (active == std::string::npos) return true;  // already default
        d.lines[active] = "#" + d.lines[active];
        return save(d, err);
    }

    const std::string line = "model_provider = " + quote(id);
    if (active != std::string::npos) {
        d.lines[active] = line;
    } else if (commented != std::string::npos) {
        d.lines.insert(d.lines.begin() + static_cast<long>(commented) + 1, line);
    } else {
        // top-level keys must come before the first table
        size_t at = top;
        while (at > 0 && trim(d.lines[at - 1]).empty()) --at;
        d.lines.insert(d.lines.begin() + static_cast<long>(at), line);
    }
    return save(d, err);
}

bool upsert(const Provider& p, std::string& err) {
    if (p.id.empty()) {
        err = "provider id 不能为空";
        return false;
    }
    Doc d;
    if (!load_doc(d, err)) return false;

    Block b;
    if (!find_provider_block(d, p.id, b)) {
        if (!d.lines.empty() && !trim(d.lines.back()).empty()) d.lines.push_back("");
        d.lines.push_back("[model_providers." + p.id + "]");
        b.begin = d.lines.size() - 1;
        b.end = d.lines.size();
        set_block_key(d, b, "name", p.name.empty() ? p.id : p.name);
        set_block_key(d, b, "base_url", p.base_url);
        set_block_key(d, b, "wire_api", "responses");
        if (!p.token.empty()) set_block_key(d, b, "experimental_bearer_token", p.token);
        return save(d, err);
    }

    if (!p.name.empty()) set_block_key(d, b, "name", p.name);
    set_block_key(d, b, "base_url", p.base_url);
    if (!p.token.empty()) set_block_key(d, b, "experimental_bearer_token", p.token);
    return save(d, err);
}

bool remove(const std::string& id, std::string& err) {
    Doc d;
    if (!load_doc(d, err)) return false;
    Block b;
    if (!find_provider_block(d, id, b)) {
        err = "找不到 provider: " + id;
        return false;
    }
    // swallow the blank separator that preceded the block
    size_t begin = b.begin;
    if (begin > 0 && trim(d.lines[begin - 1]).empty()) --begin;
    d.lines.erase(d.lines.begin() + static_cast<long>(begin), d.lines.begin() + static_cast<long>(b.end));
    return save(d, err);
}

}  // namespace codex

}  // namespace as
