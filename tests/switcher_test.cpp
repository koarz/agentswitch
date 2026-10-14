#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "switcher.hpp"
#include "util.hpp"

namespace {

using namespace as;
using json = nlohmann::json;

fs::path test_root;
constexpr auto kMode = fs::perms::owner_read | fs::perms::owner_write;

void expect(bool passed, const std::string& message) {
    if (!passed) throw std::runtime_error(message);
}

struct Fixture {
    fs::path root;
    Fixture() {
        char pattern[] = "/tmp/agentswitch-switcher-tests-XXXXXX";
        const auto dir = mkdtemp(pattern);
        if (!dir) throw std::runtime_error("mkdtemp failed");
        root = dir;
        test_root = root;
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
        test_root.clear();
    }
    fs::path profiles() const { return root / "config" / "agentswitch" / "profiles.json"; }
    fs::path settings() const { return root / "claude" / "settings.json"; }
    fs::path config() const { return root / "codex" / "config.toml"; }
};

void write(const fs::path& path, const std::string& content) {
    std::string err;
    expect(write_file_atomic(path, content, kMode, err), err);
}

std::string read(const fs::path& path) {
    auto content = read_file(path);
    expect(content.has_value(), "missing file: " + path.string());
    return *content;
}

json read_json(const fs::path& path) { return json::parse(read(path)); }

codex::Provider provider(const std::string& id, const std::string& model) {
    codex::Provider p;
    p.id = id;
    p.name = id;
    p.base_url = "https://" + id + ".example.com/v1";
    p.token = "test-token";
    p.model = model;
    return p;
}

void claude_legacy_and_shared_profiles() {
    Fixture f;
    write(f.profiles(), R"({"claude":[{"name":"legacy","base_url":"https://relay.example","token":"test"}],"codex_models":{"relay":"gpt-4o"},"other":{"keep":true}})");
    auto st = claude::load();
    expect(st.profiles.size() == 1 && st.profiles[0].model.empty(), "legacy Claude profile failed");
    auto profiles = st.profiles;
    profiles[0].model = "relay/claude-sonnet-3-5-20241022";
    const auto before = read(f.profiles());
    std::string err;
    expect(claude::save_profiles(profiles, err), err);
    const auto j = read_json(f.profiles());
    expect(j["claude"][0]["model"] == profiles[0].model, "Claude model not persisted");
    expect(j["codex_models"]["relay"] == "gpt-4o" && j["other"]["keep"] == true,
           "saving Claude profiles lost other settings");
    expect(read(f.profiles().string() + ".agentswitch.bak") == before, "profiles backup incorrect");
    expect(claude::load().profiles[0].model == profiles[0].model, "Claude model not reloaded");
}

void claude_apply_and_blank_model() {
    Fixture f;
    write(f.settings(), R"({"model":"sonnet","permissions":{"allow":["Read"]},"env":{"KEEP":"yes","ANTHROPIC_API_KEY":"previous"}})");
    const auto before = read(f.settings());
    claude::Profile p{"old model relay", "https://relay.example", "test-token", "claude-3-opus-20240229", {}};
    std::string err;
    expect(claude::apply(&p, err), err);
    auto j = read_json(f.settings());
    expect(j["env"]["ANTHROPIC_MODEL"] == p.model, "Claude custom model not applied");
    expect(j["model"] == "sonnet" && j["env"]["KEEP"] == "yes" && j.contains("permissions"),
           "Claude unrelated settings changed");
    expect(!j["env"].contains("ANTHROPIC_API_KEY"), "conflicting API key retained");
    expect(read(f.settings().string() + ".agentswitch.bak") == before, "Claude backup incorrect");
    expect(claude::load().active.model == p.model, "Claude active model not read");
    p.model.clear();
    expect(claude::apply(&p, err), err);
    expect(claude::load().active.model == "claude-3-opus-20240229", "blank Claude model changed selection");
    expect(claude::apply(nullptr, err), err);
    j = read_json(f.settings());
    expect(!j["env"].contains("ANTHROPIC_BASE_URL") && !j["env"].contains("ANTHROPIC_AUTH_TOKEN"),
           "Claude default did not clear relay");
    expect(j["env"]["ANTHROPIC_MODEL"] == "claude-3-opus-20240229", "Claude default changed model");
}

void claude_model_matching() {
    Fixture f;
    const std::vector<claude::Profile> profiles = {
        {"legacy", "https://relay.example/", "test", "", {}},
        {"older", "https://relay.example", "test", "claude-3-opus-20240229", {}},
        {"other", "https://relay.example", "test", "relay-custom-model", {}},
    };
    std::string err;
    expect(claude::save_profiles(profiles, err), err);
    expect(claude::apply(&profiles[2], err), err);
    expect(claude::load().active_index == 2, "same relay with different models matched wrong profile");
    expect(claude::apply(&profiles[1], err), err);
    expect(claude::load().active_index == 1, "legacy wildcard hid an exact model match");
    write(f.settings(), R"({"model":"legacy-model","env":{"ANTHROPIC_BASE_URL":"https://external.example"}})");
    const auto st = claude::load();
    expect(st.active_index == -1 && st.active.model == "legacy-model", "external model not available for import");
}

void codex_persist_and_switch() {
    Fixture f;
    write(f.config(), "# keep this comment\nmodel = 'original'  # preferred model\nmodel_provider = 'old' # relay\nmodel_reasoning_effort = \"low\"\n\n[profiles.work]\nmodel = \"nested-model\"\n");
    write(f.profiles(), R"({"claude":[{"name":"keep","model":"claude-legacy"}],"other":true})");
    std::string err;
    expect(codex::upsert(provider("older", "gpt-4o"), err), err);
    expect(codex::upsert(provider("alias", "gateway/gpt-4.1-legacy"), err), err);
    const auto st = codex::load();
    expect(st.providers.size() == 2 && st.providers[0].model == "gpt-4o" &&
           st.providers[1].model == "gateway/gpt-4.1-legacy", "Codex model bindings not reloaded");
    expect(st.active_model == "original", "adding inactive provider changed current model");
    expect(read_json(f.profiles())["claude"][0]["name"] == "keep", "Codex save lost Claude profiles");
    const auto before = read(f.config());
    expect(codex::use("older", err), err);
    expect(codex::load().active_id == "older" && codex::load().active_model == "gpt-4o",
           "Codex model and provider not switched together");
    const auto text = read(f.config());
    expect(text.find("model = \"gpt-4o\"  # preferred model") != std::string::npos &&
           text.find("model_provider = \"older\" # relay") != std::string::npos,
           "Codex switch lost inline comments");
    expect(text.find("model_reasoning_effort = \"low\"") != std::string::npos &&
           text.find("[profiles.work]\nmodel = \"nested-model\"") != std::string::npos,
           "Codex switch changed unrelated or nested settings");
    expect(read(f.config().string() + ".agentswitch.bak") == before, "Codex backup not from before whole switch");
    expect(codex::use("alias", err), err);
    expect(codex::load().active_model == "gateway/gpt-4.1-legacy", "arbitrary Codex alias rejected");
}

void codex_insert_and_quoted_models() {
    Fixture f;
    write(f.config(), "# model_reasoning_effort = \"high\"\n# model = \"commented\"\n\n[profiles.work]\nmodel = \"nested\"\n");
    const auto p = provider("relay", "gateway/old#model:\"quoted\"\\suffix");
    std::string err;
    expect(codex::upsert(p, err), err);
    expect(codex::use(p.id, err), err);
    expect(codex::load().active_model == p.model, "Codex model quoting round trip failed");
    auto text = read(f.config());
    expect(text.find("model = \"gateway/") < text.find("[profiles.work]"), "model inserted into a table");
    expect(text.find("model_provider = \"relay\"") < text.find("[profiles.work]"), "provider inserted into a table");
    write(f.config(), "  \"model\" = 'old#model' # keep me\n\"model_provider\" = 'relay'\n\n[model_providers.relay]\nname = \"Relay\"\n");
    expect(codex::use(p.id, err), err);
    text = read(f.config());
    expect(text.find("  \"model\" = \"gateway/") == 0 && text.find(" # keep me") != std::string::npos,
           "quoted top-level key or string '#' was mishandled");
    expect(codex::load().active_model == p.model, "quoted model key not read");
}

void codex_edit_active_and_clear_binding() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    auto p = provider("relay", "gpt-4o");
    std::string err;
    expect(codex::upsert(p, err), err);
    expect(codex::use(p.id, err), err);
    const auto before = read(f.config());
    p.model = "gpt-4.1";
    p.token.clear();
    expect(codex::upsert(p, err), err);
    auto st = codex::load();
    expect(st.active_model == p.model && st.providers[0].model == p.model, "active edit did not apply model");
    expect(st.providers[0].token == "test-token", "blank token lost existing token");
    expect(read(f.config().string() + ".agentswitch.bak") == before, "active edit backup incorrect");
    p.model.clear();
    expect(codex::upsert(p, err), err);
    expect(codex::load().providers[0].model.empty(), "model binding not cleared");
    expect(codex::load().active_model == "gpt-4.1", "clearing binding changed active model");
    expect(codex::use(p.id, err), err);
    expect(codex::load().active_model == "gpt-4.1", "provider without binding changed model");
    expect(codex::use("", err), err);
    expect(codex::load().active_id.empty() && codex::load().active_model == "gpt-4.1",
           "default Codex provider changed model");
}

void codex_remove_preserves_other_bindings() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    std::string err;
    expect(codex::upsert(provider("a", "gpt-4o"), err), err);
    expect(codex::upsert(provider("b", "gpt-4.1"), err), err);
    expect(codex::remove("a", err), err);
    const auto j = read_json(f.profiles());
    expect(!j["codex_models"].contains("a") && j["codex_models"]["b"]["model"] == "gpt-4.1",
           "deleting provider removed wrong model binding");
    expect(codex::load().providers.size() == 1 && codex::load().providers[0].id == "b",
           "deleting provider removed wrong TOML block");
}

void reject_invalid_models_and_profiles() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    write(f.settings(), "{}\n");
    const auto before = read(f.config());
    std::string err;
    expect(!codex::upsert(provider("relay", "gpt-4o\nmodel_provider=oops"), err), "newline model accepted");
    expect(read(f.config()) == before && !fs::exists(f.profiles()), "invalid model modified configuration");
    const claude::Profile invalid{"relay", "https://example.com", "test", "old model", {}};
    expect(!claude::save_profiles({invalid}, err) && !claude::apply(&invalid, err), "invalid Claude model accepted");
    expect(read(f.settings()) == "{}\n", "invalid Claude model changed settings");
    write(f.profiles(), "{invalid\n");
    expect(!claude::save_profiles({}, err), "invalid profiles JSON overwritten by Claude save");
    expect(!codex::upsert(provider("relay", "gpt-4o"), err), "invalid profiles JSON overwritten by Codex save");
    expect(read(f.profiles()) == "{invalid\n" && read(f.config()) == before,
           "invalid profiles rejection changed files");
    write(f.profiles(), R"({"codex_models":[]})");
    expect(!codex::upsert(provider("relay", "gpt-4o"), err), "invalid model binding schema overwritten");
}

