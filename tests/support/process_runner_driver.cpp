#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "process_handoff_allocation_probe.hpp"
#include "process_runner_test_support.hpp"

// The RED commit must build without a production header/library. It does not supply
// a pretend runner. The same assertions bind to the native driver once the target exists.
#ifdef SITOMETRON_TEST_HAS_PROCESS
#include "runner_internal.hpp"
#include "sitometron/process/runner.hpp"
#include "windows_command_line.hpp"

namespace sitometron::test::process {
namespace {
namespace adapter = sitometron::process;
constexpr std::array<std::string_view, 30> k_points{"before_spawn_check",
                                                    "before_spawn",
                                                    "spawn_before_publication",
                                                    "before_observation",
                                                    "observation_answered",
                                                    "slot_waiting_for_exit",
                                                    "process_gone",
                                                    "cell_released",
                                                    "before_final_completion",
                                                    "before_stop_delivery",
                                                    "stop_before_cas",
                                                    "stop_between_scan_reads",
                                                    "stop_before_rescan",
                                                    "before_park",
                                                    "after_park",
                                                    "relay_before_wait",
                                                    "relay_before_submit",
                                                    "failed_before_capacity_wait",
                                                    "failure_latched",
                                                    "slot_started",
                                                    "service_started",
                                                    "relay_started",
                                                    "slot_exited",
                                                    "relay_exited",
                                                    "missing_completion_reported",
                                                    "stop_delivered",
                                                    "mailbox_lock_held",
                                                    "before_reap",
                                                    "observation_completion_pending",
                                                    "stop_checked"};
class Native final : public Driver {
  class Waiter final : public adapter::CapacityWaiter {
   public:
    void Clear() noexcept override { flag_.store(false, std::memory_order_release); }
    void Set() noexcept override {
      flag_.store(true, std::memory_order_release);
      flag_.notify_all();
    }
    bool Wait() noexcept override {
      flag_.wait(false, std::memory_order_acquire);
      return !closed_.load(std::memory_order_acquire);
    }
    bool Closed() const noexcept override { return closed_.load(std::memory_order_acquire); }
    void Close() noexcept {
      closed_.store(true, std::memory_order_release);
      Set();
    }

   private:
    std::atomic_bool flag_{false};
    std::atomic_bool closed_{false};
  };
  class Endpoint final : public adapter::IngressHandle {
   public:
    explicit Endpoint(Native& owner) : owner_(owner) {}
    adapter::SubmissionResult Submit(core::RawCandidateEvent event) noexcept override {
      if (invoking_.test_and_set(std::memory_order_acquire)) {
        ++owner_.registration_breaches_;
        return {};
      }
      const auto here = std::this_thread::get_id();
      if (thread_ == std::thread::id{}) thread_ = here;
      if (thread_ != here) ++owner_.registration_breaches_;
      auto result = owner_.Submit(event);
      invoking_.clear(std::memory_order_release);
      return result;
    }

   private:
    Native& owner_;
    std::atomic_flag invoking_{};
    std::thread::id thread_{};
  };
  class Failure final : public adapter::FailureLatch {
   public:
    explicit Failure(Native& owner) : owner_(owner) {}
    void Latch(std::string_view) noexcept override {
      ++owner_.failures_;
      owner_.CloseAdmission();
      owner_.Reach("failure_latched");
    }

   private:
    Native& owner_;
  };

