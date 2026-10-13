#pragma once

#include <string>
#include <vector>

namespace as {

// ---- Claude Code -----------------------------------------------------------
// Relays are kept in ~/.config/agentswitch/profiles.json and applied by
// writing env.ANTHROPIC_BASE_URL / env.ANTHROPIC_AUTH_TOKEN into
// ~/.claude/settings.json (every other setting is preserved).
namespace claude {

struct Profile {
    std::string name;
    std::string base_url;
    std::string token;
};

struct Active {
    bool has_override = false;  // settings.json sets ANTHROPIC_BASE_URL
    std::string base_url;
    std::string token;
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
// switching only touches the top level `model_provider` key. The file is
// edited as text so comments and formatting survive.
namespace codex {

struct Provider {
    std::string id;
    std::string name;
    std::string base_url;
    std::string token;    // experimental_bearer_token
    std::string env_key;  // env var holding the key, when used instead of a token
};

struct State {
    bool file_exists = false;
    std::vector<Provider> providers;
    std::string active_id;  // empty = Codex default (OpenAI login / API key)
};

State load();
bool use(const std::string& id, std::string& err);  // "" = default provider
// Adds a provider or updates an existing one. Empty token on update = keep.
bool upsert(const Provider& p, std::string& err);
bool remove(const std::string& id, std::string& err);

std::string slugify(const std::string& name);

}  // namespace codex

}  // namespace as