void codex_rolls_back_failed_write() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    write(f.profiles(), R"({"claude":[],"other":"keep"})");
    const auto before = read_json(f.profiles());
    fs::create_directory(f.config().string() + ".agentswitch.tmp");
    std::string err;
    expect(!codex::upsert(provider("relay", "gpt-4o"), err), "blocked config write unexpectedly succeeded");
    expect(read_json(f.profiles()) == before, "failed config write left a new model binding");
    expect(read(f.config()) == "model = \"original\"\n", "failed config write changed original TOML");
}

void parse_direct_models_and_mappings() {
    std::vector<ModelOption> models;
    std::string err;
    expect(parse_model_list("gpt-4o, sonnet = claude-3-5-sonnet-20241022，opus=claude-3-opus-20240229;gpt-4o\nvendor/legacy", models, err), err);
    expect(models.size() == 4, "mixed direct/mapped models or deduplication failed");
    expect(models[0] == ModelOption{"gpt-4o", "gpt-4o"}, "direct model did not use relay ID");
    expect(models[1] == ModelOption{"sonnet", "claude-3-5-sonnet-20241022"}, "mapping target not parsed");
    expect(models[3].id == "vendor/legacy", "provider-prefixed ID rejected");
    const auto before = models;
    for (const auto& invalid : {"sonnet=", "=old-model", "alias=old model", "alias=one,alias=two"}) {
        expect(!parse_model_list(invalid, models, err), "invalid mapping accepted: " + std::string(invalid));
        expect(models == before, "failed parser replaced model list");
    }
    expect(parse_model_list(" , ， ; \n", models, err) && models.empty(), "empty model list rejected");
}

