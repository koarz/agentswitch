#include "ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "switcher.hpp"
#include "util.hpp"
#include "worker.hpp"

namespace as {

using namespace ftxui;

namespace {

// ---- palette ----------------------------------------------------------------

const Color kClaude = Color::RGB(217, 119, 87);
const Color kCodex = Color::RGB(88, 166, 255);
const Color kTotalColor = Color::RGB(232, 196, 104);
const Color kAccent = Color::RGB(167, 139, 250);
const Color kOk = Color::RGB(88, 200, 124);
const Color kWarn = Color::RGB(240, 180, 60);
const Color kErr = Color::RGB(240, 96, 96);
const Color kDim = Color::RGB(122, 128, 145);
const Color kFg = Color::RGB(216, 220, 230);
const Color kSelBg = Color::RGB(36, 41, 58);
const Color kKeyBg = Color::RGB(58, 64, 84);

Color tool_color(int idx) { return idx == 0 ? kClaude : idx == 1 ? kCodex : kTotalColor; }

// ---- small helpers ------------------------------------------------------------

Element rtext(const std::string& s, int w) {
    return hbox({filler(), text(s)}) | size(WIDTH, EQUAL, w);
}

Element ltext(const std::string& s, int w) { return text(s) | size(WIDTH, EQUAL, w); }

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

Element gradient_text(const std::string& s) {
    // ASCII only: interpolate between two RGB stops per character.
    const int r0 = 217, g0 = 119, b0 = 87;    // claude
    const int r1 = 88, g1 = 166, b1 = 255;    // codex
    Elements out;
    const size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        double t = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
        auto lerp = [&](int a, int b) { return static_cast<uint8_t>(a + (b - a) * t); };
        out.push_back(text(std::string(1, s[i])) | bold |
                      color(Color::RGB(lerp(r0, r1), lerp(g0, g1), lerp(b0, b1))));
    }
    return hbox(std::move(out));
}

Element key_hint(const std::string& key, const std::string& desc) {
    return hbox({text(" " + key + " ") | bold | color(kFg) | bgcolor(kKeyBg),
                 text(" " + desc + "  ") | color(kDim)});
}

Element badge(const std::string& s, Color bg) {
    return text(" " + s + " ") | bold | color(Color::Black) | bgcolor(bg);
}

struct StatusView {
    bool busy = false;
    std::string status;
    std::string price_error;
    size_t price_models = 0;
    int64_t prices_at = 0;
    int64_t scanned_at = 0;
};

// ---- header / footer ----------------------------------------------------------

Element render_header(int page, const StatusView& st, int width) {
    auto tab = [&](const std::string& key, const std::string& label, bool on) {
        Element e = text(" " + key + " " + label + " ");
        return on ? e | bold | color(Color::Black) | bgcolor(kAccent) : e | color(kDim);
    };

    const bool roomy = width >= 112;
    Elements right;
    if (st.busy) {
        right.push_back(text("◌ 同步中…") | color(kWarn));
    } else if (roomy || !st.status.empty()) {
        right.push_back(text("● " + (st.status.empty() ? std::string("就绪") : st.status)) |
                        color(kOk));
    }
    if (width >= 86) {
        right.push_back(text("  "));
        if (!st.price_error.empty()) {
            right.push_back(text("⚠ 价格获取失败") | color(kWarn));
        } else if (st.price_models > 0) {
            right.push_back(text("$ models.dev " + std::to_string(st.price_models) + " 个模型" +
                                 (roomy ? " · " + human_ago(now_unix() - st.prices_at) : "")) |
                            color(kDim));
        } else {
            right.push_back(text("$ 价格未加载") | color(kWarn));
        }
    }

    return vbox({
        hbox({text(" ◆ ") | color(kAccent) | bold, gradient_text("AgentSwitch"),
              roomy ? text("  Codex · Claude 中转切换与用量") | color(kDim) : text(""), filler(),
              tab("1", "切换", page == 0), text(" "), tab("2", "用量", page == 1), filler(),
              hbox(std::move(right)), text(" ")}),
        separatorLight() | color(kDim),
    });
}

// mode: 0 switch page, 1 usage tabs, 2 per-day view
Element render_footer(int mode, const std::string& toast, bool toast_err) {
    Elements l;
    if (mode == 0) {
        l = {key_hint("↑↓", "选择"), key_hint("Enter", "切换"), key_hint("Tab", "换栏"),
             key_hint("a", "添加"),  key_hint("e", "编辑"),    key_hint("d", "删除"),
             key_hint("m", "选模型"), key_hint("i", "导入当前")};
    } else if (mode == 1) {
        l = {key_hint("Tab", "全部/Claude/Codex"), key_hint("←→", "周期"), key_hint("↑↓", "选模型"),
             key_hint("Enter", "每日用量"), key_hint("r", "刷新"), key_hint("1/2", "页面")};
    } else {
        l = {key_hint("↑↓", "滚动"), key_hint("PgUp/PgDn", "翻页"), key_hint("d/w/m", "日·周·月"),
             key_hint("Esc", "返回")};
    }
    l.push_back(key_hint("q", "退出"));
    Elements rows;
    if (!toast.empty()) {
        rows.push_back(hbox({text(" "), text(toast) | bold | color(toast_err ? kErr : kOk)}));
    } else {
        rows.push_back(text(""));
    }
    rows.push_back(hbox(std::move(l)));
    return vbox(std::move(rows));
}

// ---- switch page ---------------------------------------------------------------

struct RowView {
    std::string title;
    std::string sub;
    bool active = false;
    bool external = false;
    std::string model;
};

std::string model_label(const std::string& id, const std::vector<ModelOption>& models) {
    std::string label = id;
    for (const auto& m : models) {
        if (m.id == id && m.name != m.id) {
            label = m.name + " → " + m.id;
            break;
        }
    }
    if (models.size() > 1) label += "  ·  " + std::to_string(models.size()) + " 个模型";
    return label;
}

std::string format_models(const std::vector<ModelOption>& models) {
    std::string out;
    for (const auto& m : models) {
        if (!out.empty()) out += ", ";
        out += m.name == m.id ? m.id : m.name + "=" + m.id;
    }
    return out;
}

std::vector<RowView> claude_rows(const claude::State& st) {
    std::vector<RowView> rows;
    rows.push_back({"官方登录", "不覆盖 ANTHROPIC_BASE_URL，使用 Claude 自己的账号",
                    !st.active.has_override, false,
                    !st.active.has_override ? st.active.model : ""});
    for (size_t i = 0; i < st.profiles.size(); ++i) {
        const auto& p = st.profiles[i];
        const bool on = static_cast<int>(i) == st.active_index;
        rows.push_back({p.name, url_host(p.base_url) + "  ·  " + mask_secret(p.token), on, false,
                        model_label(on ? st.active.model : p.model, p.models)});
    }
    if (st.active.has_override && st.active_index < 0) {
        rows.push_back({"外部配置 (未保存)",
                        url_host(st.active.base_url) + "  ·  " + mask_secret(st.active.token) +
                            "  ·  按 i 保存",
                        true, true, st.active.model});
    }
    return rows;
}

std::vector<RowView> codex_rows(const codex::State& st) {
    std::vector<RowView> rows;
    rows.push_back({"OpenAI 默认", "官方登录 / OPENAI_API_KEY，不指定 model_provider",
                    st.active_id.empty(), false, st.active_id.empty() ? st.active_model : ""});
    bool found = false;
    for (const auto& p : st.providers) {
        bool on = p.id == st.active_id;
        found = found || on;
        std::string secret = !p.token.empty() ? mask_secret(p.token)
                             : !p.env_key.empty() ? "env:" + p.env_key
                                                  : "—";
        std::string title = p.name == p.id ? p.id : p.name + "  (" + p.id + ")";
        std::string model = model_label(on ? st.active_model : p.model, p.models);
        rows.push_back({title, url_host(p.base_url) + "  ·  " + secret, on, false, model});
    }
    if (!st.active_id.empty() && !found) {
        rows.push_back({"外部 provider: " + st.active_id, "config.toml 中没有对应的表", true, true,
                        st.active_model});
    }
    return rows;
}

Element render_row(const RowView& r, bool selected, bool panel_focused, Color c) {
    Color bar = panel_focused ? c : kDim;
    Element mark = r.active ? text("●") | color(kOk) : text("○") | color(kDim);
    Elements l1 = {text(selected ? "▌" : " ") | color(bar), text(" "), mark, text(" "),
                   text(r.title) | bold | color(r.external ? kWarn : kFg), filler()};
    if (r.active) {
        l1.push_back(badge("使用中", kOk));
        l1.push_back(text(" "));
    }
    Element l2 = hbox({text(selected ? "▌" : " ") | color(bar), text("    "),
                       text(r.sub) | color(kDim)});
    Elements lines = {hbox(std::move(l1)), l2};
    if (!r.model.empty())
        lines.push_back(hbox({text(selected ? "▌" : " ") | color(bar), text("    模型: ") | color(kDim),
                              text(r.model) | color(c)}));
    lines.push_back(text(""));
    Element e = vbox(std::move(lines));
    if (selected) e = e | bgcolor(kSelBg) | focus;
    return e;
}

Element render_panel(const std::string& icon, const std::string& title, const std::string& path,
                     Color c, const std::vector<RowView>& rows, int sel, bool focused) {
    Elements body;
    for (size_t i = 0; i < rows.size(); ++i) {
        body.push_back(render_row(rows[i], static_cast<int>(i) == sel, focused, c));
    }
    body.push_back(filler());
    Element panel = vbox({
        hbox({text(" " + icon + " ") | color(c) | bold, text(title) | bold | color(c),
              filler(), text(path + " ") | color(kDim)}),
        separatorLight() | color(kDim),
        vbox(std::move(body)) | vscroll_indicator | yframe | flex,
    });
    return panel | borderStyled(focused ? HEAVY : ROUNDED, focused ? c : kDim) | flex;
}

struct SwitchView {
    claude::State claude;
    codex::State codex;
    int col = 0;
    int sel[2] = {0, 0};
};

Element render_switch(const SwitchView& v) {
    auto cr = claude_rows(v.claude);
    auto xr = codex_rows(v.codex);
    std::string cpath = "~/.claude/settings.json";
    std::string xpath = "~/.codex/config.toml";
    Element left = render_panel("✻", "Claude Code", cpath, kClaude, cr, v.sel[0], v.col == 0);
    Element right = v.codex.file_exists
                        ? render_panel("◈", "Codex", xpath, kCodex, xr, v.sel[1], v.col == 1)
                        : render_panel("◈", "Codex", xpath, kCodex,
                                       {{"未找到 config.toml", "先运行一次 codex 以生成配置", false, true, ""}},
                                       0, v.col == 1);
    return hbox({left, right}) | flex;
}

// ---- usage page ----------------------------------------------------------------
//
// One block with three tabs (全部 / Claude / Codex). Inside a tab the period
// cards (今日 / 本周 / 本月 / 总计) drive a model table. Enter or a click on a
// row opens the per-day view of that row, which reaches back to the first day
// it was used.

// token components share one colour set everywhere on the page
const Color kInput = Color::RGB(232, 196, 104);
const Color kOutput = Color::RGB(240, 110, 170);
const Color kCacheRead = Color::RGB(64, 200, 170);
const Color kCacheWrite = Color::RGB(167, 139, 250);

constexpr int kScopeCount = 3;  // 0 全部, 1 Claude, 2 Codex

struct UsageSel {
    int scope = 0;
    int period = kMonth;
    std::string key;  // selected table row: "" = aggregate, "<tool>:<model>"

