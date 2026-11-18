#ifndef SITOMETRON_DAEMON_SESSION_PRODUCER_HPP_
#define SITOMETRON_DAEMON_SESSION_PRODUCER_HPP_

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "handoff_probe.hpp"
#include "job_orchestrator.hpp"

// Private composition implementation, also exercised directly without the HTTP
// transport in the selected TSan orchestration executable. Not an installed API.
namespace sitometron::daemon::internal {
using core::internal::JobOrchestrator;
using process::internal::ProbeCore;
using process::internal::ProbeMutex;
class Phase1SessionRetainer final : public core::SessionRetainerPort {
  struct Slot {
    core::SessionRetainRequest request;
    bool occupied = false;
  };

 public:
  explicit Phase1SessionRetainer(std::size_t capacity) : requests_(capacity) {}
  void HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept override {
    std::lock_guard lock(ProbeMutex(mutex_));
    if (stopped_) return;
    for (auto& slot : requests_)
      if (!slot.occupied) {
        static_assert(noexcept(slot.request = std::move(request)));
        slot.request = std::move(request);
        slot.occupied = true;
        wake_.notify_one();
        return;
      }
    overflow_ = true;
    wake_.notify_one();
  }
  std::optional<core::SessionRetainRequest> Take() {
    std::unique_lock lock(ProbeMutex(mutex_));
    wake_.wait(lock, [this] {
      return stopped_ || overflow_ ||
             std::ranges::any_of(requests_, [](const auto& r) { return r.occupied; });
    });
    if (stopped_ || overflow_) return std::nullopt;
    for (auto& slot : requests_)
      if (slot.occupied) {
        auto request = slot.request;
        slot.occupied = false;
        active_ = true;
        return request;
      }
    return std::nullopt;
  }
  void Done() noexcept {
    std::lock_guard lock(ProbeMutex(mutex_));
    active_ = false;
    wake_.notify_all();
  }
  bool WaitIdle() {
    std::unique_lock lock(ProbeMutex(mutex_));
    wake_.wait(lock, [this] {
      return stopped_ || overflow_ ||
             (!active_ &&
              std::ranges::none_of(requests_, [](const auto& r) { return r.occupied; }));
    });
    return !overflow_;
  }
  void Stop() noexcept {
    std::lock_guard lock(ProbeMutex(mutex_));
    stopped_ = true;
    wake_.notify_all();
  }
  void Reset() {
    std::lock_guard lock(ProbeMutex(mutex_));
    stopped_ = false;
    overflow_ = false;
    active_ = false;
    for (auto& r : requests_) {
      r.occupied = false;
      r.request.job_id.value.clear();
      r.request.session_id.value.clear();
    }
  }
  bool Overflowed() {
    std::lock_guard lock(ProbeMutex(mutex_));
    return overflow_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Slot> requests_;
  bool stopped_ = false;
  bool overflow_ = false;
  bool active_ = false;
};
struct SessionTestHooks {
  void* context = nullptr;
  void (*reach)(void*, std::string_view) noexcept = nullptr;
};
class SessionProducer final {
 public:
  explicit SessionProducer(Phase1SessionRetainer& mailbox, const SessionTestHooks* hooks = nullptr)
      : mailbox_(mailbox), hooks_(hooks) {}
  ~SessionProducer() {
    Stop();
    (void)Join();
  }
  void Start(JobOrchestrator& writer) {
    writer_ = &writer;
    auto capacity = ProbeCore([&writer] { return writer.RegisterCapacityWaiter(); });
    if (!capacity) throw std::invalid_argument("Session capacity registration exhausted");
    Reach("before_capacity_publish");
    {
      std::lock_guard lock(ProbeMutex(capacity_mutex_));
      capacity_ = std::move(capacity);
    }
    if (!ProbeCore([&writer] { return writer.ready(); })) return;
    endpoint_ = ProbeCore([&writer] { return writer.RetainedCallback(); });
    physically_joined_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
  }
  void Stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    mailbox_.Stop();
    std::shared_ptr<core::internal::CapacityWaiter> capacity;
    {
      std::lock_guard lock(ProbeMutex(capacity_mutex_));
      capacity = capacity_;
    }
    // No core/waiter operation runs under the publication mutex. The producer
    // thread starts after publication and never writes capacity_ itself.
    if (capacity) ProbeCore([&capacity] { capacity->Set(); });
  }
  bool Joined() const noexcept { return physically_joined_.load(std::memory_order_acquire); }
  bool Join() noexcept {
    {
      std::lock_guard lock(ProbeMutex(identity_mutex_));
      if (!joined_ && thread_id_ == std::this_thread::get_id()) return false;
    }
    std::lock_guard lock(ProbeMutex(join_mutex_));
    if (thread_.joinable()) thread_.join();
    {
      std::lock_guard lock(ProbeMutex(identity_mutex_));
      joined_ = true;
    }
    physically_joined_.store(true, std::memory_order_release);
    return true;
  }

 private:
  void Reach(std::string_view point) const noexcept {
    if (hooks_ && hooks_->reach) hooks_->reach(hooks_->context, point);
  }
  void Deliver(const core::RawCandidateEvent& event) {
    using enum core::internal::IngressCode;
    for (;;) {
      if (stopping_.load(std::memory_order_acquire)) return;
      Reach("before_capacity_clear");
      ProbeCore([this] { capacity_->Clear(); });
      if (const auto result = ProbeCore([this, &event] { return endpoint_.Invoke(event); });
          result.code == kNormalFull) {
        // Stop may have set this waiter's level before Clear consumed it. Never
        // wait after that stop, even if the core still returns normal_full.
        if (stopping_.load(std::memory_order_acquire)) return;
        Reach("before_capacity_wait");
        if (ProbeCore([this] { return capacity_->Wait(); })) continue;
      } else if (result.code == kAdmitted || result.code == kCoalescedPending) {
        (void)ProbeCore(
            [this, &result] { return writer_->AwaitCompletion(result.ingress_sequence); });
      }
      break;
    }
  }
  void Run() {
    {
      std::lock_guard lock(ProbeMutex(identity_mutex_));
      thread_id_ = std::this_thread::get_id();
    }
    try {
      while (auto request = mailbox_.Take()) {
        const core::RawCandidateEvent event{
            1, request->job_id, "session_retained",
            R"({"session_id":")" + request->session_id.value + R"("})"};
        Deliver(event);
        Reach("after_delivery");
        mailbox_.Done();
      }
      if (mailbox_.Overflowed()) ProbeCore([this] { return writer_->LatchReadinessFailure(); });
    } catch (...) {
      ProbeCore([this] { return writer_->LatchReadinessFailure(); });
    }
    mailbox_.Done();
    ProbeCore([this] { writer_->NotifyProducerExited(); });
  }
  Phase1SessionRetainer& mailbox_;
  const SessionTestHooks* hooks_;
  JobOrchestrator* writer_ = nullptr;
  core::internal::CallbackHandle endpoint_;
  std::shared_ptr<core::internal::CapacityWaiter> capacity_;
  std::mutex capacity_mutex_;
  std::atomic_bool stopping_{false};
  std::atomic_bool physically_joined_{true};
  std::mutex identity_mutex_;
  std::thread::id thread_id_{};
  bool joined_ = false;
  std::mutex join_mutex_;
  std::thread thread_;
};
}  // namespace sitometron::daemon::internal
#endif
