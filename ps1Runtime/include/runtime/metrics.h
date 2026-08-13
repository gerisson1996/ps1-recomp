#pragma once

#include <cstdint>
#include <string_view>

// Runtime metric collector.
//
// Exists because every legacy GPU log in this project is capped (FillRect at
// 10, CPU->VRAM at 20) or one-shot (the GP0 histogram fires at fixed word
// counts).  Two consecutive debugging sessions reached opposite conclusions
// from those logs.  Counters here are uncapped and are the source of truth for
// any decision.
//
// Disabled unless PS1_METRICS names an output path, so the hot path pays
// nothing in a normal run.
namespace ps1::metrics {

/// True when PS1_METRICS is set.  Resolved once from the environment.
bool enabled();

/// Add `n` to the counter `name`.  No-op while disabled.
void count(std::string_view name, uint64_t n = 1);

/// Record the latest value of `name`.  No-op while disabled.
void setState(std::string_view name, int64_t value);

/// Read a counter back.  Returns 0 when absent.
uint64_t get(std::string_view name);

/// Read a state field back.  Returns 0 when absent.
int64_t getState(std::string_view name);

/// Write the JSON report to the path in PS1_METRICS.
bool dumpJson();

/// Write the JSON report to an explicit path, ignoring PS1_METRICS.
bool dumpJsonTo(const char *path);

/// Drop everything recorded so far.
void reset();

/// Force the enabled flag, bypassing the environment.  For tests.
void setEnabledForTesting(bool on);

} // namespace ps1::metrics
