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
constexpr std::array<std::string_view, 28> kPoints{"before_spawn_check",
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
                                                   "before_reap"};
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
      return owner_.Submit(std::move(event));
    }

   private:
    Native& owner_;
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
    hooks_.context = this;
    hooks_.reach = [](void* context, std::string_view point) noexcept {
      static_cast<Native*>(context)->Reach(point);
    };
    adapter::internal::TestAccess::Install(port_, &hooks_);
    std::vector<adapter::LaunchSpecification> specifications;
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
    producer_ = std::make_unique<adapter::RunnerProducer>(port_, std::move(specifications),
                                                          std::move(endpoints),
                                                          std::move(waiter_interfaces), failure_);
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
    released_.store(true, std::memory_order_release);
    released_.notify_all();
  }
  void WaitPoint(std::string_view point, std::size_t expected) override {
    for (std::size_t index = 0; index != kPoints.size(); ++index) {
      if (kPoints[index] != point) continue;
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
            view.handoff_locks,
            view.handoff_ingress,
            view.max_completions_per_thread,
            failures_.load(),
            view.delivery_order};
  }

 private:
  void Reach(std::string_view point) noexcept {
    for (std::size_t index = 0; index != kPoints.size(); ++index)
      if (kPoints[index] == point) {
        counts_[index].fetch_add(1, std::memory_order_acq_rel);
        counts_[index].notify_all();
        break;
      }
    if (point != options_.hold_at) return;
    held_.fetch_add(1, std::memory_order_acq_rel);
    held_.notify_all();
    released_.wait(false, std::memory_order_acquire);
  }
  adapter::SubmissionResult Submit(core::RawCandidateEvent event) noexcept {
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
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Attempt> attempts_;
  std::size_t next_observation_ = 0;
  std::size_t next_exit_ = 0;
  std::array<std::atomic_size_t, kPoints.size()> counts_{};
  std::atomic_size_t held_{0};
  std::atomic_bool released_{false};
  std::atomic_bool closed_{false};
  std::atomic_size_t failures_{0};
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
}  // namespace sitometron::test::process
#else
namespace sitometron::test::process {
std::unique_ptr<Driver> MakeDriver(const Options&) { return nullptr; }
std::string QuoteWindowsArgument(std::string_view) {
  throw std::runtime_error("production Windows serializer is absent");
}
}  // namespace sitometron::test::process
#endif