 public:
  explicit Native(const Options& options)
      : options_(options), failure_(*this), port_(options.max_jobs) {
    for (const auto& point : options.hold_points)
      if (std::find(k_points.begin(), k_points.end(), point) == k_points.end())
        throw std::invalid_argument("unknown runner hold point");
    hooks_.context = this;
    hooks_.reach = [](void* context, std::string_view point) noexcept {
      static_cast<Native*>(context)->Reach(point);
    };
    hooks_.fail_before_spawn = options.fail_before_spawn;
    hooks_.fail_thread_after = options.fail_thread_after;
    hooks_.initial_generation = options.initial_generation;
    adapter::internal::TestAccess::Install(port_, &hooks_);
    std::vector<adapter::LaunchSpecification> specifications;
    specifications.reserve(options.specifications.size());
    for (const auto& entry : options.specifications)
      specifications.push_back(
          {entry.application_id, entry.executable, entry.arguments, entry.bundle_sha256});
    std::vector<std::unique_ptr<adapter::IngressHandle>> endpoints;
    for (std::size_t index = 0; index != options.max_jobs + 3; ++index)
      endpoints.push_back(std::make_unique<Endpoint>(*this));
    for (std::size_t index = 0; index != options.max_jobs + 2; ++index)
      waiters_.push_back(std::make_shared<Waiter>());
    std::vector<std::shared_ptr<adapter::CapacityWaiter>> waiter_interfaces(waiters_.begin(),
                                                                            waiters_.end());
    if (options.queued_before_start) Launch(*options.queued_before_start);
    try {
      producer_ = std::make_unique<adapter::RunnerProducer>(port_, std::move(specifications),
                                                            std::move(endpoints),
                                                            std::move(waiter_interfaces), failure_);
    } catch (...) {
      if (options.construction_failure != nullptr) *options.construction_failure = Inspect();
      throw;
    }
  }
  ~Native() override {
    Release();
    RequestStop();
    Join();
  }
  void Launch(core::ApplicationLaunchRequest request) override {
    const HandoffAllocationScope probe;
    port_.HandoffLaunch(std::move(request));
  }
  void Cooperative(core::ApplicationStopRequest request) override {
    const HandoffAllocationScope probe;
    port_.HandoffCooperativeStop(std::move(request));
  }
  void Forced(core::ApplicationStopRequest request) override {
    const HandoffAllocationScope probe;
    port_.HandoffForcedStop(std::move(request));
  }
  void WaitHeld(std::size_t expected) override {
    auto value = held_.load(std::memory_order_acquire);
    while (value < expected) {
      held_.wait(value, std::memory_order_acquire);
      value = held_.load(std::memory_order_acquire);
    }
  }
  void Release() noexcept override {
    for (auto& level : released_) {
      level.store(true, std::memory_order_release);
      level.notify_all();
    }
  }
  void ReleasePoint(std::string_view point) override {
    for (std::size_t index = 0; index != k_points.size(); ++index)
      if (k_points[index] == point) {
        released_[index].store(true, std::memory_order_release);
        released_[index].notify_all();
        return;
      }
    throw std::invalid_argument("unknown runner release point");
  }
  void WaitPoint(std::string_view point, std::size_t expected) override {
    for (std::size_t index = 0; index != k_points.size(); ++index) {
      if (k_points[index] != point) continue;
      auto value = counts_[index].load(std::memory_order_acquire);
      while (value < expected) {
        counts_[index].wait(value, std::memory_order_acquire);
        value = counts_[index].load(std::memory_order_acquire);
      }
      return;
    }
    throw std::runtime_error("unknown deterministic test point");
  }
  void Capacity(std::size_t index) override { waiters_.at(index)->Set(); }
  void CloseAdmission() override {
    closed_.store(true, std::memory_order_release);
    for (auto& waiter : waiters_) waiter->Close();
  }
  void RequestStop() noexcept override {
    if (producer_) producer_->RequestStop();
  }
  void Join() noexcept override {
    if (producer_) producer_->Join();
  }
  void WaitFacts(std::size_t observations, std::size_t exits) override {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] {
      std::size_t observed = 0;
      std::size_t exited = 0;
      for (const auto& attempt : attempts_) {
        if (attempt.answer.admission != Admission::kAdmitted) continue;
        if (attempt.event.event_type == "worker_launch_observed") ++observed;
        if (attempt.event.event_type == "process_exit_confirmed") ++exited;
      }
      return observed >= observations && exited >= exits;
    });
  }
  std::vector<Attempt> Attempts() override {
    std::lock_guard lock(mutex_);
    return attempts_;
  }
  Diagnostics Inspect() override {
    const auto view = adapter::internal::TestAccess::Inspect(port_);
    return {view.occupied,
            view.closing,
            view.threads_created,
            view.threads_joined,
            view.processes_created,
            view.processes_reaped,
            view.cooperative_deliveries,
            view.forced_deliveries,
            HandoffAllocationCount() - initial_handoff_allocations_,
            HandoffDeallocationCount() - initial_handoff_deallocations_,
            view.undrained_requests,
            view.handoff_locks - initial_handoff_locks_,
            view.handoff_ingress - initial_handoff_ingress_,
            view.max_completions_per_thread,
            failures_.load(),
            view.delivery_order,
            registration_breaches_.load(),
            view.max_generation};
  }