void claude_multiple_models_and_mappings() {
    Fixture f;
    claude::Profile p{"multi", "https://multi.example", "test", "", {
        {"claude-3-opus-20240229", "claude-3-opus-20240229"},
        {"sonnet", "claude-3-5-sonnet-20241022"},
        {"cheap", "gateway/claude-legacy"},
    }};
    std::string err;
    expect(claude::save_profiles({p}, err), err);
    p = claude::load().profiles.at(0);
    expect(p.models.size() == 3 && p.model == p.models[0].id, "Claude list or initial model lost");
    for (const auto& option : p.models) {
        p.model = option.id;
        expect(claude::save_profiles({p}, err), err);
        expect(claude::apply(&p, err), err);
        const auto st = claude::load();
        expect(st.active_index == 0 && st.active.model == option.id, "Claude mapped model not applied or matched");
        expect(st.profiles[0].model == option.id && st.profiles[0].models == p.models,
               "Claude model selection did not survive reload");
    }
    expect(read_json(f.settings())["env"]["ANTHROPIC_MODEL"] == "gateway/claude-legacy",
           "Claude wrote display name instead of relay ID");
    p.models.pop_back();
    expect(claude::save_profiles({p}, err), err);
    expect(claude::apply(&p, err), err);
    const auto st = claude::load();
    expect(st.profiles[0].models.size() == 2 && st.active.model == p.models[0].id,
           "removing selected Claude model did not fall back to first remaining model");
}

