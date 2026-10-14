#include "switcher.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

#include "util.hpp"

namespace as {

namespace {

using ojson = nlohmann::ordered_json;

fs::path profiles_path() { return config_dir() / "profiles.json"; }

std::string get_str(const ojson& o, const char* k) {
    if (!o.is_object()) return {};
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool load_profiles_doc(ojson& j, std::string& err) {
    j = ojson::object();
    const auto path = profiles_path();
    if (auto txt = read_file(path)) {
        j = ojson::parse(*txt, nullptr, false);
        if (!j.is_object()) {
            err = path.string() + " 不是合法的 JSON 对象，已放弃修改";
            return false;
        }
    } else {
        std::error_code ec;
        if (fs::exists(path, ec) || ec) {
            err = "无法读取 " + path.string();
            return false;
        }
    }
    return true;
}

bool save_profiles_doc(const ojson& j, std::string& err) {
    backup_file(profiles_path(), ".agentswitch.bak");
    return write_file_atomic(profiles_path(), j.dump(2) + "\n",
                             fs::perms::owner_read | fs::perms::owner_write, err);
}

bool save_native_with_profiles(const fs::path& path, const std::string& content,
                               const ojson& before, const ojson& after, std::string& err) {
    if (before != after && !save_profiles_doc(after, err)) return false;
    backup_file(path, ".agentswitch.bak");
    if (write_file_atomic(path, content, fs::perms::owner_read | fs::perms::owner_write, err))
        return true;
    if (before != after) {
        std::string rollback_error;
        if (!write_file_atomic(profiles_path(), before.dump(2) + "\n",
                               fs::perms::owner_read | fs::perms::owner_write, rollback_error))
            err += "；恢复模型配置失败: " + rollback_error;
    }
    return false;
}

// Restore only values that still match our last write, preserving later
// external edits; remember the originals before applying the next relay.
void update_managed_values(ojson& target, const ojson& desired, ojson& state) {
    if (state.is_object()) {
        for (const auto& [key, saved] : state.items()) {
            if (!saved.is_object() || !saved.contains("applied")) continue;
            if (target.contains(key) && target[key] == saved["applied"]) {
                if (saved.value("present", false)) target[key] = saved["value"];
                else target.erase(key);
            }
        }
    }
    ojson next = ojson::object();
    for (const auto& [key, value] : desired.items()) {
        next[key] = {{"present", target.contains(key)},
                     {"value", target.contains(key) ? target[key] : ojson(nullptr)},
                     {"applied", value}};
        target[key] = value;
    }
    state = std::move(next);
}

bool valid_model(const std::string& model, std::string& err) {
    for (unsigned char c : model) {
        if (c <= 0x20 || c == 0x7f) {
            err = "模型 ID 不能包含空白或控制字符";
            return false;
        }
    }
    return true;
}

bool normalize_models(std::string& selected, std::vector<ModelOption>& models, std::string& err) {
    if (!valid_model(selected, err)) return false;
    std::vector<ModelOption> unique;
    for (auto option : models) {
        if (option.name.empty()) option.name = option.id;
        if (option.id.empty() || !valid_model(option.name, err) || !valid_model(option.id, err)) {
            if (err.empty()) err = "模型名称和映射目标不能为空";
            return false;
        }
        const auto existing = std::find_if(unique.begin(), unique.end(), [&](const ModelOption& m) {
            return m.name == option.name;
        });
        if (existing != unique.end()) {
            if (existing->id != option.id) {
                err = "模型名称重复: " + option.name;
                return false;
            }
        } else {
            unique.push_back(std::move(option));
        }
    }
    if (unique.empty() && !selected.empty()) unique.push_back({selected, selected});
    if (!unique.empty() && std::none_of(unique.begin(), unique.end(), [&](const ModelOption& m) {
            return m.id == selected;
        }))
        selected = unique.front().id;
    models = std::move(unique);
    return true;
}

void read_model_settings(const ojson& j, std::string& selected, std::vector<ModelOption>& models) {
    selected = j.is_string() ? j.get<std::string>() : get_str(j, "model");
    models.clear();
    if (j.is_object() && j.contains("models") && j["models"].is_array()) {
        for (const auto& entry : j["models"]) {
            if (entry.is_string()) {
                const auto id = entry.get<std::string>();
                models.push_back({id, id});
            } else if (entry.is_object()) {
                models.push_back({get_str(entry, "name"), get_str(entry, "id")});
            }
        }
    }
    std::string ignored;
    normalize_models(selected, models, ignored);
}

ojson model_settings(const std::string& selected, const std::vector<ModelOption>& models) {
    ojson entries = ojson::array();
    for (const auto& m : models) entries.push_back({{"name", m.name}, {"id", m.id}});
    return {{"model", selected}, {"models", std::move(entries)}};
}

void provider_models(const ojson& j, const std::string& id, std::string& selected,
                     std::vector<ModelOption>& models) {
    const auto it = j.find("codex_models");
    if (it != j.end() && it->is_object() && it->contains(id))
        read_model_settings((*it)[id], selected, models);
}

}  // namespace

bool parse_model_list(const std::string& text, std::vector<ModelOption>& models, std::string& err) {
    err.clear();
    auto input = text;
    for (const std::string separator : {"，", "、", "；"}) {
        size_t at = 0;
        while ((at = input.find(separator, at)) != std::string::npos)
            input.replace(at, separator.size(), ",");
    }
    for (char& c : input)
        if (c == '\n' || c == '\r' || c == ';') c = ',';
    auto trimmed = [](std::string_view value) {
        size_t begin = 0, end = value.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
        return std::string(value.substr(begin, end - begin));
    };
    std::vector<ModelOption> parsed;
    for (size_t begin = 0; begin < input.size();) {
        const auto comma = input.find(',', begin);
        const auto item = trimmed(std::string_view(input).substr(begin, comma == std::string::npos
                                                                         ? input.size() - begin
                                                                         : comma - begin));
        if (!item.empty()) {
            const auto eq = item.find('=');
            ModelOption m;
            if (eq == std::string::npos) {
                m.name = m.id = item;
            } else {
                m.name = trimmed(std::string_view(item).substr(0, eq));
                m.id = trimmed(std::string_view(item).substr(eq + 1));
                if (m.name.empty() || m.id.empty()) {
                    err = "映射格式为 名称=中转模型ID，两边都不能为空";
                    return false;
                }
            }
            parsed.push_back(std::move(m));
        }
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    std::string selected;
    if (!normalize_models(selected, parsed, err)) return false;
    models = std::move(parsed);
    return true;
}

// ============================================================================
// Claude Code
// ============================================================================
namespace claude {

namespace {

fs::path settings_path() { return claude_dir() / "settings.json"; }

std::string norm_url(std::string u) {
    while (!u.empty() && u.back() == '/') u.pop_back();
    return to_lower(u);
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
                read_model_settings(e, p.model, p.models);
                if (!p.name.empty()) st.profiles.push_back(std::move(p));
            }
        }
    }

    if (auto txt = read_file(settings_path())) {
        ojson j = ojson::parse(*txt, nullptr, false);
        if (j.is_object()) {
            st.active.model = get_str(j, "model");
            if (j.contains("env") && j["env"].is_object()) {
                const auto& env = j["env"];
                st.active.base_url = get_str(env, "ANTHROPIC_BASE_URL");
                st.active.token = get_str(env, "ANTHROPIC_AUTH_TOKEN");
                if (st.active.token.empty()) st.active.token = get_str(env, "ANTHROPIC_API_KEY");
                if (auto model = get_str(env, "ANTHROPIC_MODEL"); !model.empty())
                    st.active.model = std::move(model);
                st.active.has_override = !st.active.base_url.empty();
            }
        }
    }

    if (st.active.has_override) {
        for (size_t i = 0; i < st.profiles.size(); ++i) {
            const auto& p = st.profiles[i];
            if (norm_url(p.base_url) == norm_url(st.active.base_url) &&
                (p.token == st.active.token || st.active.token.empty()) &&
                (p.models.empty() || std::any_of(p.models.begin(), p.models.end(), [&](const ModelOption& m) {
                    return m.id == st.active.model;
                }))) {
                if (st.active_index < 0 || !p.models.empty())
                    st.active_index = static_cast<int>(i);
                if (!p.models.empty()) break;
            }
        }
    }
    return st;
}

bool save_profiles(const std::vector<Profile>& profiles, std::string& err) {
    ojson j;
    if (!load_profiles_doc(j, err)) return false;
    ojson arr = ojson::array();
    for (auto p : profiles) {
        if (!normalize_models(p.model, p.models, err)) return false;
        auto entry = model_settings(p.model, p.models);
        entry["name"] = p.name;
        entry["base_url"] = p.base_url;
        entry["token"] = p.token;
        arr.push_back(std::move(entry));
    }
    j["claude"] = std::move(arr);
    return save_profiles_doc(j, err);
}

bool apply(const Profile* profile, std::string& err) {
    Profile normalized;
    if (profile) {
        normalized = *profile;
        if (!normalize_models(normalized.model, normalized.models, err)) return false;
        profile = &normalized;
    }
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
    ojson profiles;
    if (!load_profiles_doc(profiles, err)) return false;
    const auto before = profiles;
    ojson state = profiles.contains("claude_cli") && profiles["claude_cli"].is_object()
                      ? profiles["claude_cli"] : ojson::object();
    ojson alias_env = ojson::object(), overrides = ojson::object(), picker_settings = ojson::object();
    ojson options = ojson::array();
    if (profile) {
        for (const auto& m : profile->models) {
            const auto name = to_lower(m.name);
            const bool family = name == "sonnet" || name == "opus" || name == "haiku" || name == "fable";
            const bool mapped = m.name != m.id;
            if (mapped && family) {
                auto upper = name;
                std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                alias_env["ANTHROPIC_DEFAULT_" + upper + "_MODEL"] = m.id;
            } else if (mapped && m.name.rfind("claude-", 0) == 0) {
                overrides[m.name] = m.id;
            }
            options.push_back({{"model", mapped && family ? name
                                          : mapped && m.name.rfind("claude-", 0) == 0 ? m.name : m.id},
                               {"label", m.name}, {"description", "中转模型: " + m.id}});
        }
        if (!options.empty())
            picker_settings["modelPicker"] = {{"replaceBuiltInOptions", true}, {"options", std::move(options)}};
    }
    update_managed_values(j["env"], alias_env, state["env"]);
    if (!overrides.empty() || (state.contains("overrides") && !state["overrides"].empty())) {
        if (j.contains("modelOverrides") && !j["modelOverrides"].is_object()) {
            err = "settings.json 的 modelOverrides 必须是 JSON 对象，已放弃修改";
            return false;
        }
        if (!j.contains("modelOverrides")) j["modelOverrides"] = ojson::object();
        update_managed_values(j["modelOverrides"], overrides, state["overrides"]);
        if (j["modelOverrides"].empty()) j.erase("modelOverrides");
    }
    update_managed_values(j, picker_settings, state["settings"]);
    if (!state["env"].empty() || (state.contains("overrides") && !state["overrides"].empty()) ||
        !state["settings"].empty())
        profiles["claude_cli"] = std::move(state);
    else
        profiles.erase("claude_cli");

    ojson& env = j["env"];
    if (profile) {
        env["ANTHROPIC_BASE_URL"] = profile->base_url;
        if (!profile->model.empty()) env["ANTHROPIC_MODEL"] = profile->model;
        if (!profile->token.empty()) {
            env["ANTHROPIC_AUTH_TOKEN"] = profile->token;
            env.erase("ANTHROPIC_API_KEY");  // would conflict with the token
        }
    } else {
        env.erase("ANTHROPIC_BASE_URL");
        env.erase("ANTHROPIC_AUTH_TOKEN");
    }
    if (env.empty()) j.erase("env");

    return save_native_with_profiles(path, j.dump(2) + "\n", before, profiles, err);
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
    key = unquote_key(t.substr(0, eq));
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
    return ojson(s).dump();  // JSON escaping is also valid for TOML basic strings.
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

std::string top_level_value(const Doc& d, const char* key) {
    for (size_t i = 0, end = top_level_end(d); i < end; ++i) {
        std::string k, v;
        if (parse_kv(d.lines[i], k, v) && k == key) return v;
    }
    return {};
}

// Locate comments without mistaking a '#' inside a string for a comment.
size_t comment_start(const std::string& line) {
    char quoted = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted == '"' && c == '\\') { ++i; continue; }
        if (quoted) {
            if (c == quoted) quoted = 0;
        } else if (c == '"' || c == '\'') {
            quoted = c;
        } else if (c == '#') {
            return i;
        }
    }
    return std::string::npos;
}

// Empty value disables the key. New keys always go before the first table.
bool set_top_level_key(Doc& d, const std::string& key, const std::string& value) {
    const size_t top = top_level_end(d);
    size_t commented = std::string::npos;
    for (size_t i = 0; i < top; ++i) {
        if (key_is(d.lines[i], key.c_str())) {
            auto& line = d.lines[i];
            if (value.empty()) {
                line = "#" + line;
            } else {
                size_t start = line.find('=') + 1;
                while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
                    ++start;
                size_t comment = comment_start(line);
                if (comment != std::string::npos)
                    while (comment > start && std::isspace(static_cast<unsigned char>(line[comment - 1])))
                        --comment;
                const auto updated = line.substr(0, start) + quote(value) +
                                     (comment == std::string::npos ? "" : line.substr(comment));
                if (line == updated) return false;
                line = updated;
            }
            return true;
        }
        const auto t = trim(d.lines[i]);
        if (commented == std::string::npos && !t.empty() && t[0] == '#' &&
            key_is(trim(t.substr(1)), key.c_str()))
            commented = i;
    }
    if (value.empty()) return false;

    size_t at = commented != std::string::npos ? commented + 1 : top;
    if (commented == std::string::npos)
        while (at > 0 && trim(d.lines[at - 1]).empty()) --at;
    d.lines.insert(d.lines.begin() + static_cast<long>(at), key + " = " + quote(value));
    return true;
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

bool save_provider_changes(const Doc& d, const ojson& before, const ojson& after,
                           std::string& err) {
    return save_native_with_profiles(config_path(), d.str(), before, after, err);
}

bool configure_catalog(Doc& d, const std::string& id, const std::vector<ModelOption>& models,
                       ojson& profiles, std::string& err) {
    const auto current = top_level_value(d, "model_catalog_json");
    const auto saved = profiles.contains("codex_cli") && profiles["codex_cli"].is_object()
                           ? profiles["codex_cli"] : ojson::object();
    const bool managed = !get_str(saved, "path").empty() && current == get_str(saved, "path");
    if (models.empty()) {
        if (managed) set_top_level_key(d, "model_catalog_json", get_str(saved, "previous"));
        profiles.erase("codex_cli");
        return true;
    }

    // Hex-encode the provider ID so imported IDs cannot escape this directory.
    const char* hex = "0123456789abcdef";
    std::string filename;
    for (unsigned char c : id) { filename += hex[c >> 4]; filename += hex[c & 15]; }
    const auto path = fs::absolute(config_dir() / "codex-models" / (filename + ".json"));
    ojson templates = ojson::array();
    if (auto cache = read_file(codex_dir() / "models_cache.json")) {
        const auto j = ojson::parse(*cache, nullptr, false);
        if (j.is_object() && j.contains("models") && j["models"].is_array()) templates = j["models"];
    }
    const auto original_catalog = managed ? get_str(saved, "previous") : current;
    if (!original_catalog.empty()) {
        auto template_path = fs::path(original_catalog);
        if (template_path.is_relative()) template_path = codex_dir() / template_path;
        if (auto source = read_file(template_path)) {
            const auto j = ojson::parse(*source, nullptr, false);
            if (j.is_object() && j.contains("models") && j["models"].is_array())
                for (const auto& entry : j["models"]) templates.push_back(entry);
        }
    }
    if (managed) {
        if (auto source = read_file(current)) {
            const auto j = ojson::parse(*source, nullptr, false);
            if (j.is_object() && j.contains("models") && j["models"].is_array())
                for (const auto& entry : j["models"]) templates.push_back(entry);
        }
    }
    ojson entries = ojson::array();
    std::vector<std::string> ids;
    for (const auto& m : models) {
        if (std::find(ids.begin(), ids.end(), m.id) != ids.end()) continue;
        ids.push_back(m.id);
        ojson entry;
        for (auto it = templates.rbegin(); it != templates.rend(); ++it) {
            if (get_str(*it, "slug") == m.id) { entry = *it; break; }
        }
        if (!entry.is_object()) {
            // Unknown/old models use conservative capabilities, rather than
            // inheriting the newest model's reasoning or tool protocol.
            entry = {{"default_reasoning_level", nullptr}, {"supported_reasoning_levels", ojson::array()},
                     {"shell_type", "shell_command"}, {"support_verbosity", false},
                     {"context_window", 8192}, {"effective_context_window_percent", 95},
                     {"default_reasoning_summary", "none"},
                     {"experimental_supported_tools", ojson::array()},
                     {"truncation_policy", {{"mode", "tokens"}, {"limit", 4096}}},
                     {"base_instructions", "You are a coding assistant. Use the provided tools to assist the user."}};
        }
        entry["slug"] = m.id;
        entry["display_name"] = m.name;
        entry["description"] = "中转模型: " + m.id;
        entry["visibility"] = "list";
        entry["supported_in_api"] = true;
        entry["priority"] = static_cast<int>(entries.size());
        entry["upgrade"] = nullptr;
        entry["availability_nux"] = nullptr;
        entry["use_responses_lite"] = false;
        entry["additional_speed_tiers"] = ojson::array();
        entry["service_tiers"] = ojson::array();
        entries.push_back(std::move(entry));
    }
    // Each update gets a new path so a failed config write never changes the
    // catalog still referenced by the previous active configuration.
    ojson catalog = {{"models", std::move(entries)}};
    const auto content = catalog.dump(2) + "\n";
    auto written_path = path;
    const auto old = read_file(path);
    const auto active_content = managed ? read_file(current) : std::nullopt;
    if (active_content && *active_content == content) {
        written_path = current;
    } else if (old && *old != content) {
        written_path = path.parent_path() / (filename + "-" + std::to_string(now_unix()) + ".json");
        if (auto existing = read_file(written_path); existing && *existing != content) {
            unsigned n = 2;
            do {
                written_path = path.parent_path() / (filename + "-" + std::to_string(now_unix()) +
                                                     "-" + std::to_string(n++) + ".json");
            } while (fs::exists(written_path));
        }
    }
    if (!write_file_atomic(written_path, content, fs::perms::owner_read | fs::perms::owner_write, err))
        return false;
    profiles["codex_cli"] = {{"previous", managed ? get_str(saved, "previous") : current},
                              {"path", written_path.string()}};
    set_top_level_key(d, "model_catalog_json", written_path.string());
    return true;
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

    st.active_id = top_level_value(d, "model_provider");
    st.active_model = top_level_value(d, "model");
    ojson profiles = ojson::object();
    if (auto saved = read_file(profiles_path())) {
        auto j = ojson::parse(*saved, nullptr, false);
        if (j.is_object()) profiles = std::move(j);
    }

    for (size_t i = 0; i < d.lines.size(); ++i) {
        std::string name = header_name(d.lines[i]);
        if (name.rfind("model_providers.", 0) != 0) continue;
        std::string rest = name.substr(16);
        if (rest.find('.') != std::string::npos && rest.front() != '"') continue;  // sub-table
        Provider p;
        p.id = unquote_key(rest);
        provider_models(profiles, p.id, p.model, p.models);
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
    return use(id, "", err);
}

bool use(const std::string& id, const std::string& selection, std::string& err) {
    Doc d;
    if (!load_doc(d, err)) return false;

    std::string model;
    ojson profiles = ojson::object();
    if (!load_profiles_doc(profiles, err)) return false;
    const ojson before = profiles;
    std::vector<ModelOption> models;
    if (!id.empty()) {
        Block b;
        if (!find_provider_block(d, id, b)) {
            err = "找不到 provider: " + id;
            return false;
        }
        provider_models(profiles, id, model, models);
        if (!normalize_models(model, models, err)) return false;
        if (!selection.empty()) {
            if (std::none_of(models.begin(), models.end(), [&](const ModelOption& m) {
                    return m.id == selection;
                })) {
                err = "这个中转没有配置模型: " + selection;
                return false;
            }
            model = selection;
            profiles["codex_models"][id] = model_settings(model, models);
        }
    } else if (!selection.empty()) {
        err = "请先选择一个已保存的中转";
        return false;
    }
    bool changed = set_top_level_key(d, "model_provider", id);
    if (!model.empty()) changed = set_top_level_key(d, "model", model) || changed;
    if (!configure_catalog(d, id, models, profiles, err)) return false;
    return (!changed && before == profiles) || save_provider_changes(d, before, profiles, err);
}

bool upsert(const Provider& input, std::string& err) {
    auto p = input;
    if (p.id.empty()) {
        err = "provider id 不能为空";
        return false;
    }
    if (!normalize_models(p.model, p.models, err)) return false;
    Doc d;
    if (!load_doc(d, err)) return false;
    ojson profiles;
    if (!load_profiles_doc(profiles, err)) return false;
    const ojson before = profiles;
    if (profiles.contains("codex_models") && !profiles["codex_models"].is_object()) {
        err = "profiles.json 的 codex_models 必须是 JSON 对象，已放弃修改";
        return false;
    }
    if (!p.model.empty()) {
        profiles["codex_models"][p.id] = model_settings(p.model, p.models);
    } else if (profiles.contains("codex_models")) {
        profiles["codex_models"].erase(p.id);
        if (profiles["codex_models"].empty()) profiles.erase("codex_models");
    }

    Block b;
    if (!find_provider_block(d, p.id, b)) {
        if (!d.lines.empty() && !trim(d.lines.back()).empty()) d.lines.push_back("");
        const bool bare = std::all_of(p.id.begin(), p.id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        });
        d.lines.push_back("[model_providers." + (bare ? p.id : quote(p.id)) + "]");
        b.begin = d.lines.size() - 1;
        b.end = d.lines.size();
        set_block_key(d, b, "name", p.name.empty() ? p.id : p.name);
        set_block_key(d, b, "base_url", p.base_url);
        set_block_key(d, b, "wire_api", "responses");
        if (!p.token.empty()) set_block_key(d, b, "experimental_bearer_token", p.token);
    } else {
        if (!p.name.empty()) set_block_key(d, b, "name", p.name);
        set_block_key(d, b, "base_url", p.base_url);
        if (!p.token.empty()) set_block_key(d, b, "experimental_bearer_token", p.token);
    }
    if (top_level_value(d, "model_provider") == p.id) {
        if (!p.model.empty()) set_top_level_key(d, "model", p.model);
        if (!configure_catalog(d, p.id, p.models, profiles, err)) return false;
    }
    return save_provider_changes(d, before, profiles, err);
}

bool remove(const std::string& id, std::string& err) {
    Doc d;
    if (!load_doc(d, err)) return false;
    Block b;
    if (!find_provider_block(d, id, b)) {
        err = "找不到 provider: " + id;
        return false;
    }
    ojson profiles;
    if (!load_profiles_doc(profiles, err)) return false;
    const ojson before = profiles;
    if (profiles.contains("codex_models") && profiles["codex_models"].is_object()) {
        profiles["codex_models"].erase(id);
        if (profiles["codex_models"].empty()) profiles.erase("codex_models");
    }
    // swallow the blank separator that preceded the block
    size_t begin = b.begin;
    if (begin > 0 && trim(d.lines[begin - 1]).empty()) --begin;
    d.lines.erase(d.lines.begin() + static_cast<long>(begin), d.lines.begin() + static_cast<long>(b.end));
    return save_provider_changes(d, before, profiles, err);
}

}  // namespace codex

}  // namespace as
