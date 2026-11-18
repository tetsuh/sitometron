#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "runner_internal.hpp"

namespace sitometron::process::internal {
namespace {
std::size_t MailboxCount(std::size_t max_jobs) {
  if (max_jobs == 0 || max_jobs >= std::numeric_limits<std::size_t>::max() / 4)
    throw std::invalid_argument("invalid runner mailbox inventory");
  return max_jobs + 1;
}
bool Matches(const Cell& cell, std::uint64_t word, const core::Uuid& job) noexcept {
  return (word & k_occupied) != 0 && (word & k_phase_mask) != k_claiming &&
         std::string_view(cell.job.data(), cell.job_size) == job.value;
}
}  // namespace
PortState::PortState(std::size_t max_jobs)
    : count(MailboxCount(max_jobs)), mailboxes(std::make_unique<Mailbox[]>(count)) {
  delivery_order.reserve(count * 4);
}
void PortState::Stop(const core::ApplicationStopRequest& request, std::uint64_t flag) noexcept {
  for (unsigned pass = 0; pass != 2; ++pass) {
    bool changed = false;
    for (std::size_t index = 0; index != count; ++index) {
      auto& mailbox = mailboxes[index];
      for (std::size_t cell_index = 0; cell_index != 2; ++cell_index) {
        if (cell_index == 1) Reach("stop_between_scan_reads");
        auto& cell = mailbox.cells[cell_index];
        auto word = cell.word.load(std::memory_order_acquire);
        if (!Matches(cell, word, request.job_id)) continue;
        if ((word & flag) != 0) return;
        Reach("stop_before_cas");
        if (cell.word.compare_exchange_strong(word, word | flag, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          service_wake.Set();
          return;
        }
        changed = true;
      }
    }
    if (!changed) return;
    if (pass == 0) Reach("stop_before_rescan");
  }
}
void TestAccess::Install(RunnerPort& port, const TestHooks* hooks) noexcept {
  port.state_->hooks = hooks;
  if (hooks != nullptr && hooks->initial_generation != 0) {
    assert(hooks->initial_generation <=
           std::numeric_limits<std::uint64_t>::max() / k_generation_step);
    for (std::size_t index = 0; index != port.state_->count; ++index)
      for (auto& cell : port.state_->mailboxes[index].cells)
        cell.word.store(hooks->initial_generation * k_generation_step, std::memory_order_relaxed);
  }
}
void TestAccess::SeedPhases(const RunnerPort& port, std::uint64_t first,
                            std::uint64_t second) noexcept {
  const auto& state = *port.state_;
  state.mailboxes[0].cells[0].word.store(first == 0 ? 0 : first | k_occupied);
  state.mailboxes[0].cells[1].word.store(second == 0 ? 0 : second | k_occupied);
  // The remaining mailbox offers no place: its empty cell has an active partner.
  state.mailboxes[1].cells[0].word.store(k_occupied | k_active);
}
bool TestAccess::Contains(const RunnerPort& port, const core::Uuid& job) noexcept {
  for (std::size_t index = 0; index != port.state_->count; ++index)
    for (const auto& cell : port.state_->mailboxes[index].cells) {
      const auto word = cell.word.load(std::memory_order_acquire);
      if ((word & k_occupied) != 0 && (word & k_phase_mask) != k_claiming &&
          std::string_view(cell.job.data(), cell.job_size) == job.value)
        return true;
    }
  return false;
}
bool TestAccess::FaultRecorded(const RunnerPort& port) noexcept {
  return port.state_->fault.load(std::memory_order_acquire);
}
void TestAccess::ExerciseMutex(const RunnerPort& port) {
  std::lock_guard lock(ProbeMutex(port.state_->mailboxes[0].mutex));
}
Diagnostics TestAccess::Inspect(RunnerPort& port) {
  auto& state = *port.state_;
  Diagnostics result;
  for (std::size_t index = 0; index != state.count; ++index)
    for (const auto& cell : state.mailboxes[index].cells) {
      const auto word = cell.word.load(std::memory_order_acquire);
      result.max_generation = std::max(result.max_generation, word / k_generation_step);
      if ((word & k_occupied) != 0) ++result.occupied;
      if ((word & k_occupied) != 0 && (word & k_phase_mask) == k_closing) ++result.closing;
    }
  result.threads_created = state.threads_created.load();
  result.threads_joined = state.threads_joined.load();
  result.processes_created = state.processes_created.load();
  result.processes_reaped = state.processes_reaped.load();
  result.cooperative_deliveries = state.cooperative_deliveries.load();
  result.forced_deliveries = state.forced_deliveries.load();
  result.max_completions_per_thread = state.max_completions.load();
  result.undrained_requests = state.undrained_requests.load();
  result.handoff_locks = HandoffMutexEntries();
  result.handoff_ingress = HandoffCoreEntries();
  std::lock_guard lock(ProbeMutex(state.diagnostics_mutex));
  result.delivery_order = state.delivery_order;
  return result;
}
}  // namespace sitometron::process::internal

namespace sitometron::process {
RunnerPort::RunnerPort(std::size_t max_jobs)
    : state_(std::make_unique<internal::PortState>(max_jobs)) {}
RunnerPort::~RunnerPort() = default;
void RunnerPort::HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept {
  using namespace internal;
  const HandoffProbeScope probe(state_->hooks != nullptr);
  static_assert(std::is_nothrow_move_assignable_v<core::ApplicationLaunchRequest>);
  for (std::size_t index = 0; index != state_->count; ++index) {
    auto& mailbox = state_->mailboxes[index];
    for (std::size_t cell_index = 0; cell_index != 2; ++cell_index) {
      auto& cell = mailbox.cells[cell_index];
      auto word = cell.word.load(std::memory_order_acquire);
      if ((word & k_occupied) != 0) continue;
      if (const auto partner = mailbox.cells[1 - cell_index].word.load(std::memory_order_acquire);
          (partner & k_occupied) != 0 && (partner & k_phase_mask) != k_closing)
        continue;
      if (!cell.word.compare_exchange_strong(word, word | k_occupied | k_claiming,
                                             std::memory_order_acq_rel, std::memory_order_acquire))
        continue;
      // Core guarantees canonical UUID text. A malformed private caller must not overflow routing.
      if (request.job_id.value.size() > cell.job.size()) {
        cell.word.store(word, std::memory_order_release);
        state_->fault.store(true, std::memory_order_release);
        state_->service_wake.Set();
        return;
      }
      cell.job_size = request.job_id.value.size();
      std::copy(request.job_id.value.begin(), request.job_id.value.end(), cell.job.begin());
      // Owner decision (2026-11-18): no Sitometron-owned mutex and no core entry.
      // Internal STL/OS locks in this ADR-0005 move (including Windows Debug
      // iterator bookkeeping) are outside that rule; build settings stay unchanged.
      // The slot swapped out all prior buffers before its release exchange.
      cell.request = std::move(request);
      cell.word.store(word | k_occupied | k_active, std::memory_order_release);
      mailbox.wake.Set();
      return;
    }
  }
  state_->fault.store(true, std::memory_order_release);
  state_->service_wake.Set();
}
void RunnerPort::HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept {
  const internal::HandoffProbeScope probe(state_->hooks != nullptr);
  state_->Stop(request, internal::k_cooperative);
}
void RunnerPort::HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept {
  const internal::HandoffProbeScope probe(state_->hooks != nullptr);
  state_->Stop(request, internal::k_forced);
}
}  // namespace sitometron::process
