#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include "runner_internal.hpp"

namespace sitometron::process::internal {
namespace {
thread_local const ProducerState* running_producer = nullptr;
std::string JsonString(std::string_view value) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result = "\"";
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      result.push_back('\\');
      result.push_back(character);
    } else if (byte < 32) {
      result += "\\u00";
      result.push_back(digits[byte >> 4U]);
      result.push_back(digits[byte & 15U]);
    } else {
      result.push_back(character);
    }
  }
  result.push_back('"');
  return result;
}
core::RawCandidateEvent Observation(const core::ApplicationLaunchRequest& request, bool started) {
  return {1, request.job_id, "worker_launch_observed",
          "{\"operation_id\":" + JsonString(request.intent.operation_id.value) +
              ",\"outcome\":" + JsonString(started ? "started" : "failed") + "}"};
}
core::RawCandidateEvent Exit(const core::ApplicationLaunchRequest& request, std::string_view mode) {
  return {1, request.job_id, "process_exit_confirmed",
          "{\"launch_operation_id\":" + JsonString(request.intent.operation_id.value) +
              ",\"completion_mode\":" + JsonString(mode) + "}"};
}
std::string_view Mode(std::uint64_t word) noexcept {
  if ((word & k_forced) != 0) return "forced";
  if ((word & k_cooperative) != 0) return "cooperative";
  return "process_already_exited";
}
void Closing(Cell& cell) noexcept {
  // active=4 -> closing=6, preserving generation and concurrently recorded stop/delivery bits.
  (void)cell.word.fetch_or(k_closing ^ k_active, std::memory_order_acq_rel);
}
void Release(PortState& port, Cell& cell) noexcept {
  // RunCell swaps every string with a default slot-local string, transferring its
  // buffers (not merely leaving unspecified moved-from strings). That local is
  // destroyed, also on exception unwind, before this slot-thread release exchange.
  const auto& request = cell.request;
  if (const auto& intent = request.intent;
      !request.job_id.value.empty() || !intent.operation_id.value.empty() ||
      !intent.application_id.value.empty() || !intent.application_version.empty() ||
      !intent.bundle_sha256.value.empty() || !intent.allocation_id.value.empty() ||
      !intent.allocation_digest.value.empty() || !intent.worker_id.value.empty())
    ++port.undrained_requests;
  const auto word = cell.word.load(std::memory_order_acquire);
  const auto generation = word / k_generation_step;
  // 57-bit generations cover practical daemon lifetimes. Exhaustion is a debug precondition;
  // there is deliberately no new failure policy or wrap handling.
  assert(generation < (std::numeric_limits<std::uint64_t>::max() / k_generation_step));
  (void)cell.word.exchange((generation + 1) * k_generation_step, std::memory_order_acq_rel);
  port.Reach("cell_released");
}
enum class Answer : std::uint8_t { kAccepted, kFull, kClosed, kRejected };
struct Parked {
  std::mutex mutex;
  std::condition_variable wake;
  bool parked = false;
  bool in_flight = false;
  bool exited = true;
  bool closed = false;
  core::RawCandidateEvent observation;
};
}  // namespace

struct ProducerState {
  PortState& port;
  std::vector<LaunchSpecification> specifications;
  std::vector<std::unique_ptr<IngressHandle>> handles;
  std::vector<std::shared_ptr<CapacityWaiter>> waiters;
  FailureLatch& failure;
  std::unique_ptr<Parked[]> parking;
  std::vector<std::thread> slots;
  std::thread service;
  std::thread relay;
  std::mutex join_mutex;
  std::atomic_bool start_gate{false};