void codex_multiple_models_and_mappings() {
    Fixture f;
    write(f.config(), "# custom relay\nmodel = \"original\"\n");
    auto p = provider("multi", "");
    p.models = {{"gpt-4o", "gpt-4o"}, {"fast", "gateway/gpt-4.1-legacy"}, {"small", "gpt-4o-mini"}};
    std::string err;
    expect(codex::upsert(p, err), err);
    auto loaded = codex::load().providers.at(0);
    expect(loaded.models == p.models && loaded.model == "gpt-4o", "Codex list or initial model lost");
    const auto before = read(f.config());
    expect(codex::use(p.id, "gateway/gpt-4.1-legacy", err), err);
    auto st = codex::load();
    expect(st.active_id == p.id && st.active_model == "gateway/gpt-4.1-legacy",
           "Codex mapping did not apply relay ID");
    expect(st.providers[0].models == p.models && st.providers[0].model == st.active_model,
           "Codex selected model or alternatives lost on reload");
    expect(read(f.config().string() + ".agentswitch.bak") == before, "mapped switch backup incorrect");
    expect(codex::use("", err), err);
    expect(codex::use(p.id, err), err);
    expect(codex::load().active_model == "gateway/gpt-4.1-legacy", "Codex did not remember selected model");
    loaded = codex::load().providers.at(0);
    loaded.models.erase(loaded.models.begin() + 1);
    expect(codex::upsert(loaded, err), err);
    st = codex::load();
    expect(st.providers[0].models.size() == 2 && st.active_model == "gpt-4o",
           "removed Codex selection was restored instead of falling back");
    const auto config_before = read(f.config());
    const auto profiles_before = read(f.profiles());
    expect(!codex::use(p.id, "unconfigured-model", err), "unconfigured model selection accepted");
    expect(read(f.config()) == config_before && read(f.profiles()) == profiles_before,
           "invalid Codex selection modified files");
}

