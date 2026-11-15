#include <algorithm>
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
        if ((word & k_occupied) == 0 || (word & k_phase_mask) == k_claiming ||
            std::string_view(cell.job.data(), cell.job_size) != request.job_id.value)
          continue;
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
}
Diagnostics TestAccess::Inspect(RunnerPort& port) {
  auto& state = *port.state_;
  Diagnostics result;
  for (std::size_t index = 0; index != state.count; ++index)
    for (const auto& cell : state.mailboxes[index].cells) {
      const auto word = cell.word.load(std::memory_order_acquire);
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
  std::lock_guard lock(state.diagnostics_mutex);
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
  static_assert(std::is_nothrow_move_assignable_v<core::ApplicationLaunchRequest>);
  for (std::size_t index = 0; index != state_->count; ++index) {
    auto& mailbox = state_->mailboxes[index];
    for (std::size_t cell_index = 0; cell_index != 2; ++cell_index) {
      auto& cell = mailbox.cells[cell_index];
      auto word = cell.word.load(std::memory_order_acquire);
      if ((word & k_occupied) != 0) continue;
      const auto partner = mailbox.cells[1 - cell_index].word.load(std::memory_order_acquire);
      if ((partner & k_occupied) != 0 && (partner & k_phase_mask) != k_closing) continue;
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
  state_->Stop(request, internal::k_cooperative);
}
void RunnerPort::HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept {
  state_->Stop(request, internal::k_forced);
}
}  // namespace sitometron::process
