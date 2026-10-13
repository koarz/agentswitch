#pragma once

#include <cstdint>

#include "store.hpp"

namespace as {

struct ScanResult {
    int files = 0;      // log files seen
    int changed = 0;    // files that had new bytes
    int64_t events = 0; // usage events parsed this run
    int64_t bytes = 0;  // bytes read this run
};

// Incrementally ingests Claude Code and Codex session logs into the store.
// Safe to call repeatedly: every file is tracked by a byte cursor and every
// event is de-duplicated by a stable key.
ScanResult scan_all(Store& store);

}  // namespace as