  ProducerState(PortState& table, std::vector<LaunchSpecification> entries,
                std::vector<std::unique_ptr<IngressHandle>> endpoints,
                std::vector<std::shared_ptr<CapacityWaiter>> levels, FailureLatch& latch)
      : port(table),
        specifications(std::move(entries)),
        handles(std::move(endpoints)),
        waiters(std::move(levels)),
        failure(latch),
        parking(std::make_unique<Parked[]>(table.count)) {
    if (handles.size() != port.count + 2 || waiters.size() != port.count + 1)
      throw std::invalid_argument("runner handle/waiter inventory mismatch");
    for (const auto& handle : handles)
      if (!handle) throw std::invalid_argument("runner ingress handle missing");
    for (const auto& waiter : waiters)
      if (!waiter) throw std::invalid_argument("runner capacity waiter missing");
    slots.reserve(port.count);
  }
  void ConstructionCheckpoint() const {
    if (port.hooks != nullptr && port.hooks->fail_thread_after == port.threads_created.load())
      throw std::runtime_error("injected runner thread construction failure");
  }
  void Start() {
    try {
      ConstructionCheckpoint();
      for (std::size_t index = 0; index != port.count; ++index) {
        slots.emplace_back([this, index] {
          start_gate.wait(false, std::memory_order_acquire);
          Slot(index);
        });
        ++port.threads_created;
        ConstructionCheckpoint();
      }
      service = std::thread([this] {
        start_gate.wait(false, std::memory_order_acquire);
        Service();
      });
      ++port.threads_created;
      ConstructionCheckpoint();
      relay = std::thread([this] {
        start_gate.wait(false, std::memory_order_acquire);
        Relay();
      });
      ++port.threads_created;
      ConstructionCheckpoint();
      start_gate.store(true, std::memory_order_release);
      start_gate.notify_all();
    } catch (...) {
      // No slot may spawn until all threads exist, including the force-stop service.
      RequestStop();
      start_gate.store(true, std::memory_order_release);
      start_gate.notify_all();
      Join();
      throw;
    }
  }
  void RequestStop() noexcept {
    port.stopping.store(true, std::memory_order_release);
    for (const auto& waiter : waiters) waiter->Set();
    for (std::size_t index = 0; index != port.count; ++index) port.mailboxes[index].wake.Set();
    port.service_wake.Set();
  }
  bool OwnThread() const noexcept { return running_producer == this; }
  void Join() noexcept {
    // A producer may request stop, but physical joining belongs to the external coordinator.
    if (OwnThread()) return;
    std::lock_guard lock(ProbeMutex(join_mutex));
    for (auto& slot : slots)
      if (slot.joinable()) {
        port.Reach("join_before_slot");
        slot.join();
        ++port.threads_joined;
      }
    if (service.joinable()) {
      service.join();
      ++port.threads_joined;
    }
    if (relay.joinable()) {
      relay.join();
      ++port.threads_joined;
    }
  }
  Answer Submit(std::size_t endpoint, core::RawCandidateEvent event) noexcept {
    // A synchronous handle includes consumption on this same thread; no second flight can start.
    port.max_completions.store(1, std::memory_order_relaxed);
    const auto result = handles[endpoint]->Submit(std::move(event));
    if (result.admission == AdmissionCode::kNormalFull) return Answer::kFull;
    if (result.admission == AdmissionCode::kAdmissionClosed ||
        result.admission == AdmissionCode::kServiceFailed)
      return Answer::kClosed;
    if (result.admission != AdmissionCode::kAdmitted) return Answer::kRejected;
    if (!result.completion) {
      std::cerr << "runner admitted input has no completion\n";
      port.Reach("missing_completion_reported");
      return Answer::kClosed;
    }
    if (result.completion->code == CompletionCode::kServiceFailed) return Answer::kClosed;
    return result.completion->code == CompletionCode::kSuccess ? Answer::kAccepted
                                                               : Answer::kRejected;
  }
  void RecordDelivery(bool forced) {
    if (forced)
      ++port.forced_deliveries;
    else
      ++port.cooperative_deliveries;
    if (port.hooks != nullptr) {
      std::lock_guard lock(ProbeMutex(port.diagnostics_mutex));
      if (port.delivery_order.size() < port.delivery_order.capacity())
        port.delivery_order.emplace_back(forced ? "forced" : "cooperative");
    }
    port.Reach("stop_delivered");
  }
  void DeliverLocked(Cell& cell) {
    if (cell.gone) return;
    auto word = cell.word.load(std::memory_order_acquire);
    if ((word & k_cooperative) != 0 && (word & k_cooperative_delivered) == 0) {
      (void)cell.word.fetch_or(k_cooperative_delivered, std::memory_order_acq_rel);
      cell.process.Signal(false);
      RecordDelivery(false);
    }
    word = cell.word.load(std::memory_order_acquire);
    if (((word & k_forced) != 0 || port.stopping.load(std::memory_order_acquire)) &&
        (word & k_forced_delivered) == 0) {
      (void)cell.word.fetch_or(k_forced_delivered, std::memory_order_acq_rel);
      cell.process.Signal(true);
      RecordDelivery(true);
    }
  }
  void Service() noexcept {
    running_producer = this;
    port.Reach("service_started");
    for (;;) {
      port.service_wake.Clear();
      const bool shutdown_scan = port.stopping.load(std::memory_order_acquire);
      port.Reach("service_scan_started");
      if (port.fault.exchange(false, std::memory_order_acq_rel)) {
        std::cerr << "runner mailbox invariant violated\n";
        failure.Latch("runner mailbox invariant violated");
      }
      for (std::size_t index = 0; index != port.count; ++index) {
        auto& mailbox = port.mailboxes[index];
        for (auto& cell : mailbox.cells) {
          if (const auto word = cell.word.load(std::memory_order_acquire);
              (word & (k_cooperative | k_forced)) == 0 && !shutdown_scan)
            continue;
          port.Reach("before_stop_delivery");
          std::lock_guard lock(ProbeMutex(mailbox.mutex));
          DeliverLocked(cell);
          port.Reach("stop_checked");
        }
      }
      port.Reach("service_scan_finished");
      // Only a scan selected in shutdown mode discharges the force-stop duty.
      // A stop arriving during a normal scan leaves service_wake set for another pass.
      if (shutdown_scan) {
        port.Reach("service_exited");
        return;
      }
      port.service_wake.Wait();
    }
  }
  void Park(std::size_t index, const core::ApplicationLaunchRequest& request) {
    port.Reach("before_park");
    auto candidate = Observation(request, true);
    {
      auto& entry = parking[index];
      std::lock_guard lock(ProbeMutex(entry.mutex));
      entry.observation = std::move(candidate);
      entry.parked = true;
    }
    port.Reach("after_park");
    waiters.back()->Set();
  }
  void CloseParking() const noexcept {
    for (std::size_t index = 0; index != port.count; ++index) {
      auto& entry = parking[index];
      std::lock_guard lock(ProbeMutex(entry.mutex));
      entry.closed = true;
      entry.parked = false;
      entry.wake.notify_all();
    }
  }
  void Relay() noexcept {
    running_producer = this;
    port.Reach("relay_started");
    try {
      while (!port.stopping.load(std::memory_order_acquire)) {
        waiters.back()->Clear();
        if (waiters.back()->Closed()) break;
        for (std::size_t index = 0; index != port.count; ++index) {
          auto& entry = parking[index];
          {
            std::lock_guard lock(ProbeMutex(entry.mutex));
            if (!entry.parked || entry.exited || entry.closed) continue;
          }
          // No claim yet: an exit observed while this hook is held must suppress the retry.
          port.Reach("relay_before_submit");
          core::RawCandidateEvent event;
          {
            std::lock_guard lock(ProbeMutex(entry.mutex));
            if (!entry.parked || entry.exited || entry.closed || port.stopping.load()) continue;
            event = entry.observation;
            entry.in_flight = true;
          }
          const auto answer = Submit(port.count + 1, std::move(event));
          {
            std::lock_guard lock(ProbeMutex(entry.mutex));
            entry.in_flight = false;
            if (answer != Answer::kFull) entry.parked = false;
            if (answer == Answer::kClosed) entry.closed = true;
            entry.wake.notify_all();
          }
        }
        port.Reach("relay_before_wait");
        if (port.stopping.load(std::memory_order_acquire) || !waiters.back()->Wait()) break;
      }
    } catch (...) {
      port.fault.store(true, std::memory_order_release);
      port.service_wake.Set();
    }
    CloseParking();
    port.Reach("relay_exited");
  }
  std::string_view Reap(std::size_t index, Cell& cell) noexcept {
    cell.process.Wait();
    port.Reach("process_wait_returned");
    auto& entry = parking[index];
    {
      std::lock_guard lock(ProbeMutex(entry.mutex));
      entry.exited = true;
      entry.parked = false;
    }
    std::uint64_t word = 0;
    {
      std::lock_guard lock(ProbeMutex(port.mailboxes[index].mutex));
      // ADR-0016 Section 4: "a stop recorded after the mark is ignored".
      // Issue #141's 2026-11-21 owner decision keeps this cut: a stop after
      // native exit but before this mark still counts and names the exit mode.
      cell.gone = true;
      word = cell.word.load(std::memory_order_acquire);
    }
    port.Reach("process_gone");
    port.Reach("before_reap");
    cell.process.Reap();
    ++port.processes_reaped;
    std::unique_lock lock(ProbeMutex(entry.mutex));
    entry.wake.wait(lock, [&entry] { return !entry.in_flight; });
    return Mode(word);
  }
  const LaunchSpecification* SpecificationFor(const core::StableId& application) const noexcept {
    for (const auto& candidate : specifications)
      if (candidate.application_id == application.value) return &candidate;
    return nullptr;
  }
  void RunCell(std::size_t index, Cell& cell) {
    // Keep the cell's strings default-empty before publishing its next generation.
    // A moved-from string is only valid-but-unspecified; swapping explicitly moves
    // every owned buffer onto this slot thread so the next handoff frees none.
    core::ApplicationLaunchRequest request;
    request.job_id.value.swap(cell.request.job_id.value);
    request.intent.operation_id.value.swap(cell.request.intent.operation_id.value);
    request.intent.application_id.value.swap(cell.request.intent.application_id.value);
    request.intent.application_version.swap(cell.request.intent.application_version);
    request.intent.bundle_sha256.value.swap(cell.request.intent.bundle_sha256.value);
    request.intent.allocation_id.value.swap(cell.request.intent.allocation_id.value);
    request.intent.allocation_digest.value.swap(cell.request.intent.allocation_digest.value);
    request.intent.worker_id.value.swap(cell.request.intent.worker_id.value);
    auto& entry = parking[index];
    {
      std::lock_guard lock(ProbeMutex(entry.mutex));
      entry.exited = false;
      entry.parked = false;
      entry.closed = false;
    }
    port.Reach("before_spawn_check");
    bool skip = false;
    {
      std::lock_guard lock(ProbeMutex(port.mailboxes[index].mutex));
      port.Reach("mailbox_lock_held");
      if (port.stopping.load(std::memory_order_acquire)) return;
      skip = (cell.word.load(std::memory_order_acquire) & (k_cooperative | k_forced)) != 0;
    }
    if (skip) {
      Closing(cell);
      (void)Submit(index, Exit(request, Mode(cell.word.load(std::memory_order_acquire))));
      return;
    }
    auto process = Process{};
    try {
      const auto* specification = SpecificationFor(request.intent.application_id);
      port.Reach("before_spawn");
      if (port.stopping.load(std::memory_order_acquire)) return;
      if (port.hooks != nullptr && port.hooks->fail_before_spawn)
        std::cerr << "injected operating-system creation failure\n";
      else if (specification != nullptr)
        process = Process::Spawn(*specification, request);
      else
        std::cerr << "runner has no specification for committed application\n";
    } catch (const std::exception& error) {
      std::cerr << "process start failed: " << error.what() << '\n';
    }
    const auto started = static_cast<bool>(process);
    if (started) {
      ++port.processes_created;
      port.Reach("spawn_before_publication");
      // A pending stop must not wait for the service thread to rediscover this publication.
      {
        std::lock_guard lock(ProbeMutex(port.mailboxes[index].mutex));
        cell.process = std::move(process);
        cell.gone = false;
        DeliverLocked(cell);
      }
    } else {
      Closing(cell);
    }
    port.Reach("before_observation");
    Answer answer;
    for (;;) {
      waiters[index]->Clear();
      answer = Submit(index, Observation(request, started));
      port.Reach("observation_answered");
      if (answer != Answer::kFull) break;
      if (started) {
        Park(index, request);
        break;
      }
      port.Reach("failed_before_capacity_wait");
      if (port.stopping.load(std::memory_order_acquire) || !waiters[index]->Wait()) {
        answer = Answer::kClosed;
        break;
      }
    }
    if (!started) {
      if (answer != Answer::kAccepted && answer != Answer::kClosed && !port.stopping.load())
        (void)Submit(index, Exit(request, Mode(cell.word.load(std::memory_order_acquire))));
      return;
    }
    port.Reach("slot_waiting_for_exit");
    const auto mode = Reap(index, cell);
    bool closed = answer == Answer::kClosed;
    {
      std::lock_guard lock(ProbeMutex(entry.mutex));
      closed = closed || entry.closed;
    }
    Closing(cell);
    if (!closed && !port.stopping.load(std::memory_order_acquire))
      (void)Submit(index, Exit(request, mode));
  }
  void Slot(std::size_t index) noexcept {
    running_producer = this;
    port.Reach("slot_started");
    auto& mailbox = port.mailboxes[index];
    std::size_t next = 0;
    for (;;) {
      mailbox.wake.Clear();
      Cell* cell = nullptr;
      for (std::size_t offset = 0; offset != 2; ++offset) {
        const auto selected = (next + offset) % 2;
        const auto word = mailbox.cells[selected].word.load(std::memory_order_acquire);
        if ((word & k_occupied) != 0 && (word & k_phase_mask) != k_claiming) {
          cell = &mailbox.cells[selected];
          next = 1 - selected;
          break;
        }
      }
      if (cell == nullptr) {
        if (port.stopping.load(std::memory_order_acquire)) break;
        mailbox.wake.Wait();
        continue;
      }
      try {
        RunCell(index, *cell);
      } catch (...) {
        port.fault.store(true, std::memory_order_release);
        port.service_wake.Set();
        if (cell->process) (void)Reap(index, *cell);
      }
      Release(port, *cell);
    }
    port.Reach("slot_exited");
  }
};
}  // namespace sitometron::process::internal

namespace sitometron::process {
RunnerProducer::RunnerProducer(RunnerPort& port, std::vector<LaunchSpecification> specifications,
                               std::vector<std::unique_ptr<IngressHandle>> handles,
                               std::vector<std::shared_ptr<CapacityWaiter>> waiters,
                               FailureLatch& failure)
    : state_(std::make_unique<internal::ProducerState>(*port.state_, std::move(specifications),
                                                       std::move(handles), std::move(waiters),
                                                       failure)) {
  internal::CheckChildWaitPolicy();
  state_->Start();
}
RunnerProducer::~RunnerProducer() {
  RequestStop();
  Join();
}
void RunnerProducer::RequestStop() noexcept { state_->RequestStop(); }
void RunnerProducer::Join() noexcept { state_->Join(); }
}  // namespace sitometron::process
