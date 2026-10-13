#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "pricing.hpp"
#include "report.hpp"
#include "store.hpp"
#include "ui.hpp"
#include "util.hpp"
#include "worker.hpp"

namespace {

using namespace as;

void usage(const char* argv0) {
    std::printf(
        "agentswitch - Codex / Claude Code 中转切换 + 用量统计\n\n"
        "用法:\n"
        "  %s                     启动 TUI\n"
        "  %s report              打印用量报告（先同步日志）\n"
        "  %s scan                只同步会话日志到 DuckDB\n"
        "  %s prices              强制刷新 models.dev 价格\n"
        "  %s snapshot [switch|usage] [WxH] [标签0-2] [周期0-3] [行] [粒度0-2]\n"
        "                          把一个页面渲染成文本输出（调试用）\n\n"
        "数据库: %s\n",
        argv0, argv0, argv0, argv0, argv0, (data_dir() / "usage.duckdb").c_str());
}

std::unique_ptr<Store> open_store() {
    try {
        return std::make_unique<Store>(data_dir() / "usage.duckdb");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        std::fprintf(stderr, "（如果另一个 agentswitch 正在运行，请先退出它）\n");
        return nullptr;
    }
}

void print_report(const Report& rep) {
    std::printf("agentswitch 用量报告   today=%s  events=%lld  prices=%zu models\n\n",
                rep.today.c_str(), static_cast<long long>(rep.events), rep.price_models);
    std::printf("%-8s %22s %22s %22s\n", "", "Claude Code", "Codex", "Total");
    static const char* names[] = {"today", "week", "month", "total"};
    for (int p = 0; p < kPeriodCount; ++p) {
        std::printf("%-8s", names[p]);
        for (int t = 0; t < 3; ++t) {
            const Sum& s = rep.sums[p][t];
            std::string cell = fmt_usd(s.usd) + " / " + fmt_tokens(s.tok.total());
            std::printf(" %22s", cell.c_str());
        }
        std::printf("\n");
    }
    std::printf("\n%-8s %-34s %10s %10s %10s %10s %12s\n", "tool", "model", "input", "output",
                "cache_r", "cache_w", "usd");
    for (const auto& m : rep.models[kTotal]) {
        std::string usd = m.priced_as.empty() ? "unpriced" : fmt_usd(m.usd);
        std::printf("%-8s %-34s %10s %10s %10s %10s %12s\n", tool_id(m.tool), m.model.c_str(),
                    fmt_tokens(m.tok.input).c_str(), fmt_tokens(m.tok.output).c_str(),
                    fmt_tokens(m.tok.cache_read).c_str(), fmt_tokens(m.tok.cache_write).c_str(),
                    usd.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage(argv[0]);
        return 0;
    }

    if (cmd.empty() || cmd == "tui") {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            std::fprintf(stderr, "TUI 需要在交互式终端中运行；非交互场景请用 `%s report`\n", argv[0]);
            return 2;
        }
        auto store = open_store();
        if (!store) return 1;
        return run_tui(std::move(store));
    }

    if (cmd == "report" || cmd == "scan" || cmd == "prices" || cmd == "snapshot") {
        auto store = open_store();
        if (!store) return 1;
        PriceBook book;
        std::string note;
        Report rep = Worker::run_once(*store, book, cmd == "prices", &note);

        if (cmd == "report") {
            print_report(rep);
        } else if (cmd == "scan") {
            std::printf("%s  total_events=%lld\n", note.c_str(),
                        static_cast<long long>(rep.events));
        } else if (cmd == "prices") {
            std::printf("prices: %zu models, fetched %s\n%s\n", rep.price_models,
                        rep.prices_at ? human_ago(now_unix() - rep.prices_at).c_str() : "never",
                        note.c_str());
        } else {
            std::string page = argc > 2 ? argv[2] : "usage";
            int w = 120, h = 40;
            if (argc > 3) std::sscanf(argv[3], "%dx%d", &w, &h);
            const int scope = argc > 4 ? std::atoi(argv[4]) : 0;
            const int period = argc > 5 ? std::atoi(argv[5]) : kMonth;
            const int row = argc > 6 ? std::atoi(argv[6]) : 0;
            const int detail = argc > 7 ? std::atoi(argv[7]) : -1;
            std::fputs(render_snapshot(page, w, h, rep, scope, period, row, detail).c_str(), stdout);
            std::fputc('\n', stdout);
        }
        return 0;
    }

    std::fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    usage(argv[0]);
    return 2;
}