    // per-day view
    bool detail = false;
    int d_tool = -1;  // -1 = both tools
    std::string d_model;
    int gran = 0;  // 0 day, 1 week, 2 month
    int d_cur = 0;
};

// Screen boxes recorded while rendering, used to resolve mouse clicks.
struct UsageNav {
    Box tab[kScopeCount];
    Box period[kPeriodCount];
    Box table_view;
    std::vector<Box> rows;
    Box back;
    Box gran[3];
    Box detail_view;
    std::vector<Box> d_rows;
};

Element hit(Element e, Box* box) { return box ? std::move(e) | reflect(*box) : std::move(e); }

Color scope_color(int scope) { return scope == 0 ? kTotalColor : scope == 1 ? kClaude : kCodex; }

const char* scope_name(int scope) {
    return scope == 0 ? "全部" : scope == 1 ? "Claude Code" : "Codex";
}

const char* scope_icon(int scope) { return scope == 0 ? "Σ" : scope == 1 ? "✻" : "◈"; }

// ---- table rows ----

struct UsageRow {
    int tool = -1;  // -1 aggregate of the scope (both tools for 全部)
    std::string model;
    Tokens tok;
    double usd = 0;
    bool priced = true;
    bool aggregate = false;
};

std::string row_key(int tool, const std::string& model) {
    return model.empty() ? std::string() : std::to_string(tool) + ":" + model;
}

std::vector<UsageRow> usage_rows(const Report& rep, int scope, int period) {
    std::vector<UsageRow> rows;
    const Sum& all = rep.sums[period][scope == 0 ? 2 : scope - 1];
    UsageRow agg;
    agg.tool = scope == 0 ? -1 : scope - 1;
    agg.tok = all.tok;
    agg.usd = all.usd;
    agg.priced = !(all.usd == 0 && all.unpriced_tokens > 0);
    agg.aggregate = true;
    rows.push_back(std::move(agg));
    for (const auto& m : rep.models[period]) {
        if (scope != 0 && static_cast<int>(m.tool) != scope - 1) continue;
        UsageRow r;
        r.tool = static_cast<int>(m.tool);
        r.model = m.model;
        r.tok = m.tok;
        r.usd = m.usd;
        r.priced = !m.priced_as.empty();
        rows.push_back(std::move(r));
    }
    return rows;
}

int row_index(const std::vector<UsageRow>& rows, const std::string& key) {
    if (key.empty()) return 0;
    for (size_t i = 1; i < rows.size(); ++i)
        if (row_key(rows[i].tool, rows[i].model) == key) return static_cast<int>(i);
    return 0;
}

// ---- small drawing helpers ----

const Color kComp[4] = {kInput, kOutput, kCacheRead, kCacheWrite};
const char* const kCompName[4] = {"输入", "输出", "缓存读", "缓存写"};

int64_t comp_value(const Tokens& t, int i) {
    return i == 0 ? t.input : i == 1 ? t.output : i == 2 ? t.cache_read : t.cache_write;
}

// A bar of `cells` cells split into the four token components. Each present
// component gets at least one cell, the rest is shared proportionally.
Element comp_bar(const Tokens& t, int cells, int full_cells = -1) {
    const int64_t total = t.total();
    if (total <= 0 || cells <= 0) return text(repeat("·", 1)) | color(kDim);
    int n = full_cells < 0 ? cells : full_cells;  // cells used by the whole bar
    n = std::clamp(n, 1, cells);

    int alloc[4] = {0, 0, 0, 0};
    int present = 0;
    for (int i = 0; i < 4; ++i) present += comp_value(t, i) > 0;
    int left = n;
    // largest remainder over the proportional share
    double frac[4];
    for (int i = 0; i < 4; ++i) {
        const double share = static_cast<double>(comp_value(t, i)) / static_cast<double>(total) * n;
        alloc[i] = static_cast<int>(share);
        frac[i] = share - alloc[i];
        left -= alloc[i];
    }
    while (left > 0) {
        int best = -1;
        for (int i = 0; i < 4; ++i)
            if (comp_value(t, i) > 0 && (best < 0 || frac[i] > frac[best])) best = i;
        if (best < 0) break;
        ++alloc[best];
        frac[best] = -1;
        --left;
    }
    if (n >= present) {
        // make sure tiny components stay visible
        for (int i = 0; i < 4; ++i) {
            if (comp_value(t, i) > 0 && alloc[i] == 0) {
                int big = 0;
                for (int j = 1; j < 4; ++j)
                    if (alloc[j] > alloc[big]) big = j;
                if (alloc[big] > 1) {
                    --alloc[big];
                    alloc[i] = 1;
                }
            }
        }
    }
    Elements parts;
    for (int i = 0; i < 4; ++i)
        if (alloc[i] > 0) parts.push_back(text(repeat("█", alloc[i])) | color(kComp[i]));
    return hbox(std::move(parts));
}

Element comp_legend(const Tokens& t, int width) {
    const int64_t total = std::max<int64_t>(1, t.total());
    const bool pct_on = width >= 98;
    Elements l;
    for (int i = 0; i < 4; ++i) {
        const int64_t v = comp_value(t, i);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%.1f%%", static_cast<double>(v) * 100.0 / total);
        l.push_back(text("■ ") | color(kComp[i]));
        l.push_back(text(kCompName[i]) | color(kDim));
        l.push_back(text(" " + fmt_tokens(v)) | bold | color(kFg));
        if (pct_on) l.push_back(text(std::string(" ") + pct) | color(kDim));
        l.push_back(text(pct_on ? "    " : "  "));
    }
    return hbox(std::move(l));
}

// ---- main view ----

Element usage_tabs(const UsageSel& sel, UsageNav* nav) {
    Elements l = {text(" ")};
    for (int sc = 0; sc < kScopeCount; ++sc) {
        const bool on = sc == sel.scope;
        Element e = text(std::string(" ") + scope_icon(sc) + " " + scope_name(sc) + " ");
        e = on ? e | bold | color(Color::Black) | bgcolor(scope_color(sc))
               : e | color(scope_color(sc));
        l.push_back(hit(std::move(e), nav ? &nav->tab[sc] : nullptr));
        l.push_back(text(" "));
    }
    l.push_back(filler());
    return hbox(std::move(l));
}

Element period_cards(const Report& rep, const UsageSel& sel, UsageNav* nav) {
    const Color c = scope_color(sel.scope);
    const int idx = sel.scope == 0 ? 2 : sel.scope - 1;
    Elements cards;
    for (int p = 0; p < kPeriodCount; ++p) {
        const Sum& s = rep.sums[p][idx];
        const bool on = p == sel.period;
        Element usd = text(fmt_usd(s.usd)) | bold | color(on ? c : kFg);
        if (s.usd == 0 && s.tok.total() > 0 && s.unpriced_tokens > 0) usd = text("—") | color(kWarn);
        Element card = vbox({hbox({text(" "), text(period_label(p)) | (on ? bold : nothing) |
                                                  color(on ? kFg : kDim)}),
                             hbox({text(" "), std::move(usd)}),
                             hbox({text(" "), text(fmt_tokens(s.tok.total()) + " tokens") |
                                                  color(kDim)})}) |
                       borderStyled(on ? HEAVY : ROUNDED, on ? c : kDim) | xflex;
        cards.push_back(hit(std::move(card), nav ? &nav->period[p] : nullptr));
    }
    return hbox(std::move(cards));
}

Element usage_main(const Report& rep, const UsageSel& sel, UsageNav* nav, int width) {
    const Color c = scope_color(sel.scope);
    const auto rows = usage_rows(rep, sel.scope, sel.period);
    const int cur = row_index(rows, sel.key);
    const Sum& all = rep.sums[sel.period][sel.scope == 0 ? 2 : sel.scope - 1];

    // numeric columns by priority while they fit next to a minimal model column
    const int dot_w = sel.scope == 0 ? 2 : 0;
    int room = width - 6 - 18 - dot_w;
    struct Col {
        int kind;  // 0 usd, 1 total, 2 bar, 3.. components (3 + i)
        int w;
    };
    auto take = [&](int w) {
        if (room >= w) {
            room -= w;
            return true;
        }
        return false;
    };
    const bool k_usd = take(11), k_tot = take(9), k_bar = take(18), k_out = take(8), k_in = take(8),
               k_cr = take(8), k_cw = take(8);
    std::vector<Col> cols;
    if (k_bar) cols.push_back({2, 18});
    if (k_in) cols.push_back({3, 8});
    if (k_out) cols.push_back({4, 8});
    if (k_cr) cols.push_back({5, 8});
    if (k_cw) cols.push_back({6, 8});
    if (k_tot) cols.push_back({1, 9});
    if (k_usd) cols.push_back({0, 11});
    const int model_w = std::clamp(18 + room, 18, 44);

    // model rows are scaled against the largest row so the bars compare
    int64_t max_tok = 1;
    for (size_t i = 1; i < rows.size(); ++i) max_tok = std::max(max_tok, rows[i].tok.total());

    Elements head = {text(" "), ltext("模型", model_w + dot_w + 2) | color(kDim), filler()};
    for (const auto& col : cols) {
        const char* t = col.kind == 0 ? "费用" : col.kind == 1 ? "合计" : col.kind == 2 ? "构成"
                                                                                       : kCompName[col.kind - 3];
        head.push_back(rtext(t, col.w) | color(col.kind >= 3 ? kComp[col.kind - 3] : kDim));
    }
    head.push_back(text(" "));

    Elements body;
    if (nav) nav->rows.assign(rows.size(), Box{});
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const bool on = static_cast<int>(i) == cur;
        const Color tc = r.tool < 0 ? c : tool_color(r.tool);
        std::string label = r.aggregate ? (sel.scope == 0 ? "全部模型" : "全部模型") : r.model;
        const size_t keep = static_cast<size_t>(model_w) - (r.priced ? 1 : 3);
        if (label.size() > keep) label = label.substr(0, keep - 1) + "…";

        Elements line = {text(" "), text(on ? "▌" : " ") | color(c), text(" ")};
        if (dot_w) line.push_back(text(r.aggregate ? "Σ " : "● ") | color(tc));
        line.push_back(text(label) | (on || r.aggregate ? bold : nothing) |
                       color(r.priced ? (r.aggregate ? c : kFg) : kWarn));
        if (!r.priced) line.push_back(text(" ⚠") | color(kWarn));
        line.push_back(filler());
        for (const auto& col : cols) {
            if (col.kind == 0) {
                line.push_back(rtext(r.priced ? fmt_usd(r.usd) : "—", col.w) | bold |
                               color(r.priced ? tc : kWarn));
            } else if (col.kind == 1) {
                line.push_back(rtext(fmt_tokens(r.tok.total()), col.w) | color(kFg));
            } else if (col.kind == 2) {
                const int full = r.aggregate
                                     ? col.w - 2
                                     : std::max<int>(1, static_cast<int>(std::lround(
                                           static_cast<double>(r.tok.total()) / max_tok * (col.w - 2))));
                line.push_back(hbox({filler(), comp_bar(r.tok, col.w - 2, full), text("  ")}) |
                               size(WIDTH, EQUAL, col.w));
            } else {
                line.push_back(rtext(fmt_tokens(comp_value(r.tok, col.kind - 3)), col.w) |
                               color(kDim));
            }
        }
        line.push_back(text(" "));
        Element e = hbox(std::move(line));
        if (on) e = std::move(e) | bgcolor(kSelBg) | focus;
        body.push_back(hit(std::move(e), nav ? &nav->rows[i] : nullptr));
    }
    if (rows.size() == 1) {
        body.push_back(text("   这个周期内还没有用量记录") | color(kDim));
    }
    if (rep.has_unpriced) {
        body.push_back(text(""));
        body.push_back(text(" ⚠ 带警告的模型在 models.dev 中找不到价格，只统计 token，不计入美元") |
                       color(kWarn));
    }

