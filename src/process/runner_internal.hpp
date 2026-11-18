#ifndef SITOMETRON_PROCESS_RUNNER_INTERNAL_HPP_
#define SITOMETRON_PROCESS_RUNNER_INTERNAL_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "handoff_probe.hpp"
#include "platform.hpp"
#include "sitometron/process/runner.hpp"

namespace sitometron::process::internal {
struct TestHooks {
  void* context = nullptr;
  void (*reach)(void*, std::string_view) noexcept = nullptr;
  bool fail_before_spawn = false;
  std::optional<std::size_t> fail_thread_after;
  std::uint64_t initial_generation = 0;
};
struct Diagnostics {
  std::size_t occupied = 0;
  std::size_t closing = 0;
  std::size_t threads_created = 0;
  std::size_t threads_joined = 0;
  std::size_t processes_created = 0;
  std::size_t processes_reaped = 0;
  std::size_t cooperative_deliveries = 0;
  std::size_t forced_deliveries = 0;
  std::size_t undrained_requests = 0;
  std::size_t handoff_locks = 0;
  std::size_t handoff_ingress = 0;
  std::size_t max_completions_per_thread = 0;
  std::vector<std::string> delivery_order;
  std::uint64_t max_generation = 0;
};
inline constexpr std::uint64_t k_occupied = 1;
inline constexpr std::uint64_t k_claiming = 2;
inline constexpr std::uint64_t k_active = 4;
inline constexpr std::uint64_t k_closing = 6;
inline constexpr std::uint64_t k_phase_mask = 6;
inline constexpr std::uint64_t k_cooperative = 8;
inline constexpr std::uint64_t k_forced = 16;
inline constexpr std::uint64_t k_cooperative_delivered = 32;
inline constexpr std::uint64_t k_forced_delivered = 64;
inline constexpr std::uint64_t k_generation_step = 128;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

struct WakeLevel {
  std::atomic_bool level{false};
  void Set() noexcept {
    level.store(true, std::memory_order_release);
    level.notify_one();
  }
  void Clear() noexcept { (void)level.exchange(false, std::memory_order_acq_rel); }
  void Wait() const noexcept { level.wait(false, std::memory_order_acquire); }
};
struct Cell {
  std::atomic<std::uint64_t> word{0};
  // Routing bytes are writer-only. The slot moves request, never these bytes.
  std::array<char, 36> job{};
  std::size_t job_size = 0;
  core::ApplicationLaunchRequest request;
  Process process;
  bool gone = true;  // Guarded by the mailbox mutex, checked before touching process.
};
struct Mailbox {
  std::array<Cell, 2> cells;
  std::mutex mutex;
  WakeLevel wake;
};
struct PortState {
  explicit PortState(std::size_t max_jobs);
  const std::size_t count;
  std::unique_ptr<Mailbox[]> mailboxes;
  WakeLevel service_wake;
  std::atomic_bool fault{false};
  std::atomic_bool stopping{false};
  const TestHooks* hooks = nullptr;  // Installed before threads; production leaves this unset.
  std::atomic_size_t threads_created{0};
  std::atomic_size_t threads_joined{0};
  std::atomic_size_t processes_created{0};
  std::atomic_size_t processes_reaped{0};
  std::atomic_size_t cooperative_deliveries{0};
  std::atomic_size_t forced_deliveries{0};
  std::atomic_size_t max_completions{0};
  std::atomic_size_t undrained_requests{0};
  std::mutex diagnostics_mutex;
  std::vector<std::string> delivery_order;  // Test-only, bounded/reserved at construction.
  void Reach(std::string_view point) const noexcept {
    if (hooks != nullptr && hooks->reach != nullptr) hooks->reach(hooks->context, point);
  }
  void Stop(const core::ApplicationStopRequest&, std::uint64_t flag) noexcept;
};
struct TestAccess {
  static void Install(RunnerPort&, const TestHooks*) noexcept;
  static Diagnostics Inspect(RunnerPort&);
  // Static state probes only, before any producer exists.
  static void SeedPhases(const RunnerPort&, std::uint64_t first, std::uint64_t second) noexcept;
  static bool Contains(const RunnerPort&, const core::Uuid&) noexcept;
  static bool FaultRecorded(const RunnerPort&) noexcept;
  static void ExerciseMutex(const RunnerPort&);
  static void WakeService(const RunnerPort&) noexcept;
};
}  // namespace sitometron::process::internal
#endif  // SITOMETRON_PROCESS_RUNNER_INTERNAL_HPP_