void legacy_model_bindings_upgrade() {
    Fixture f;
    write(f.config(), "[model_providers.relay]\nname = \"Relay\"\nbase_url = \"https://relay.example/v1\"\n");
    write(f.profiles(), R"({"claude":[{"name":"old","base_url":"https://old.example","token":"test","model":"claude-legacy"}],"codex_models":{"relay":"gpt-4o"},"keep":true})");
    auto p = codex::load().providers.at(0);
    expect(p.model == "gpt-4o" && p.models == std::vector<ModelOption>{{"gpt-4o", "gpt-4o"}},
           "legacy Codex string did not become a model option");
    const auto c = claude::load().profiles.at(0);
    expect(c.models == std::vector<ModelOption>{{"claude-legacy", "claude-legacy"}},
           "legacy Claude model did not become a model option");
    std::string err;
    expect(codex::use(p.id, p.model, err), err);
    const auto j = read_json(f.profiles());
    expect(j["codex_models"][p.id]["models"].size() == 1 && j["keep"] == true && j["claude"][0]["model"] == c.model,
           "legacy binding migration lost other profiles");
}

void codex_selection_rolls_back_failed_write() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    auto p = provider("multi", "");
    p.models = {{"one", "gpt-4o"}, {"two", "gpt-4.1"}};
    std::string err;
    expect(codex::upsert(p, err), err);
    const auto before = read_json(f.profiles());
    fs::create_directory(f.config().string() + ".agentswitch.tmp");
    expect(!codex::use(p.id, "gpt-4.1", err), "blocked model selection unexpectedly succeeded");
    expect(read_json(f.profiles()) == before && codex::load().active_model == "original",
           "failed selection changed saved or active model");
}