    Element table = vbox(std::move(body)) | vscroll_indicator | yframe | flex;
    if (nav) table = std::move(table) | reflect(nav->table_view);

    return vbox({
               usage_tabs(sel, nav),
               separatorLight() | color(kDim),
               period_cards(rep, sel, nav),
               hbox({text(" 构成  ") | color(kDim), comp_bar(all.tok, std::max(10, width - 12)),
                     filler()}),
               hbox({text(" "), comp_legend(all.tok, width)}),
               separatorLight() | color(kDim),
               hbox(std::move(head)),
               std::move(table),
           }) |
           borderStyled(ROUNDED, c) | flex;
}

// ---- per-day view ----

struct Bucket {
    std::string label;  // sort key / date
    Tokens tok;
    double usd = 0;
    bool unpriced = false;
    bool today = false;
    int weekday = -1;  // only for day granularity
};

// Rows of the selected scope. tool -1 = both, model empty = every model.
// Returns newest first, continuous from the first day with data to today.
std::vector<Bucket> build_series(const Report& rep, int tool, const std::string& model, int gran) {
    auto key_of = [&](const std::string& day) {
        return gran == 0 ? day : gran == 1 ? week_start(day) : month_start(day).substr(0, 7);
    };

    std::map<std::string, Bucket> acc;
    std::string first;
    for (const auto& r : rep.daily) {
        if (tool >= 0 && static_cast<int>(r.tool) != tool) continue;
        if (!model.empty() && r.model != model) continue;
        if (first.empty() || r.day < first) first = r.day;
        Bucket& b = acc[key_of(r.day)];
        b.tok += r.tok;
        b.usd += r.usd;
        if (!r.priced) b.unpriced = true;
    }
    std::vector<Bucket> out;
    if (acc.empty()) return out;

    std::string last = rep.today;
    if (acc.rbegin()->first > key_of(last)) last = acc.rbegin()->first;  // clock skew safety
    std::string k = key_of(first);
    const std::string end = key_of(last);
    for (int guard = 0; guard < 20000 && k <= end; ++guard) {
        Bucket b;
        if (auto it = acc.find(k); it != acc.end()) b = it->second;
        b.label = k;
        b.today = k == key_of(rep.today);
        if (gran == 0) b.weekday = weekday_of(k);
        out.push_back(std::move(b));
        if (gran == 0) {
            k = add_days(k, 1);
        } else if (gran == 1) {
            k = add_days(k, 7);
        } else {
            k = month_start(add_days(k + "-01", 32)).substr(0, 7);
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

Element sparkline(const std::vector<Bucket>& newest_first, int width, Color c) {
    static const char* blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const int n = static_cast<int>(newest_first.size());
    if (n == 0 || width <= 0) return text("");
    const int cols = std::min(width, n);
    std::vector<double> v(static_cast<size_t>(cols), 0.0);
    for (int i = 0; i < n; ++i) {
        // oldest -> newest, spread over the columns
        const auto& b = newest_first[static_cast<size_t>(n - 1 - i)];
        const int col = std::min(cols - 1, static_cast<int>(static_cast<int64_t>(i) * cols / n));
        v[static_cast<size_t>(col)] = std::max(v[static_cast<size_t>(col)], static_cast<double>(b.tok.total()));
    }
    const double mx = *std::max_element(v.begin(), v.end());
    std::string s;
    for (double x : v) {
        if (mx <= 0 || x <= 0) {
            s += " ";
            continue;
        }
        const int lvl = std::clamp(static_cast<int>(std::lround(x / mx * 7.0)), 0, 7);
        s += blocks[lvl];
    }
    return text(s) | color(c);
}

Element usage_detail(const Report& rep, UsageSel& sel, UsageNav* nav, int width) {
    const auto series = build_series(rep, sel.d_tool, sel.d_model, sel.gran);
    const int n = static_cast<int>(series.size());
    sel.d_cur = std::clamp(sel.d_cur, 0, std::max(0, n - 1));

    const int ti = sel.d_tool;
    const Color c = ti < 0 ? kTotalColor : tool_color(ti);
    const std::string who = (ti < 0 ? "Σ " : ti == 0 ? "✻ " : "◈ ") +
                            (sel.d_model.empty() ? std::string(ti < 0 ? "全部工具 · 全部模型"
                                                                      : std::string(tool_title(static_cast<Tool>(ti))) + " · 全部模型")
                                                 : sel.d_model);

    Tokens total;
    double usd = 0, peak = 0;
    int active = 0;
    std::string peak_label;
    for (const auto& b : series) {
        total += b.tok;
        usd += b.usd;
        if (b.tok.total() > 0) ++active;
        if (b.usd > peak || (peak == 0 && b.tok.total() > 0 && peak_label.empty())) {
            peak = b.usd;
            peak_label = b.label;
        }
    }
    static const char* gran_unit[] = {"天", "周", "月"};
    static const char* gran_name[] = {"日", "周", "月"};

    // title row with granularity tabs and the back button
    Elements title = {text(" "), hit(text(" ← 返回 ") | bold | color(kFg) | bgcolor(kKeyBg),
                                     nav ? &nav->back : nullptr),
                      text("  "), text(who) | bold | color(c), filler()};
    title.push_back(text("粒度 ") | color(kDim));
    for (int g = 0; g < 3; ++g) {
        Element e = text(std::string(" ") + gran_name[g] + " ");
        e = g == sel.gran ? e | bold | color(Color::Black) | bgcolor(kAccent) : e | color(kDim);
        title.push_back(hit(std::move(e), nav ? &nav->gran[g] : nullptr));
    }
    title.push_back(text(" "));

    Elements head_rows;
    head_rows.push_back(hbox(std::move(title)));
    head_rows.push_back(separatorLight() | color(kDim));

    if (n == 0) {
        head_rows.push_back(text("   还没有这一项的用量记录") | color(kDim));
        return vbox(std::move(head_rows)) | borderStyled(ROUNDED, c) | flex;
    }

    char buf[160];
    const double per = active > 0 ? usd / active : 0.0;
    std::snprintf(buf, sizeof buf, " 自 %s 起 · 共 %d %s (有用量 %d) · %s · %s tokens · 均 %s/%s",
                  series.back().label.c_str(), n, gran_unit[sel.gran], active, fmt_usd(usd).c_str(),
                  fmt_tokens(total.total()).c_str(), fmt_usd(per).c_str(), gran_unit[sel.gran]);
    head_rows.push_back(text(buf) | color(kFg));
    if (!peak_label.empty()) {
        std::snprintf(buf, sizeof buf, " 最高 %s · %s", peak_label.c_str(), fmt_usd(peak).c_str());
        head_rows.push_back(text(buf) | color(kDim));
    }
    head_rows.push_back(hbox({text(" 构成  ") | color(kDim),
                              comp_bar(total, std::max(10, width - 12)), filler()}));
    head_rows.push_back(hbox({text(" "), comp_legend(total, width)}));
    head_rows.push_back(hbox({text(" 走势  ") | color(kDim),
                              sparkline(series, std::max(10, width - 12), c), filler()}));
    head_rows.push_back(separatorLight() | color(kDim));

    // columns
    const int label_w = sel.gran == 0 ? 15 : sel.gran == 1 ? 12 : 9;
    int room = width - 8 - label_w - 8;
    auto take = [&](int w) {
        if (room >= w) {
            room -= w;
            return true;
        }
        return false;
    };
    const bool k_usd = take(11), k_tot = take(9), k_out = take(8), k_in = take(8), k_cr = take(8),
               k_cw = take(8);
    const int bar_w = std::clamp(room, 0, 56);

    Elements head = {text("   "), ltext(sel.gran == 0 ? "日期" : sel.gran == 1 ? "周起始" : "月份",
                                         label_w + 6) |
                                      color(kDim)};
    if (bar_w >= 6) head.push_back(ltext("构成", bar_w + 1) | color(kDim));
    head.push_back(filler());
    if (k_in) head.push_back(rtext("输入", 8) | color(kInput));
    if (k_out) head.push_back(rtext("输出", 8) | color(kOutput));
    if (k_cr) head.push_back(rtext("缓存读", 8) | color(kCacheRead));
    if (k_cw) head.push_back(rtext("缓存写", 8) | color(kCacheWrite));
    if (k_tot) head.push_back(rtext("合计", 9) | color(kDim));
    if (k_usd) head.push_back(rtext("费用", 11) | color(kDim));
    head.push_back(text(" "));
    head_rows.push_back(hbox(std::move(head)));

    int64_t max_tok = 1;
    for (const auto& b : series) max_tok = std::max(max_tok, b.tok.total());

    static const char* wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    Elements rows;
    if (nav) nav->d_rows.assign(series.size(), Box{});
    for (int i = 0; i < n; ++i) {
        const auto& b = series[static_cast<size_t>(i)];
        const bool on = i == sel.d_cur;
        const bool empty = b.tok.total() == 0;
        const bool weekend = b.weekday == 0 || b.weekday == 6;

        std::string lab = b.label;
        if (sel.gran == 1) lab += " 起";
        Elements line = {text(" "), text(on ? "▌" : " ") | color(c), text(" "),
                         ltext(lab, label_w) |
                             color(b.today ? kAccent : (empty ? kDim : kFg)) |
                             (b.today ? bold : nothing)};
        line.push_back(sel.gran == 0
                           ? ltext(std::string("周") + wd[b.weekday] + (b.today ? " ▸" : ""), 6) |
                                 color(b.today ? kAccent : (weekend ? kWarn : kDim))
                           : ltext(b.today ? "▸" : "", 6) | color(kAccent));
        if (bar_w >= 6) {
            if (empty) {
                line.push_back(ltext("·", bar_w + 1) | color(kDim));
            } else {
                const int cells = std::max<int>(
                    1, static_cast<int>(std::lround(static_cast<double>(b.tok.total()) / max_tok * bar_w)));
                line.push_back(hbox({comp_bar(b.tok, bar_w, cells)}) | size(WIDTH, EQUAL, bar_w + 1));
            }
        }
        line.push_back(filler());
        auto num = [&](int64_t v, Color col, int w) {
            return rtext(empty ? "" : fmt_tokens(v), w) | color(col);
        };
        if (k_in) line.push_back(num(b.tok.input, kInput, 8));
        if (k_out) line.push_back(num(b.tok.output, kOutput, 8));
        if (k_cr) line.push_back(num(b.tok.cache_read, kCacheRead, 8));
        if (k_cw) line.push_back(num(b.tok.cache_write, kCacheWrite, 8));
        if (k_tot) line.push_back(num(b.tok.total(), kFg, 9));
        if (k_usd) {
            line.push_back(rtext(empty ? "" : fmt_usd(b.usd) + (b.unpriced ? "⚠" : ""), 11) | bold |
                           color(b.unpriced ? kWarn : c));
        }
        line.push_back(text(" "));
        Element e = hbox(std::move(line));
        if (on) e = std::move(e) | bgcolor(kSelBg) | focus;
        rows.push_back(hit(std::move(e), nav ? &nav->d_rows[static_cast<size_t>(i)] : nullptr));
    }

    Element list = vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
    if (nav) list = std::move(list) | reflect(nav->detail_view);
    head_rows.push_back(std::move(list));
    return vbox(std::move(head_rows)) | borderStyled(ROUNDED, c) | flex;
}

Element render_usage(const Report* rep, UsageSel& sel, UsageNav* nav, int width,
                     const StatusView& st) {
    if (!rep) {
        return vbox({filler(),
                     hbox({filler(), text(st.busy ? "◌ 正在读取会话日志并计算用量…"
                                                  : "暂无数据") |
                                         color(kDim),
                           filler()}),
                     filler()}) |
               flex;
    }
    Elements page;
    if (st.price_models == 0) {
        page.push_back(hbox({text(" ⚠ 没有可用的价格数据，美元金额暂时显示为 $0") | color(kWarn),
                             text(st.price_error.empty() ? "" : "  (" + st.price_error + ")") |
                                 color(kDim)}));
    }
    page.push_back(sel.detail ? usage_detail(*rep, sel, nav, width)
                              : usage_main(*rep, sel, nav, width));
    return vbox(std::move(page)) | flex;
}

Element frame(int page, const StatusView& st, Element body, const std::string& toast,
              bool toast_err, int width, int footer_mode) {
    return vbox({render_header(page, st, width), std::move(body) | flex,
                 render_footer(footer_mode, toast, toast_err)});
}

// ---- interactive app -----------------------------------------------------------

struct Form {
    bool open = false;
    int tool = 0;       // 0 claude, 1 codex
    bool edit = false;
    std::string key;    // claude: original name, codex: provider id
    std::string name, url, token, models, selected_model;
    int field = 0;
    std::string error;
};

struct ModelPicker {
    int tool = 0;
    std::string key, name;
    std::vector<ModelOption> models;
    std::vector<std::string> labels;
    int selected = 0;
    std::string error;
};

class App {
public:
    explicit App(std::unique_ptr<Store> store)
        : screen_(ScreenInteractive::Fullscreen()),
          worker_(std::make_unique<Worker>(std::move(store), [this] {
              screen_.PostEvent(Event::Custom);
          })) {}

    int run() {
        reload();
        build_components();
        worker_->start();
        screen_.Loop(root_);
        return 0;
    }

private:
    ScreenInteractive screen_;
    std::unique_ptr<Worker> worker_;  // destroyed before screen_ is torn down

    int page_ = 0;
    UsageSel usage_;
    UsageNav nav_;
    SwitchView view_;
    Form form_;
    ModelPicker picker_;
    int mode_ = 0;  // 0 main, 1 form, 2 model picker
    std::string toast_;
    bool toast_err_ = false;
    int64_t toast_at_ = 0;
    int64_t reload_at_ = 0;
    std::string pending_delete_;

    Component in_name_, in_url_, in_token_, in_model_, form_container_, form_comp_, main_comp_, root_;
    Component model_menu_, model_comp_;

    // ---- state ----
    void reload() {
        view_.claude = claude::load();
        view_.codex = codex::load();
        reload_at_ = now_unix();
        clamp_selection();
    }

    size_t row_count(int col) const {
        return col == 0 ? claude_rows(view_.claude).size() : codex_rows(view_.codex).size();
    }

    void clamp_selection() {
        for (int c = 0; c < 2; ++c) {
            int n = static_cast<int>(row_count(c));
            view_.sel[c] = std::clamp(view_.sel[c], 0, std::max(0, n - 1));
        }
    }

    void notify(const std::string& msg, bool err = false) {
        toast_ = msg;
        toast_err_ = err;
        toast_at_ = now_unix();
    }

    // ---- actions ----
    void activate() {
        std::string err;
        const int sel = view_.sel[view_.col];
        if (view_.col == 0) {
            const auto& st = view_.claude;
            if (sel == 0) {
                if (claude::apply(nullptr, err)) notify("✔ Claude Code → 官方登录（新会话生效）");
                else notify("✘ " + err, true);
            } else if (sel - 1 < static_cast<int>(st.profiles.size())) {
                const auto& p = st.profiles[sel - 1];
                if (p.models.size() > 1) { open_model_picker(); return; }
                if (claude::apply(&p, err)) notify("✔ Claude Code → " + p.name + "（新会话生效）");
                else notify("✘ " + err, true);
            } else {
                notify("按 i 把当前外部配置保存为一个配置", true);
            }
        } else {
            const auto& st = view_.codex;
            if (sel == 0) {
                if (codex::use("", err)) notify("✔ Codex → OpenAI 默认（新会话生效）");
                else notify("✘ " + err, true);
            } else if (sel - 1 < static_cast<int>(st.providers.size())) {
                const auto& p = st.providers[sel - 1];
                if (p.models.size() > 1) { open_model_picker(); return; }
                if (codex::use(p.id, err)) notify("✔ Codex → " + p.name + "（新会话生效）");
                else notify("✘ " + err, true);
            }
        }
        reload();
    }

    void open_model_picker() {
        const int index = view_.sel[view_.col] - 1;
        picker_ = ModelPicker{};
        picker_.tool = view_.col;
        std::string selected;
        if (view_.col == 0 && index >= 0 && index < static_cast<int>(view_.claude.profiles.size())) {
            const auto& p = view_.claude.profiles[index];
            picker_.key = picker_.name = p.name;
            picker_.models = p.models;
            selected = index == view_.claude.active_index ? view_.claude.active.model : p.model;
        } else if (view_.col == 1 && index >= 0 && index < static_cast<int>(view_.codex.providers.size())) {
            const auto& p = view_.codex.providers[index];
            picker_.key = p.id;
            picker_.name = p.name;
            picker_.models = p.models;
            selected = p.id == view_.codex.active_id ? view_.codex.active_model : p.model;
        } else {
            notify("请先选择一个已保存的中转", true);
            return;
        }
        if (picker_.models.empty()) {
            open_form(true);
            form_.field = 3;
            return;
        }
        for (size_t i = 0; i < picker_.models.size(); ++i) {
            const auto& m = picker_.models[i];
            picker_.labels.push_back(m.name == m.id ? m.id : m.name + " → " + m.id);
            if (m.id == selected) picker_.selected = static_cast<int>(i);
        }
        mode_ = 2;
    }

    void submit_model() {
        if (picker_.models.empty()) return;
        const auto& option = picker_.models[picker_.selected];
        std::string err;
        if (picker_.tool == 0) {
            auto profiles = claude::load().profiles;
            const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const claude::Profile& p) {
                return p.name == picker_.key;
            });
            if (it == profiles.end() || std::find(it->models.begin(), it->models.end(), option) == it->models.end()) {
                picker_.error = "中转或模型已改变，请关闭列表后重试";
                return;
            }
            it->model = option.id;
            if (!claude::save_profiles(profiles, err)) { picker_.error = err; return; }
            if (!claude::apply(&*it, err)) {
                picker_.error = "模型选择已保存，但切换失败: " + err;
                return;
            }
        } else if (!codex::use(picker_.key, option.id, err)) {
            picker_.error = err;
            return;
        }
        notify("✔ " + std::string(tool_title(static_cast<Tool>(picker_.tool))) + " → " +
               picker_.name + " · " + picker_.labels[picker_.selected] + "（新会话生效）");
        mode_ = 0;
        reload();
    }

    void open_form(bool edit) {
        form_ = Form{};
        form_.open = true;
        form_.tool = view_.col;
        form_.edit = edit;
        const int sel = view_.sel[view_.col];
        if (edit) {
            if (view_.col == 0) {
                if (sel < 1 || sel - 1 >= static_cast<int>(view_.claude.profiles.size())) {
                    notify("这一项不能编辑", true);
                    form_.open = false;
                    return;
                }
                const auto& p = view_.claude.profiles[sel - 1];
                form_.key = p.name;
                form_.name = p.name;
                form_.url = p.base_url;
                form_.token = p.token;
                form_.models = format_models(p.models);
                form_.selected_model = p.model;
            } else {
                if (sel < 1 || sel - 1 >= static_cast<int>(view_.codex.providers.size())) {
                    notify("这一项不能编辑", true);
                    form_.open = false;
                    return;
                }
                const auto& p = view_.codex.providers[sel - 1];
                form_.key = p.id;
                form_.name = p.name;
                form_.url = p.base_url;
                form_.token = p.token;
                form_.models = format_models(p.models);
                form_.selected_model = p.model;
            }
        }
        mode_ = 1;
    }

    void close_form() {
        form_.open = false;
        mode_ = 0;
    }

    void submit_form() {
        std::string err;
        const std::string name = trim_copy(form_.name), url = trim_copy(form_.url),
                          token = trim_copy(form_.token);
        if (name.empty() || url.empty()) {
            form_.error = "名称和 Base URL 不能为空";
            return;
        }
        std::vector<ModelOption> models;
        if (!parse_model_list(form_.models, models, err)) { form_.error = err; return; }
        const bool keep_selection = std::any_of(models.begin(), models.end(), [&](const ModelOption& m) {
            return m.id == form_.selected_model;
        });
        const std::string model = keep_selection ? form_.selected_model
                                                : models.empty() ? "" : models.front().id;

        if (form_.tool == 0) {
            auto profiles = view_.claude.profiles;
            claude::Profile np{name, url, token, model, models};
            bool was_active = false;
            if (form_.edit) {
                for (size_t i = 0; i < profiles.size(); ++i) {
                    if (profiles[i].name == form_.key) {
                        was_active = static_cast<int>(i) == view_.claude.active_index;
                        profiles[i] = np;
                    }
                }
            } else {
                for (const auto& p : profiles) {
                    if (p.name == name) {
                        form_.error = "已存在同名配置";
                        return;
                    }
                }
                profiles.push_back(np);
            }
            if (!claude::save_profiles(profiles, err)) {
                form_.error = err;
                return;
            }
            form_.key = name;
            if (was_active && !claude::apply(&np, err)) {
                form_.edit = true;
                form_.error = "配置已保存，但重新应用失败: " + err;
                reload();
                return;
            }
            notify("✔ 已保存 " + name);
        } else {
            codex::Provider p;
            p.name = name;
            p.base_url = url;
            p.token = token;
            p.model = model;
            p.models = models;
            if (form_.edit) {
                p.id = form_.key;
            } else {
                p.id = codex::slugify(name);
                if (p.id.empty()) {
                    form_.error = "名称里需要包含字母或数字";
                    return;
                }
                for (const auto& e : view_.codex.providers) {
                    if (e.id == p.id) {
                        form_.error = "已存在 id 为 " + p.id + " 的 provider";
                        return;
                    }
                }
            }
            if (!codex::upsert(p, err)) {
                form_.error = err;
                return;
            }
            notify("✔ 已保存 " + name + (form_.edit ? "" : "  (id: " + p.id + ")"));
        }
        close_form();
        reload();
    }

    void remove_selected() {
        const int sel = view_.sel[view_.col];
        std::string err;
        if (view_.col == 0) {
            if (sel < 1 || sel - 1 >= static_cast<int>(view_.claude.profiles.size())) return;
            const std::string name = view_.claude.profiles[sel - 1].name;
            if (pending_delete_ != "c:" + name) {
                pending_delete_ = "c:" + name;
                notify("再按一次 d 确认删除 “" + name + "”", true);
                return;
            }
            auto profiles = view_.claude.profiles;
            profiles.erase(profiles.begin() + (sel - 1));
            if (claude::save_profiles(profiles, err)) notify("✔ 已删除 " + name);
            else notify("✘ " + err, true);
        } else {
            if (sel < 1 || sel - 1 >= static_cast<int>(view_.codex.providers.size())) return;
            const auto& p = view_.codex.providers[sel - 1];
            if (p.id == view_.codex.active_id) {
                notify("正在使用的 provider 不能删除，请先切换到别的", true);
                return;
            }
            if (pending_delete_ != "x:" + p.id) {
                pending_delete_ = "x:" + p.id;
                notify("再按一次 d 确认删除 “" + p.name + "”（会改写 config.toml）", true);
                return;
            }
            if (codex::remove(p.id, err)) notify("✔ 已删除 " + p.name);
            else notify("✘ " + err, true);
        }
        pending_delete_.clear();
        reload();
    }

    void import_external() {
        if (view_.col != 0 || !view_.claude.active.has_override || view_.claude.active_index >= 0) {
            notify("没有可导入的外部配置", true);
            return;
        }
        auto profiles = view_.claude.profiles;
        std::string name = url_host(view_.claude.active.base_url);
        std::string unique = name;
        for (int n = 2;; ++n) {
            bool dup = std::any_of(profiles.begin(), profiles.end(),
                                   [&](const claude::Profile& p) { return p.name == unique; });
            if (!dup) break;
            unique = name + " " + std::to_string(n);
        }
        profiles.push_back({unique, view_.claude.active.base_url, view_.claude.active.token,
                            view_.claude.active.model, {}});
        std::string err;
        if (claude::save_profiles(profiles, err)) notify("✔ 已保存为 " + unique);
        else notify("✘ " + err, true);
        reload();
    }

    static std::string trim_copy(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    // ---- components ----
    StatusView status_view() const {
        StatusView sv;
        auto snap = worker_->snapshot();
        sv.busy = snap.busy;
        sv.status = snap.status;
        sv.price_error = snap.price_error;
        sv.scanned_at = snap.scanned_at;
        if (snap.report) {
            sv.price_models = snap.report->price_models;
            sv.prices_at = snap.report->prices_at;
        }
        return sv;
    }

    Element form_overlay() {
        const char* tool = form_.tool == 0 ? "Claude Code" : "Codex";
        const Color c = form_.tool == 0 ? kClaude : kCodex;
        auto field = [&](const std::string& label, Component input) {
            return hbox({text(" " + label) | color(kDim) | size(WIDTH, EQUAL, 12),
                         input->Render() | flex});
        };
        Elements rows = {
            hbox({text(std::string(" ") + (form_.edit ? "编辑" : "添加") + " · " + tool) | bold |
                  color(c)}),
            separatorLight() | color(kDim),
            text(""),
            field("名称", in_name_),
            field("Base URL", in_url_),
            field("Token", in_token_),
            field("模型列表", in_model_),
            text(" 多个用逗号分隔；支持直接 ID 或 名称=中转模型ID") | color(kDim),
            text(" 例如 gpt-4o, sonnet=claude-3-5-sonnet-20241022；留空保留当前模型") | color(kDim),
            text(" 模型列表和映射会同步到 CLI 的模型选择菜单") | color(kDim),
            text(""),
        };
        if (!form_.error.empty()) {
            rows.push_back(text(" ✘ " + form_.error) | color(kErr));
        } else if (form_.tool == 1 && !form_.edit) {
            rows.push_back(text(" 将写入 config.toml 的 [model_providers.*]") | color(kDim));
        } else if (form_.edit) {
            rows.push_back(text(" 修改后如果它正在使用，会立即重新应用") | color(kDim));
        } else {
            rows.push_back(text(""));
        }
        rows.push_back(hbox({key_hint("Enter", "下一项 / 保存"), key_hint("↑↓", "移动"),
                             key_hint("Esc", "取消")}));
        return vbox(std::move(rows)) | borderStyled(HEAVY, c) | size(WIDTH, EQUAL, 72);
    }

    Element model_picker_overlay() {
        const Color c = picker_.tool == 0 ? kClaude : kCodex;
        return vbox({
            text(" 选择模型 · " + std::string(tool_title(static_cast<Tool>(picker_.tool))) +
                 " · " + picker_.name) | bold | color(c),
            separatorLight() | color(kDim),
            model_menu_->Render() | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 12),
            text(picker_.error.empty() ? " 将切换到此中转，并应用所选模型；新会话生效"
                                      : " ✘ " + picker_.error) | color(picker_.error.empty() ? kDim : kErr),
            hbox({key_hint("↑↓", "选择"), key_hint("Enter", "切换"), key_hint("Esc", "取消")}),
        }) | borderStyled(HEAVY, c) | size(WIDTH, EQUAL, 84);
    }

    std::shared_ptr<const Report> report() const { return worker_->snapshot().report; }

    void open_detail(int tool, const std::string& model) {
        usage_.detail = true;
        usage_.d_tool = tool;
        usage_.d_model = model;
        usage_.d_cur = 0;
    }

    void open_selected() {
        const auto rep = report();
        if (!rep) return;
        const auto rows = usage_rows(*rep, usage_.scope, usage_.period);
        const auto& r = rows[static_cast<size_t>(row_index(rows, usage_.key))];
        open_detail(r.aggregate ? (usage_.scope == 0 ? -1 : usage_.scope - 1) : r.tool,
                    r.aggregate ? std::string() : r.model);
    }

    // Moves the table cursor, or the per-day cursor while that view is open.
    void usage_move(int amount, bool absolute) {
        const auto rep = report();
        if (!rep) return;
        if (usage_.detail) {
            const int n = static_cast<int>(
                build_series(*rep, usage_.d_tool, usage_.d_model, usage_.gran).size());
            usage_.d_cur = std::clamp(absolute ? amount : usage_.d_cur + amount, 0, std::max(0, n - 1));
            return;
        }
        const auto rows = usage_rows(*rep, usage_.scope, usage_.period);
        const int cur = row_index(rows, usage_.key);
        const int next = std::clamp(absolute ? amount : cur + amount, 0, static_cast<int>(rows.size()) - 1);
        const auto& r = rows[static_cast<size_t>(next)];
        usage_.key = row_key(r.tool, r.model);
    }

    bool on_usage_mouse(const Mouse& m) {
        const auto rep = report();
        if (!rep) return false;

        if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
            usage_move((m.button == Mouse::WheelUp ? -1 : 1) * (usage_.detail ? 3 : 1), false);
            return true;
        }
        if (m.button != Mouse::Left || m.motion != Mouse::Pressed) return false;

        if (usage_.detail) {
            if (nav_.back.Contain(m.x, m.y)) {
                usage_.detail = false;
                return true;
            }
            for (int g = 0; g < 3; ++g)
                if (nav_.gran[g].Contain(m.x, m.y)) {
                    usage_.gran = g;
                    usage_.d_cur = 0;
                    return true;
                }
            if (nav_.detail_view.Contain(m.x, m.y)) {
                for (size_t i = 0; i < nav_.d_rows.size(); ++i)
                    if (nav_.d_rows[i].Contain(m.x, m.y)) {
                        usage_.d_cur = static_cast<int>(i);
                        return true;
                    }
            }
            return false;
        }

        for (int sc = 0; sc < kScopeCount; ++sc)
            if (nav_.tab[sc].Contain(m.x, m.y)) {
                usage_.scope = sc;
                return true;
            }
        for (int p = 0; p < kPeriodCount; ++p)
            if (nav_.period[p].Contain(m.x, m.y)) {
                usage_.period = p;
                return true;
            }
        if (nav_.table_view.Contain(m.x, m.y)) {
            const auto rows = usage_rows(*rep, usage_.scope, usage_.period);
            for (size_t i = 0; i < nav_.rows.size() && i < rows.size(); ++i) {
                if (!nav_.rows[i].Contain(m.x, m.y)) continue;
                usage_.key = row_key(rows[i].tool, rows[i].model);
                open_selected();
                return true;
            }
        }
        return false;
    }

    bool on_main_event(Event e) {
        if (e.is_mouse()) return page_ == 1 && on_usage_mouse(e.mouse());
        if (e == Event::Custom) {
            if (now_unix() - reload_at_ >= 3) reload();
            return false;
        }
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            screen_.ExitLoopClosure()();
            return true;
        }
        if (e == Event::Character('1')) { page_ = 0; return true; }
        if (e == Event::Character('2')) { page_ = 1; return true; }
        if (e == Event::Character('r') || e == Event::Character('R')) {
            worker_->refresh(true);
            reload();
            notify("正在刷新价格并重新扫描…");
            return true;
        }

        if (page_ == 1) {
            const bool up = e == Event::ArrowUp || e == Event::Character('k');
            const bool down = e == Event::ArrowDown || e == Event::Character('j');
            if (up) { usage_move(-1, false); return true; }
            if (down) { usage_move(1, false); return true; }
            if (e == Event::PageUp) { usage_move(-10, false); return true; }
            if (e == Event::PageDown) { usage_move(10, false); return true; }
            if (e == Event::Home) { usage_move(0, true); return true; }
            if (e == Event::End) { usage_move(1 << 20, true); return true; }

            if (usage_.detail) {
                if (e == Event::Escape || e == Event::Backspace || e == Event::Character('b')) {
                    usage_.detail = false;
                    return true;
                }
                int g = -1;
                if (e == Event::Character('g')) g = (usage_.gran + 1) % 3;
                if (e == Event::Character('d')) g = 0;
                if (e == Event::Character('w')) g = 1;
                if (e == Event::Character('m')) g = 2;
                if (g >= 0) {
                    usage_.gran = g;
                    usage_.d_cur = 0;
                    return true;
                }
                return false;
            }

            if (e == Event::ArrowLeft || e == Event::Character('h')) {
                usage_.period = (usage_.period + kPeriodCount - 1) % kPeriodCount;
                return true;
            }
            if (e == Event::ArrowRight || e == Event::Character('l')) {
                usage_.period = (usage_.period + 1) % kPeriodCount;
                return true;
            }
            if (e == Event::Tab) { usage_.scope = (usage_.scope + 1) % kScopeCount; return true; }
            if (e == Event::TabReverse) {
                usage_.scope = (usage_.scope + kScopeCount - 1) % kScopeCount;
                return true;
            }
            if (e == Event::Return) { open_selected(); return true; }
            return false;
        }

        // switch page
        int& sel = view_.sel[view_.col];
        const int n = static_cast<int>(row_count(view_.col));
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            sel = std::max(0, sel - 1);
            return true;
        }
        if (e == Event::ArrowDown || e == Event::Character('j')) {
            sel = std::min(std::max(0, n - 1), sel + 1);
            return true;
        }
        if (e == Event::Tab || e == Event::TabReverse || e == Event::ArrowLeft ||
            e == Event::ArrowRight || e == Event::Character('h') || e == Event::Character('l')) {
            view_.col = 1 - view_.col;
            return true;
        }
        if (e == Event::Return) { activate(); return true; }
        if (e == Event::Character('a')) { open_form(false); return true; }
        if (e == Event::Character('e')) { open_form(true); return true; }
        if (e == Event::Character('d')) { remove_selected(); return true; }
        if (e == Event::Character('i')) { import_external(); return true; }
        if (e == Event::Character('m')) { open_model_picker(); return true; }
        pending_delete_.clear();
        return false;
    }

    void build_components() {
        in_name_ = Input(&form_.name, "例如 anyrouter");
        in_url_ = Input(&form_.url, "https://example.com/v1");
        in_token_ = Input(&form_.token, "sk-…");
        in_model_ = Input(&form_.models, "gpt-4o, sonnet=claude-3-5-sonnet-20241022");
        form_container_ = Container::Vertical({in_name_, in_url_, in_token_, in_model_}, &form_.field);

        form_comp_ = CatchEvent(form_container_, [this](Event e) {
            if (e == Event::Escape) {
                close_form();
                return true;
            }
            if (e == Event::Return) {
                form_.error.clear();
                if (form_.field >= 3) submit_form();
                else ++form_.field;
                return true;
            }
            if (e.is_character()) form_.error.clear();
            return false;
        });

        model_menu_ = Menu(&picker_.labels, &picker_.selected);
        model_comp_ = CatchEvent(model_menu_, [this](Event e) {
            if (e == Event::Escape) { mode_ = 0; reload(); return true; }
            if (e == Event::Return) { submit_model(); return true; }
            if (e == Event::ArrowUp || e == Event::ArrowDown) picker_.error.clear();
            return false;
        });

        // the focusable Renderer overload: a plain one would make the Tab container
        // unfocusable and swallow every key
        main_comp_ = CatchEvent(Renderer([](bool) { return emptyElement(); }),
                                [this](Event e) { return on_main_event(e); });

        auto tab = Container::Tab({main_comp_, form_comp_, model_comp_}, &mode_);
        root_ = Renderer(tab, [this]() -> Element {
            const StatusView sv = status_view();
            const auto snap = worker_->snapshot();
            if (!toast_.empty() && now_unix() - toast_at_ > 6) toast_.clear();

            Element body;
            if (page_ == 0) {
                body = render_switch(view_);
            } else {
                body = render_usage(snap.report.get(), usage_, &nav_, Terminal::Size().dimx, sv);
            }
            if (mode_ == 1) {
                // the form replaces the page body, so nothing bleeds through
                body = vbox({filler(), hbox({filler(), form_overlay(), filler()}), filler()}) | flex;
            } else if (mode_ == 2) {
                body = vbox({filler(), hbox({filler(), model_picker_overlay(), filler()}), filler()}) | flex;
            }
            const int footer_mode = page_ == 0 ? 0 : (usage_.detail ? 2 : 1);
            return frame(page_, sv, std::move(body), toast_, toast_err_, Terminal::Size().dimx,
                         footer_mode);
        });
    }
};

}  // namespace

