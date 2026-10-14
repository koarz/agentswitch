#pragma once

#include <string>
#include <vector>

namespace as {

struct ModelOption {
    std::string name;  // selection name; identical to id for a direct model
    std::string id;    // actual model ID sent to the relay
    bool operator==(const ModelOption&) const = default;
};

// Comma-separated IDs or name=ID mappings; accepts Chinese commas as well.
bool parse_model_list(const std::string& text, std::vector<ModelOption>& models, std::string& err);

// ---- Claude Code -----------------------------------------------------------
// Relays are kept in ~/.config/agentswitch/profiles.json and applied by
// writing env.ANTHROPIC_BASE_URL / env.ANTHROPIC_AUTH_TOKEN and an optional
// env.ANTHROPIC_MODEL into ~/.claude/settings.json. Model lists and mappings
// also populate the native modelPicker, family aliases and modelOverrides.
namespace claude {

struct Profile {
    std::string name;
    std::string base_url;
    std::string token;
    std::string model;  // empty = keep the current model
    std::vector<ModelOption> models;
};

struct Active {
    bool has_override = false;  // settings.json sets ANTHROPIC_BASE_URL
    std::string base_url;
    std::string token;
    std::string model;
};

struct State {
    std::vector<Profile> profiles;
    Active active;
    // index into profiles that matches `active`, -1 when none
    int active_index = -1;
};

State load();
bool save_profiles(const std::vector<Profile>& profiles, std::string& err);
// nullptr removes the override (back to Claude Code's own login).
bool apply(const Profile* profile, std::string& err);

}  // namespace claude

// ---- Codex -----------------------------------------------------------------
// Providers are the [model_providers.*] tables of ~/.codex/config.toml;
// Model bindings are kept in profiles.json; switching writes the top-level
// `model_provider` and, when configured, `model` / `model_catalog_json` keys.
// The generated catalog exposes names and relay IDs in Codex's native picker.
// The TOML is edited as
// text so unrelated comments and formatting survive.
namespace codex {

struct Provider {
    std::string id;
    std::string name;
    std::string base_url;
    std::string token;    // experimental_bearer_token
    std::string env_key;  // env var holding the key, when used instead of a token
    std::string model;    // empty = keep the current model
    std::vector<ModelOption> models;
};

struct State {
    bool file_exists = false;
    std::vector<Provider> providers;
    std::string active_id;  // empty = Codex default (OpenAI login / API key)
    std::string active_model;
};

State load();
bool use(const std::string& id, std::string& err);  // "" = default provider
// Selects one configured model and switches the relay in a single write.
bool use(const std::string& id, const std::string& model, std::string& err);
// Adds a provider or updates an existing one. Empty token on update = keep.
// A nonempty model is also applied when updating the active provider.
bool upsert(const Provider& p, std::string& err);
bool remove(const std::string& id, std::string& err);

std::string slugify(const std::string& name);

}  // namespace codex

}  // namespace as