void claude_native_aliases_and_restore() {
    Fixture f;
    const json original = {
        {"env", {{"KEEP", "yes"}, {"ANTHROPIC_DEFAULT_SONNET_MODEL", "original-sonnet"}}},
        {"modelOverrides", {{"claude-sonnet-5-5", "original-version"}, {"claude-opus-4-6", "keep-version"}}},
        {"modelPicker", {{"options", json::array({{{"model", "original-picker"}}})}}},
    };
    write(f.settings(), original.dump());
    claude::Profile p{"mapped", "https://relay.example", "test", "", {
        {"sonnet", "claude-3-5-sonnet-20241022"},
        {"opus", "claude-3-opus-20240229"},
        {"claude-sonnet-5-5", "relay/version-id"},
        {"custom", "relay/direct-model"},
    }};
    std::string err;
    expect(claude::apply(&p, err), err);
    auto j = read_json(f.settings());
    expect(j["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] == p.models[0].id &&
           j["env"]["ANTHROPIC_DEFAULT_OPUS_MODEL"] == p.models[1].id,
           "Claude native family aliases not mapped");
    expect(j["modelOverrides"]["claude-sonnet-5-5"] == "relay/version-id" &&
           j["modelOverrides"]["claude-opus-4-6"] == "keep-version",
           "Claude native version mapping lost unrelated overrides");
    expect(j["modelPicker"]["replaceBuiltInOptions"] == true && j["modelPicker"]["options"].size() == 4 &&
           j["modelPicker"]["options"][0]["model"] == "sonnet" &&
           j["modelPicker"]["options"][3]["model"] == "relay/direct-model",
           "Claude native model picker not populated");
    auto next = p;
    next.name = "next";
    next.model.clear();
    next.models = {{"haiku", "claude-3-haiku-20240307"}};
    expect(claude::apply(&next, err), err);
    j = read_json(f.settings());
    expect(j["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] == "original-sonnet" &&
           !j["env"].contains("ANTHROPIC_DEFAULT_OPUS_MODEL") &&
           j["env"]["ANTHROPIC_DEFAULT_HAIKU_MODEL"] == "claude-3-haiku-20240307",
           "switching Claude relay retained previous aliases");
    expect(j["modelOverrides"] == original["modelOverrides"], "switching relay did not restore version overrides");
    expect(claude::apply(nullptr, err), err);
    j = read_json(f.settings());
    expect(j["modelPicker"] == original["modelPicker"] && j["env"]["KEEP"] == "yes" &&
           !j["env"].contains("ANTHROPIC_DEFAULT_HAIKU_MODEL"), "Claude defaults not restored");
    expect(!read_json(f.profiles()).contains("claude_cli"), "Claude native ownership metadata not cleared");
}

void claude_preserves_external_alias_edits() {
    Fixture f;
    claude::Profile p{"mapped", "https://relay.example", "test", "", {{"sonnet", "old-sonnet"}}};
    std::string err;
    expect(claude::apply(&p, err), err);
    auto j = read_json(f.settings());
    j["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] = "external-change";
    j["modelPicker"] = {{"options", json::array({{{"model", "external-picker"}}})}};
    write(f.settings(), j.dump());
    expect(claude::apply(nullptr, err), err);
    const auto after = read_json(f.settings());
    expect(after["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] == "external-change" &&
           after["modelPicker"] == j["modelPicker"], "restoration overwrote externally edited Claude settings");
}

void claude_native_failed_write_rollback() {
    Fixture f;
    write(f.settings(), "{}\n");
    write(f.profiles(), R"({"claude":[],"keep":true})");
    const auto before = read_json(f.profiles());
    fs::create_directory(f.settings().string() + ".agentswitch.tmp");
    const claude::Profile p{"mapped", "https://relay.example", "test", "", {{"sonnet", "old-sonnet"}}};
    std::string err;
    expect(!claude::apply(&p, err), "blocked Claude native write succeeded");
    expect(read_json(f.profiles()) == before && read(f.settings()) == "{}\n",
           "failed Claude native write left ownership metadata or modified settings");
}

void codex_native_catalog_and_restore() {
    Fixture f;
    const auto original_catalog = f.root / "original-models.json";
    write(original_catalog, R"({"models":[{"slug":"gpt-4o","context_window":128000,"base_instructions":"preserve instructions"}]})");
    write(f.config(), "model_catalog_json = \"" + original_catalog.string() + "\" # original catalog\n");
    auto p = provider("multi", "");
    p.models = {{"legacy", "gpt-4o"}, {"fast", "relay/old-model"}};
    std::string err;
    expect(codex::upsert(p, err), err);
    expect(codex::use(p.id, p.models[1].id, err), err);
    auto shared = read_json(f.profiles());
    const auto generated = fs::path(shared["codex_cli"]["path"].get<std::string>());
    const auto j = read_json(generated);
    expect(j["models"].size() == 2 && j["models"][0]["slug"] == "gpt-4o" &&
           j["models"][0]["display_name"] == "legacy" &&
           j["models"][1]["slug"] == "relay/old-model" && j["models"][1]["display_name"] == "fast",
           "Codex native catalog did not map display names to relay IDs");
    expect(j["models"][0]["context_window"] == 128000 &&
           j["models"][0]["base_instructions"] == "preserve instructions", "existing native model metadata lost");
    expect(j["models"][1]["supported_reasoning_levels"].empty() &&
           j["models"][1]["use_responses_lite"] == false, "unknown model inherited new-model protocol");
    expect(codex::use(p.id, p.models[0].id, err), err);
    expect(read_json(f.profiles())["codex_cli"]["path"] == generated.string(), "unchanged model list generated a new catalog");
    const auto catalog_before = read(generated);
    p = codex::load().providers[0];
    p.models.push_back({"mini", "gpt-4o-mini"});
    expect(codex::upsert(p, err), err);
    expect(read(generated) == catalog_before && read_json(f.profiles())["codex_cli"]["path"] != generated.string(),
           "catalog update changed the backup configuration's catalog");
    expect(codex::use("", err), err);
    shared = read_json(f.profiles());
    expect(!shared.contains("codex_cli") && read(f.config()).find("model_catalog_json = \"" + original_catalog.string() + "\" # original catalog") != std::string::npos,
           "default provider did not restore the original Codex catalog");
}

void codex_preserves_external_catalog_edits() {
    Fixture f;
    write(f.config(), "model = \"original\"\n");
    auto p = provider("relay", "old-model");
    std::string err;
    expect(codex::upsert(p, err), err);
    expect(codex::use(p.id, err), err);
    const auto owned = read_json(f.profiles())["codex_cli"]["path"].get<std::string>();
    auto text = read(f.config());
    const auto at = text.find(owned);
    expect(at != std::string::npos, "generated catalog not applied");
    text.replace(at, owned.size(), "/external/model-catalog.json");
    write(f.config(), text);
    expect(codex::use("", err), err);
    expect(read(f.config()).find("model_catalog_json = \"/external/model-catalog.json\"") != std::string::npos,
           "catalog restoration overwrote external changes");
}

}  // namespace

// src/switcher.cpp uses these names only in the test target.
namespace as {
fs::path test_claude_dir() { return test_root / "claude"; }
fs::path test_codex_dir() { return test_root / "codex"; }
fs::path test_config_dir() { return test_root / "config" / "agentswitch"; }
}  // namespace as

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"Claude legacy and shared profiles", claude_legacy_and_shared_profiles},
        {"Claude apply and blank model", claude_apply_and_blank_model},
        {"Claude model matching", claude_model_matching},
        {"Codex persist and switch", codex_persist_and_switch},
        {"Codex insert and quoted models", codex_insert_and_quoted_models},
        {"Codex active edit and clear binding", codex_edit_active_and_clear_binding},
        {"Codex remove binding", codex_remove_preserves_other_bindings},
        {"Invalid models and profiles", reject_invalid_models_and_profiles},
        {"Codex failed write rollback", codex_rolls_back_failed_write},
        {"Direct models and mappings parser", parse_direct_models_and_mappings},
        {"Claude multiple models and mappings", claude_multiple_models_and_mappings},
        {"Codex multiple models and mappings", codex_multiple_models_and_mappings},
        {"Legacy model binding upgrade", legacy_model_bindings_upgrade},
        {"Codex selection rollback", codex_selection_rolls_back_failed_write},
        {"Claude native aliases and restoration", claude_native_aliases_and_restore},
        {"Claude external alias edits", claude_preserves_external_alias_edits},
        {"Claude native failed write rollback", claude_native_failed_write_rollback},
        {"Codex native catalog and restoration", codex_native_catalog_and_restore},
        {"Codex external catalog edits", codex_preserves_external_catalog_edits},
    };
    int failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::printf("PASS %s\n", name);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL %s: %s\n", name, e.what());
            ++failed;
        }
    }
    std::printf("%zu tests, %d failed\n", tests.size(), failed);
    return failed ? 1 : 0;
}