int run_tui(std::unique_ptr<Store> store) {
    App app(std::move(store));
    return app.run();
}

std::string render_snapshot(const std::string& page, int width, int height, const Report& rep,
                            int scope, int period, int row, int detail_gran) {
    StatusView sv;
    sv.status = "已是最新";
    sv.price_models = rep.price_models;
    sv.prices_at = rep.prices_at;

    Element body;
    int pg = 0;
    int footer_mode = 0;
    if (page == "usage") {
        pg = 1;
        footer_mode = 1;
        UsageSel sel;
        sel.scope = std::clamp(scope, 0, kScopeCount - 1);
        sel.period = std::clamp(period, 0, kPeriodCount - 1);
        const auto rows = usage_rows(rep, sel.scope, sel.period);
        const size_t idx = static_cast<size_t>(std::clamp(row, 0, static_cast<int>(rows.size()) - 1));
        sel.key = row_key(rows[idx].tool, rows[idx].model);
        if (detail_gran >= 0) {
            footer_mode = 2;
            sel.detail = true;
            sel.gran = std::clamp(detail_gran, 0, 2);
            sel.d_tool = rows[idx].aggregate ? (sel.scope == 0 ? -1 : sel.scope - 1) : rows[idx].tool;
            sel.d_model = rows[idx].aggregate ? std::string() : rows[idx].model;
        }
        body = render_usage(&rep, sel, nullptr, width, sv);
    } else {
        SwitchView v;
        v.claude = claude::load();
        v.codex = codex::load();
        body = render_switch(v);
    }
    Element doc = frame(pg, sv, std::move(body), "", false, width, footer_mode);
    Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    Render(screen, doc);
    return screen.ToString();
}

}  // namespace as