 private:
  void Reach(std::string_view point) noexcept {
    const auto found = std::find(k_points.begin(), k_points.end(), point);
    if (found == k_points.end()) return;
    const auto index = static_cast<std::size_t>(found - k_points.begin());
    counts_[index].fetch_add(1, std::memory_order_acq_rel);
    counts_[index].notify_all();
    if (point == options_.stop_on_point) {
      producer_->RequestStop();
      producer_->Join();  // Must be a nonblocking self-join refusal, followed by external join.
    }
    if (point != options_.hold_at &&
        std::find(options_.hold_points.begin(), options_.hold_points.end(), point) ==
            options_.hold_points.end())
      return;
    held_.fetch_add(1, std::memory_order_acq_rel);
    held_.notify_all();
    released_[index].wait(false, std::memory_order_acquire);
  }
  adapter::SubmissionResult Submit(const core::RawCandidateEvent& event) noexcept {
    adapter::internal::RecordCoreEntry();
    try {
      const bool observation = event.event_type == "worker_launch_observed";
      Answer answer;
      {
        std::lock_guard lock(mutex_);
        auto& index = observation ? next_observation_ : next_exit_;
        const auto& script = observation ? options_.observations : options_.exits;
        if (closed_.load(std::memory_order_acquire))
          answer.admission = Admission::kAdmissionClosed;
        else if (index < script.size())
          answer = script[index++];
        attempts_.push_back({event, answer, std::this_thread::get_id()});
        wake_.notify_all();
      }
      if (answer.admission == Admission::kAdmitted && observation)
        Reach("observation_completion_pending");
      if (answer.admission == Admission::kAdmitted &&
          (!observation || event.payload_json.find("failed") != std::string::npos))
        Reach("before_final_completion");
      adapter::SubmissionResult result;
      switch (answer.admission) {
        case Admission::kAdmitted:
          result.admission = adapter::AdmissionCode::kAdmitted;
          break;
        case Admission::kCoalescedPending:
          result.admission = adapter::AdmissionCode::kCoalescedPending;
          return result;
        case Admission::kAlreadyPending:
          result.admission = adapter::AdmissionCode::kAlreadyPending;
          return result;
        case Admission::kResidentLimit:
          result.admission = adapter::AdmissionCode::kResidentLimit;
          return result;
        case Admission::kNormalFull:
          result.admission = adapter::AdmissionCode::kNormalFull;
          return result;
        case Admission::kAdmissionClosed:
          result.admission = adapter::AdmissionCode::kAdmissionClosed;
          return result;
        case Admission::kServiceFailed:
          result.admission = adapter::AdmissionCode::kServiceFailed;
          return result;
      }
      if (answer.completion != Completion::kMissing) {
        adapter::CompletionResult completion;
        if (answer.completion == Completion::kRejected) {
          completion.code = adapter::CompletionCode::kRejected;
          completion.rejection = answer.rejection;
        } else if (answer.completion == Completion::kServiceFailed) {
          completion.code = adapter::CompletionCode::kServiceFailed;
        } else {
          completion.code = adapter::CompletionCode::kSuccess;
        }
        result.completion = completion;
      }
      return result;
    } catch (...) {
      std::terminate();
    }
  }
  Options options_;
  const std::size_t initial_handoff_allocations_ = HandoffAllocationCount();
  const std::size_t initial_handoff_deallocations_ = HandoffDeallocationCount();
  const std::size_t initial_handoff_locks_ = adapter::internal::HandoffMutexEntries();
  const std::size_t initial_handoff_ingress_ = adapter::internal::HandoffCoreEntries();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Attempt> attempts_;
  std::size_t next_observation_ = 0;
  std::size_t next_exit_ = 0;
  std::array<std::atomic_size_t, k_points.size()> counts_{};
  std::atomic_size_t held_{0};
  std::array<std::atomic_bool, k_points.size()> released_{};
  std::atomic_bool closed_{false};
  std::atomic_size_t failures_{0};
  std::atomic_size_t registration_breaches_{0};
  std::vector<std::shared_ptr<Waiter>> waiters_;
  Failure failure_;
  adapter::internal::TestHooks hooks_{};
  adapter::RunnerPort port_;
  std::unique_ptr<adapter::RunnerProducer> producer_;
};
}  // namespace
std::unique_ptr<Driver> MakeDriver(const Options& options) {
  return std::make_unique<Native>(options);
}
std::string QuoteWindowsArgument(std::string_view value) {
  return adapter::internal::QuoteWindowsArgument(value);
}
void VerifyCellPhaseMatrix(const core::ApplicationLaunchRequest& request) {
  const std::array<std::uint64_t, 4> phases{
      0, adapter::internal::k_claiming, adapter::internal::k_active, adapter::internal::k_closing};
  for (const auto first : phases) {
    for (const auto second : phases) {
      adapter::RunnerPort port(1);
      const adapter::internal::TestHooks hooks;
      adapter::internal::TestAccess::Install(port, &hooks);
      adapter::internal::TestAccess::SeedPhases(port, first, second);
      auto prepared = request;
      const auto before = HandoffAllocationCount();
      const auto free_before = HandoffDeallocationCount();
      const auto locks_before = adapter::internal::HandoffMutexEntries();
      const auto core_before = adapter::internal::HandoffCoreEntries();
      {
        const HandoffAllocationScope probe;
        port.HandoffLaunch(std::move(prepared));
      }
      const bool allowed =
          (first == 0 && (second == 0 || second == adapter::internal::k_closing)) ||
          (second == 0 && first == adapter::internal::k_closing);
      if (adapter::internal::TestAccess::Contains(port, request.job_id) != allowed ||
          adapter::internal::TestAccess::FaultRecorded(port) == allowed ||
          HandoffAllocationCount() != before || HandoffDeallocationCount() != free_before ||
          adapter::internal::HandoffMutexEntries() != locks_before ||
          adapter::internal::HandoffCoreEntries() != core_before)
        throw std::runtime_error("real-port empty/claiming/active/closing pair matrix failed");
    }
  }
}
void VerifyOwnedLockProbe() {
  adapter::RunnerPort port(1);
  const auto before = adapter::internal::HandoffMutexEntries();
  {
    const adapter::internal::HandoffProbeScope scope(true);
    adapter::internal::TestAccess::ExerciseMutex(port);
  }
  if (adapter::internal::HandoffMutexEntries() != before + 1)
    throw std::runtime_error("owned adapter mutex probe missed its planted acquisition");
}
}  // namespace sitometron::test::process
#else
namespace sitometron::test::process {
std::unique_ptr<Driver> MakeDriver(const Options&) { return nullptr; }
std::string QuoteWindowsArgument(std::string_view) {
  throw std::runtime_error("production Windows serializer is absent");
}
void VerifyCellPhaseMatrix(const core::ApplicationLaunchRequest&) {
  throw std::runtime_error("production mailbox table is absent");
}
void VerifyOwnedLockProbe() { throw std::runtime_error("production owned mutex probe is absent"); }
}  // namespace sitometron::test::process
#endif
