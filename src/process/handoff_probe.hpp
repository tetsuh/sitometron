#ifndef SITOMETRON_PROCESS_HANDOFF_PROBE_HPP_
#define SITOMETRON_PROCESS_HANDOFF_PROBE_HPP_
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace sitometron::process::internal {
// Test instrumentation for explicit Sitometron-owned mutex acquisitions and owned
// bridge entries into core. It does NOT observe hidden STL, CRT, or OS locks.
inline thread_local unsigned handoff_probe_depth = 0;
inline std::atomic_size_t handoff_mutex_entries{0};
inline std::atomic_size_t handoff_core_entries{0};
class HandoffProbeScope final {
 public:
  explicit HandoffProbeScope(bool enabled) noexcept : enabled_(enabled) {
    if (enabled_) ++handoff_probe_depth;
  }
  ~HandoffProbeScope() {
    if (enabled_) --handoff_probe_depth;
  }
  HandoffProbeScope(const HandoffProbeScope&) = delete;
  HandoffProbeScope& operator=(const HandoffProbeScope&) = delete;

 private:
  bool enabled_;
};
inline std::mutex& ProbeMutex(std::mutex& mutex) noexcept {
  if (handoff_probe_depth != 0) handoff_mutex_entries.fetch_add(1, std::memory_order_relaxed);
  return mutex;
}
inline void RecordCoreEntry() noexcept {
  if (handoff_probe_depth != 0) handoff_core_entries.fetch_add(1, std::memory_order_relaxed);
}
template <class Function>
decltype(auto) ProbeCore(Function&& function) {
  RecordCoreEntry();
  return std::forward<Function>(function)();
}
inline std::size_t HandoffMutexEntries() noexcept {
  return handoff_mutex_entries.load(std::memory_order_relaxed);
}
inline std::size_t HandoffCoreEntries() noexcept {
  return handoff_core_entries.load(std::memory_order_relaxed);
}
}  // namespace sitometron::process::internal
#endif
