#pragma once

#include <memory>
#include <string>

#include "report.hpp"
#include "store.hpp"

namespace as {

// Runs the interactive TUI. Takes ownership of the store (used by the
// background worker). Returns the process exit code.
int run_tui(std::unique_ptr<Store> store);

// Renders one page to a string with ANSI colours, for `agentswitch snapshot`.
// page: "switch" | "usage". For the usage page: `scope` 0 全部 / 1 Claude /
// 2 Codex, `period` 0..3, `row` = table row (0 = aggregate) and `detail_gran`
// (-1 = table, 0/1/2 = per-day view by day / week / month).
std::string render_snapshot(const std::string& page, int width, int height, const Report& rep,
                            int scope = 0, int period = kMonth, int row = 0, int detail_gran = -1);

}  // namespace as
