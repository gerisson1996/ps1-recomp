#include "runtime/metrics.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace ps1::metrics {
namespace {

std::mutex g_mutex;
// `std::less<>` makes these heterogeneously comparable, so lookups take a
// string_view without allocating.
std::map<std::string, uint64_t, std::less<>> g_counters;
std::map<std::string, int64_t, std::less<>> g_states;

// -1 = unresolved, 0 = off, 1 = on.  Racing threads all compute the same
// value from the same environment, so the race is benign.
std::atomic<int> g_enabled{-1};

} // namespace

bool enabled() {
  int e = g_enabled.load(std::memory_order_relaxed);
  if (e < 0) {
    e = std::getenv("PS1_METRICS") != nullptr ? 1 : 0;
    g_enabled.store(e, std::memory_order_relaxed);
  }
  return e == 1;
}

void setEnabledForTesting(bool on) {
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

void count(std::string_view name, uint64_t n) {
  if (!enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_counters.find(name);
  if (it == g_counters.end())
    g_counters.emplace(std::string(name), n);
  else
    it->second += n;
}

void setState(std::string_view name, int64_t value) {
  if (!enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_states.find(name);
  if (it == g_states.end())
    g_states.emplace(std::string(name), value);
  else
    it->second = value;
}

uint64_t get(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_counters.find(name);
  return it == g_counters.end() ? 0u : it->second;
}

int64_t getState(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_states.find(name);
  return it == g_states.end() ? 0 : it->second;
}

bool dumpJsonTo(const char *path) {
  if (!path)
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ofstream out(path);
  if (!out)
    return false;
  // Metric names are internal literals, never user input, so they need no
  // JSON escaping.  Keep it that way.
  out << "{\n  \"counters\": {";
  bool first = true;
  for (const auto &kv : g_counters) {
    out << (first ? "\n" : ",\n") << "    \"" << kv.first << "\": " << kv.second;
    first = false;
  }
  out << (first ? "" : "\n") << "  },\n  \"state\": {";
  first = true;
  for (const auto &kv : g_states) {
    out << (first ? "\n" : ",\n") << "    \"" << kv.first << "\": " << kv.second;
    first = false;
  }
  out << (first ? "" : "\n") << "  }\n}\n";
  return out.good();
}

bool dumpJson() {
  if (!enabled())
    return false;
  return dumpJsonTo(std::getenv("PS1_METRICS"));
}

void reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters.clear();
  g_states.clear();
}

} // namespace ps1::metrics
