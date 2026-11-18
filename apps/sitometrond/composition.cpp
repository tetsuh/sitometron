#include "composition.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "handoff_probe.hpp"
#include "job_orchestrator.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/identity/system_clock.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"
#include "sitometron/process/runner.hpp"
#include "sitometron/registry/registry.hpp"

namespace sitometron::daemon {
namespace {

using core::internal::IngressCode;
using core::internal::JobOrchestrator;
using process::internal::ProbeCore;
using process::internal::ProbeMutex;

class CountingClock final : public core::ClockPort {
 public:
  CountingClock(std::unique_ptr<core::ClockPort> port, std::atomic_size_t& reads)
      : port_(std::move(port)), reads_(reads) {
    if (!port_) throw std::invalid_argument("clock factory returned no port");
  }
  [[nodiscard]] core::ClockReading Read() override {
    ++reads_;
    return port_->Read();
  }

 private:
  std::unique_ptr<core::ClockPort> port_;
  std::atomic_size_t& reads_;
};

class CountingIdentitySource final : public core::IdentitySourcePort {
 public:
  CountingIdentitySource(std::unique_ptr<core::IdentitySourcePort> port, std::atomic_size_t& reads)
      : port_(std::move(port)), reads_(reads) {
    if (!port_) throw std::invalid_argument("identity factory returned no port");
  }
  [[nodiscard]] core::JobSessionIdentityResult GenerateJobSessionIdentity() override {
    ++reads_;
    return port_->GenerateJobSessionIdentity();
  }
  [[nodiscard]] core::WorkerIdentityResult GenerateWorkerIdentity() override {
    ++reads_;
    return port_->GenerateWorkerIdentity();
  }
  [[nodiscard]] core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    ++reads_;
    return port_->GenerateLaunchOperationIdentity();
  }

 private:
  std::unique_ptr<core::IdentitySourcePort> port_;
  std::atomic_size_t& reads_;
};

// Private deterministic observation gate, unset in production. It never holds a core lock.
class RunnerObservationGate final {
 public:
  void Reset(bool hold) noexcept {
    held_ = hold;
    reached_.store(0);
    released_.store(!hold);
  }
  void Reach() noexcept {
    if (!held_) return;
    ++reached_;
    reached_.notify_all();
    released_.wait(false, std::memory_order_acquire);
  }
  void Wait() const noexcept {
    auto reached = reached_.load(std::memory_order_acquire);
    while (reached == 0) {
      reached_.wait(reached, std::memory_order_acquire);
      reached = reached_.load(std::memory_order_acquire);
    }
  }
  void Release() noexcept {
    released_.store(true, std::memory_order_release);
    released_.notify_all();
  }

 private:
  bool held_ = false;  // Configured before any producer starts.
  std::atomic_size_t reached_{0};
  std::atomic_bool released_{true};
};
class RunnerProgress final {
 public:
  void Reset() {
    std::lock_guard lock(ProbeMutex(mutex_));
    exits_ = 0;
    closed_ = false;
  }
  void Close() noexcept {
    std::lock_guard lock(ProbeMutex(mutex_));
    closed_ = true;
    wake_.notify_all();
  }
  void Exited() noexcept {
    std::lock_guard lock(ProbeMutex(mutex_));
    ++exits_;
    wake_.notify_all();
  }
  bool Wait(std::size_t count) {
    std::unique_lock lock(ProbeMutex(mutex_));
    wake_.wait(lock, [this, count] { return closed_ || exits_ >= count; });
    return exits_ >= count;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::size_t exits_ = 0;
  bool closed_ = false;
};
class RunnerIngress final : public process::IngressHandle {
 public:
  RunnerIngress(JobOrchestrator& writer, RunnerObservationGate& gate, RunnerProgress& progress)
      : writer_(writer),
        endpoint_(ProbeCore([&writer] { return writer.RetainedCallback(); })),
        gate_(gate),
        progress_(progress) {}
  process::SubmissionResult Submit(core::RawCandidateEvent event) noexcept override {
    if (event.event_type == "worker_launch_observed") gate_.Reach();
    const bool exit = event.event_type == "process_exit_confirmed";
    const auto admitted = ProbeCore([this, &event] { return endpoint_.Invoke(std::move(event)); });
    process::SubmissionResult result;
    switch (admitted.code) {
      case IngressCode::kAdmitted:
        result.admission = process::AdmissionCode::kAdmitted;
        break;
      case IngressCode::kCoalescedPending:
        result.admission = process::AdmissionCode::kCoalescedPending;
        return result;
      case IngressCode::kAlreadyPending:
        result.admission = process::AdmissionCode::kAlreadyPending;
        return result;
      case IngressCode::kNormalFull:
        result.admission = process::AdmissionCode::kNormalFull;
        return result;
      case IngressCode::kResidentLimit:
        result.admission = process::AdmissionCode::kResidentLimit;
        return result;
      case IngressCode::kAdmissionClosed:
        result.admission = process::AdmissionCode::kAdmissionClosed;
        return result;
      case IngressCode::kServiceFailed:
        result.admission = process::AdmissionCode::kServiceFailed;
        return result;
    }
    const auto completion =
        ProbeCore([this, &admitted] { return writer_.AwaitCompletion(admitted.ingress_sequence); });
    if (!completion) {
      // Every admitted sequence completes once. Missing is an invariant breach, never success.
      (void)ProbeCore([this] { return writer_.LatchReadinessFailure(); });
      return result;
    }
    process::CompletionResult value;
    switch (completion->code) {
      case core::internal::Completion::Code::kSuccess:
        value.code = process::CompletionCode::kSuccess;
        break;
      case core::internal::Completion::Code::kReducerRejection:
        value.code = process::CompletionCode::kRejected;
        if (completion->rejection) value.rejection = completion->rejection->reason;
        break;
      case core::internal::Completion::Code::kResidentLimit:
        value.code = process::CompletionCode::kRejected;
        break;
      case core::internal::Completion::Code::kServiceFailed:
        value.code = process::CompletionCode::kServiceFailed;
        break;
    }
    result.completion = value;
    if (exit) progress_.Exited();
    return result;
  }

 private:
  JobOrchestrator& writer_;  // Composition-owned; every producer joins before core destruction.
  core::internal::CallbackHandle endpoint_;
  RunnerObservationGate& gate_;
  RunnerProgress& progress_;
};
class RunnerCapacity final : public process::CapacityWaiter {
 public:
  explicit RunnerCapacity(JobOrchestrator& writer)
      : waiter_(ProbeCore([&writer] { return writer.RegisterCapacityWaiter(); })) {
    if (!waiter_) throw std::invalid_argument("runner capacity registration exhausted");
  }
  void Clear() noexcept override {
    ProbeCore([this] { waiter_->Clear(); });
  }
  void Set() noexcept override {
    ProbeCore([this] { waiter_->Set(); });
  }
  bool Wait() noexcept override {
    return ProbeCore([this] { return waiter_->Wait(); });
  }
  bool Closed() const noexcept override {
    return ProbeCore([this] { return waiter_->Closed(); });
  }

 private:
  std::shared_ptr<core::internal::CapacityWaiter> waiter_;
};
class RunnerFailure final : public process::FailureLatch {
 public:
  explicit RunnerFailure(JobOrchestrator& writer) : writer_(writer) {}
  void Latch(std::string_view) noexcept override {
    (void)ProbeCore([this] { return writer_.LatchReadinessFailure(); });
  }

 private:
  JobOrchestrator& writer_;
};

// The port owns storage only. No writer/ingress pointer or result delivery in handoff.
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
        // Copy on the producer, never construct a request in the writer's handoff.
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

class SessionProducer final {
 public:
  explicit SessionProducer(Phase1SessionRetainer& mailbox) : mailbox_(mailbox) {}
  ~SessionProducer() {
    Stop();
    (void)Join();
  }
  void Start(JobOrchestrator& writer) {
    writer_ = &writer;
    capacity_ = ProbeCore([&writer] { return writer.RegisterCapacityWaiter(); });
    if (!capacity_) throw std::invalid_argument("Session capacity registration exhausted");
    if (!ProbeCore([&writer] { return writer.ready(); })) return;
    endpoint_ = ProbeCore([&writer] { return writer.RetainedCallback(); });
    physically_joined_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
  }
  void Stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    mailbox_.Stop();
    if (capacity_) ProbeCore([this] { capacity_->Set(); });
  }
  bool Joined() const noexcept { return physically_joined_.load(std::memory_order_acquire); }
  bool Join() noexcept {
    // Check self before the join lock: another thread may hold it while joining us.
    {
      std::lock_guard lock(ProbeMutex(identity_mutex_));
      if (!joined_ && thread_id_ == std::this_thread::get_id()) return false;
    }
    std::lock_guard lock(ProbeMutex(join_mutex_));
    if (thread_.joinable()) {
      thread_.join();
    }
    {
      std::lock_guard identity_lock(ProbeMutex(identity_mutex_));
      joined_ = true;
    }
    physically_joined_.store(true, std::memory_order_release);
    return true;
  }

 private:
  void Deliver(const core::RawCandidateEvent& event) {
    using enum IngressCode;
    for (;;) {
      if (stopping_.load(std::memory_order_acquire)) return;
      ProbeCore([this] { capacity_->Clear(); });
      if (const auto result = ProbeCore([this, &event] { return endpoint_.Invoke(event); });
          result.code == kNormalFull) {
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
        // Bound UUID strings need no escaping; application versions are escaped in core.
        const core::RawCandidateEvent event{
            1, request->job_id, "session_retained",
            R"({"session_id":")" + request->session_id.value + R"("})"};
        Deliver(event);
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
  JobOrchestrator* writer_ = nullptr;
  core::internal::CallbackHandle endpoint_;
  std::shared_ptr<core::internal::CapacityWaiter> capacity_;
  std::atomic_bool stopping_{false};
  std::atomic_bool physically_joined_{true};
  std::mutex identity_mutex_;
  std::thread::id thread_id_{};
  bool joined_ = false;
  std::mutex join_mutex_;
  std::thread thread_;
};

bool RegisteredApplicationForm(std::string_view value) {
  const auto alphanumeric = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 128 && alphanumeric(value.front()) &&
         std::ranges::all_of(value, [&](unsigned char c) {
           return alphanumeric(c) || c == '.' || c == '_' || c == ':' || c == '-';
         });
}

http::JobResultCode IngressStatus(IngressCode code) {
  switch (code) {
    case IngressCode::kServiceFailed:
      return http::JobResultCode::kServiceFailed;
    case IngressCode::kAdmissionClosed:
      return http::JobResultCode::kNotReady;
    case IngressCode::kResidentLimit:
      return http::JobResultCode::kCapacityExhausted;
    case IngressCode::kNormalFull:
    case IngressCode::kAlreadyPending:
      return http::JobResultCode::kBusy;
    case IngressCode::kAdmitted:
    case IngressCode::kCoalescedPending:
      return http::JobResultCode::kInternal;
  }
  return http::JobResultCode::kInternal;
}

class Phase1Backend final : public http::Backend {
 public:
  Phase1Backend(JobOrchestrator& orchestrator, std::size_t max_jobs,
                const std::vector<core::Snapshot>& replayed,
                const std::vector<core::internal::JobLaunchInputs>& applications)
      : orchestrator_(orchestrator), applications_(applications), order_(max_jobs) {
    if (max_jobs == 0 || replayed.size() > max_jobs ||
        !std::ranges::all_of(applications_, [](const auto& app) {
          return RegisteredApplicationForm(app.application_id.value);
        }))
      throw std::invalid_argument("invalid Backend composition");
    // Reserve every fixed-size UUID string before serving requests. Index publication reuses
    // this storage only after successful completion and confirmation of the committed resource.
    for (auto& id : order_) id.reserve(36);
    for (const auto& snapshot : replayed) order_[order_size_++] = snapshot.job_id.value;
  }

  [[nodiscard]] http::Readiness GetReadiness() override {
    http::Readiness result;
    result.ready = orchestrator_.ready();
    // A later failure must not attach a false-readiness reason to an earlier ready observation.
    if (!result.ready && orchestrator_.failed()) result.reasons.push_back({"service_failed", {}});
    std::vector<std::string> unresolved;
    for (const auto& id : orchestrator_.unresolved()) unresolved.push_back(id.value);
    if (!unresolved.empty()) result.reasons.push_back({"unresolved_jobs", std::move(unresolved)});
    if (!result.ready && result.reasons.empty()) result.reasons.push_back({"shutting_down", {}});
    return result;
  }

  [[nodiscard]] http::JobResult CreateJob(const std::string& application_id) override {
    const auto application = std::ranges::find_if(
        applications_, [&](const auto& app) { return app.application_id.value == application_id; });
    if (application == applications_.end())
      return {http::JobResultCode::kUnknownApplication, std::nullopt};
    // Serialize generation, admission, the sequence-specific wait, and index publication. Reads
    // use only the separate short-lived index mutex, never this create mutex.
    const std::lock_guard creation(ProbeMutex(create_mutex_));
    const auto created = orchestrator_.CreateJob(*application);
    if (created.ingress.code != IngressCode::kAdmitted)
      return {IngressStatus(created.ingress.code), std::nullopt};
    if (!created.job_id) return {};
    // Owner decision A, 2026-10-24: synchronously await this turn on the submitting I/O thread.
    const auto completion = orchestrator_.AwaitCompletion(created.ingress.ingress_sequence);
    if (!completion) return {};
    if (completion->code == core::internal::Completion::Code::kServiceFailed)
      return {http::JobResultCode::kServiceFailed, std::nullopt};
    if (completion->code == core::internal::Completion::Code::kResidentLimit)
      return {http::JobResultCode::kCapacityExhausted, std::nullopt};
    if (completion->code != core::internal::Completion::Code::kSuccess) return {};
    auto job = GetJob(created.job_id->value);
    if (!job) return {};
    {
      const std::lock_guard index(ProbeMutex(index_mutex_));
      // Compact only after successful installation. Failure reads retain the old index.
      std::size_t retained = 0;
      for (std::size_t i = 0; i != order_size_; ++i) {
        if (GetJob(order_[i])) {
          if (retained != i) order_[retained] = order_[i];
          ++retained;
        }
      }
      order_size_ = retained;
      if (std::find(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(order_size_),
                    job->job_id) == order_.begin() + static_cast<std::ptrdiff_t>(order_size_)) {
        if (order_size_ == order_.size()) throw std::logic_error("resident index exhausted");
        order_[order_size_++] = job->job_id;
      }
    }
    return {http::JobResultCode::kSuccess, std::move(job)};
  }

  [[nodiscard]] std::optional<http::JobResource> GetJob(const std::string& job_id) override {
    const auto snapshot = orchestrator_.SnapshotFor(core::Uuid{job_id});
    if (!snapshot || !snapshot->entity_exists) return std::nullopt;
    const auto state = std::string(core::ToString(snapshot->state));
    const auto terminal = core::IsTerminalState(snapshot->state);
    return http::JobResource{snapshot->job_id.value, state, terminal,
                             terminal ? std::optional<std::string>{state} : std::nullopt};
  }

  [[nodiscard]] std::vector<http::JobResource> ListJobs() override {
    std::vector<http::JobResource> result;
    const std::lock_guard index(ProbeMutex(index_mutex_));
    result.reserve(order_size_);
    for (std::size_t position = 0; position < order_size_; ++position) {
      auto job = GetJob(order_[position]);
      if (job) result.push_back(std::move(*job));
    }
    return result;
  }

  [[nodiscard]] http::JobResult CancelJob(const std::string& job_id,
                                          const std::string& principal) override {
    const auto input = orchestrator_.SubmitCommand(
        core::Command{1, core::CommandType::kCancel, core::Uuid{job_id}, principal});
    if (input.code != IngressCode::kAdmitted) return {IngressStatus(input.code), std::nullopt};
    const auto completion = orchestrator_.AwaitCompletion(input.ingress_sequence);
    if (!completion) return {};
    if (completion->code == core::internal::Completion::Code::kServiceFailed)
      return {http::JobResultCode::kServiceFailed, std::nullopt};
    if (completion->code == core::internal::Completion::Code::kReducerRejection) {
      if (!completion->rejection) return {};
      switch (completion->rejection->reason) {
        case core::RejectionReason::kJobNotFound:
          return {http::JobResultCode::kJobNotFound, std::nullopt};
        case core::RejectionReason::kStopCauseAlreadyLatched:
          return {http::JobResultCode::kStopCauseAlreadyLatched, std::nullopt};
        case core::RejectionReason::kCommandNotAllowedInState:
          return {http::JobResultCode::kCommandNotAllowedInState, std::nullopt};
        default:
          return {};
      }
    }
    auto job = GetJob(job_id);
    if (!job) return {};
    return {http::JobResultCode::kSuccess, std::move(job)};
  }

 private:
  JobOrchestrator& orchestrator_;
  const std::vector<core::internal::JobLaunchInputs> applications_;
  std::mutex create_mutex_;
  std::mutex index_mutex_;
  std::vector<std::string> order_;
  std::size_t order_size_ = 0;
};

}  // namespace

namespace {

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty() || text.front() == '+' || text.front() == '-') return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool ParseListen(std::string_view text, std::string& address, std::uint16_t& port) {
  std::string_view port_text;
  if (text.starts_with('[')) {
    const auto close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':')
      return false;
    address = std::string(text.substr(1, close - 1));
    port_text = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || text.find(':') != colon) return false;
    address = std::string(text.substr(0, colon));
    port_text = text.substr(colon + 1);
  }
  std::uint64_t parsed = 0;
  if (address.empty() || !ParseUnsigned(port_text, parsed) || parsed > 65535) return false;
  port = static_cast<std::uint16_t>(parsed);
  return true;
}

bool VisibleAscii(std::string_view text, std::size_t maximum) {
  return !text.empty() && text.size() <= maximum &&
         std::ranges::all_of(text, [](unsigned char c) { return c > ' ' && c <= '~'; });
}

}  // namespace

std::unique_ptr<http::Backend> MakeJobBackend(JobOrchestrator& orchestrator, std::size_t max_jobs,
                                              const std::vector<core::Snapshot>& replayed,
                                              const std::vector<std::string>& applications) {
  std::vector<core::internal::JobLaunchInputs> inputs;
  inputs.reserve(applications.size());
  for (const auto& id : applications)
    inputs.emplace_back(core::StableId{id}, "1", core::Digest{std::string(64, 'a')});
  return std::make_unique<Phase1Backend>(orchestrator, max_jobs, replayed, inputs);
}
std::unique_ptr<http::Backend> MakeJobBackend(
    JobOrchestrator& orchestrator, std::size_t max_jobs,
    const std::vector<core::Snapshot>& replayed,
    const std::vector<core::internal::JobLaunchInputs>& applications) {
  return std::make_unique<Phase1Backend>(orchestrator, max_jobs, replayed, applications);
}

ParseResult ParseStartupOptions(const std::vector<std::string>& args) {
  ParseResult result;
  result.config.http.listen_address = "127.0.0.1";
  result.config.http.listen_port = 8080;
  result.config.max_jobs = 32;
  std::set<std::string, std::less<>> seen;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string_view flag(args[index]);
    if (flag == "--help" || flag == "-h") {
      if (args.size() != 1) {
        result.error = "--help cannot be combined with other options";
        return result;
      }
      result.help = true;
      result.ok = true;
      return result;
    }
    if (!flag.starts_with("--") || !seen.insert(std::string(flag)).second) {
      result.error = "unknown or repeated option: " + std::string(flag);
      return result;
    }
    const bool known_option = flag == "--listen" || flag == "--journal" ||
                              flag == "--applications" || flag == "--cancel-principal" ||
                              flag == "--max-jobs" || flag == "--target-limit" ||
                              flag == "--header-limit" || flag == "--body-limit" ||
                              flag == "--header-timeout-ms" || flag == "--body-timeout-ms";
    if (!known_option) {
      result.error = "unknown option: " + std::string(flag);
      return result;
    }
    if (index + 1 >= args.size()) {
      result.error = "missing value for " + std::string(flag);
      return result;
    }
    const std::string_view value(args[++index]);
    if (value.starts_with("--")) {
      result.error = "missing value for " + std::string(flag);
      return result;
    }
    if (flag == "--listen") {
      if (!ParseListen(value, result.config.http.listen_address, result.config.http.listen_port)) {
        result.error = "--listen must be ADDRESS:PORT or [IPv6]:PORT";
        return result;
      }
    } else if (flag == "--journal") {
      if (value.empty()) {
        result.error = "--journal needs a directory";
        return result;
      }
      result.config.journal_path = value;
    } else if (flag == "--applications") {
      if (value.empty()) {
        result.error = "--applications needs a nonempty path";
        return result;
      }
      result.config.applications_path = value;
    } else if (flag == "--cancel-principal") {
      if (!VisibleAscii(value, 256)) {
        result.error = "--cancel-principal must be 1 to 256 visible ASCII characters";
        return result;
      }
      result.config.http.cancel_principal = value;
    } else {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, parsed)) {
        result.error = "malformed numeric value for " + std::string(flag);
        return result;
      }
      if (flag == "--max-jobs") {
        if (parsed < 1 || parsed > 1024) {
          result.error = "--max-jobs must be between 1 and 1024";
          return result;
        }
        result.config.max_jobs = static_cast<std::size_t>(parsed);
      } else if (flag == "--target-limit") {
        if (parsed < 2048 || parsed > 65536) {
          result.error = "--target-limit must be between 2048 and 65536";
          return result;
        }
        result.config.http.target_limit_bytes = parsed;
      } else if (flag == "--header-limit") {
        if (parsed < 8192 || parsed > 262144) {
          result.error = "--header-limit must be between 8192 and 262144";
          return result;
        }
        result.config.http.header_limit_bytes = parsed;
      } else if (flag == "--body-limit") {
        if (parsed < 4096 || parsed > 1048576) {
          result.error = "--body-limit must be between 4096 and 1048576";
          return result;
        }
        result.config.http.body_limit_bytes = parsed;
      } else if (flag == "--header-timeout-ms") {
        if (parsed == 0 || parsed > 60000) {
          result.error = "--header-timeout-ms must be between 1 and 60000";
          return result;
        }
        result.config.http.header_timeout_ms = parsed;
      } else if (flag == "--body-timeout-ms") {
        if (parsed == 0 || parsed > 60000) {
          result.error = "--body-timeout-ms must be between 1 and 60000";
          return result;
        }
        result.config.http.body_timeout_ms = parsed;
      } else {
        result.error = "unknown option: " + std::string(flag);
        return result;
      }
    }
  }
  if (result.config.journal_path.empty()) {
    result.error = "--journal is required";
    return result;
  }
  const auto header = result.config.http.header_limit_bytes;
  const auto body = result.config.http.body_limit_bytes;
  if (header > std::numeric_limits<std::uint64_t>::max() - body ||
      header + body > std::numeric_limits<std::uint64_t>::max() - 4096U) {
    result.error = "request bounds overflow";
    return result;
  }
  result.config.http.combined_request_limit_bytes = header + body + 4096U;
  result.ok = true;
  return result;
}

struct Phase1Composition::Impl {
  Impl(StartupConfig value, IdentityFactory source_factory)
      : config(std::move(value)), factory(source_factory), session(config.max_jobs + 1) {}

  StartupConfig config;
  registry::VerifiedApplications applications;
  IdentityFactory factory;
  std::atomic_size_t clock_read_count{0};
  std::atomic_size_t identity_read_count{0};
  std::unique_ptr<CountingClock> clock;
  std::unique_ptr<CountingIdentitySource> identity;
  std::unique_ptr<process::RunnerPort> runner;
  Phase1SessionRetainer session;
  std::unique_ptr<SessionProducer> producer;
  std::unique_ptr<process::RunnerProducer> process_producer;
  std::unique_ptr<RunnerFailure> runner_failure;
  RunnerObservationGate runner_gate;
  RunnerProgress runner_progress;
  std::atomic_bool runner_joined{true};
  std::atomic_bool last_stop_sealed{true};
  std::size_t other_completion_budget = 0;

  void CloseProducers() noexcept {
    // Admission closure is not runner shutdown. No force, join, or writer wait here.
    runner_progress.Close();
    runner_gate.Release();
    if (producer) producer->Stop();
  }
  bool ProducersJoined() const noexcept {
    return runner_joined.load(std::memory_order_acquire) && (!producer || producer->Joined());
  }
  std::unique_ptr<journal::SegmentJournal> journal;
  std::unique_ptr<JobOrchestrator> orchestrator;
  std::unique_ptr<http::Backend> backend;
  std::unique_ptr<http::Server> server;
  bool started = false;
};

Phase1Composition::Phase1Composition(StartupConfig config, IdentityFactory factory)
    : impl_(std::make_unique<Impl>(std::move(config), factory)) {}

Phase1Composition::~Phase1Composition() { Stop(); }

bool Phase1Composition::Start(std::string& error) {
  if (impl_->started) {
    error = "composition is already started";
    return false;
  }
  if (impl_->config.journal_path.empty()) {
    error = "Journal path is required";
    return false;
  }
  try {
    impl_->applications.clear();
    if (const auto path = impl_->config.applications_path; path.has_value()) {
      auto loaded = registry::LoadRegistry(path.value());
      if (const auto* refusal = std::get_if<registry::RegistryRefusal>(&loaded)) {
        error = refusal->reason;
        Stop();
        return false;
      }
      impl_->applications = std::get<registry::VerifiedApplications>(std::move(loaded));
    }
    if (!impl_->factory) throw std::invalid_argument("identity factory must be present");
    // Seed both generators and capture the clock origin before constructing any listener.
    impl_->identity = std::make_unique<CountingIdentitySource>(
        impl_->factory(identity::kDefaultIdentityCapacity), impl_->identity_read_count);
    impl_->clock =
        std::make_unique<CountingClock>(identity::MakeProductionClock(), impl_->clock_read_count);
    impl_->journal = std::make_unique<journal::SegmentJournal>(journal::SystemFileSystem());
    const auto opened = impl_->journal->Open(impl_->config.journal_path);
    if (!opened.ok) {
      error = opened.detail;
      Stop();
      return false;
    }
    const auto replayed =
        journal::ReplayJournal(journal::SystemFileSystem(), impl_->config.journal_path,
                               journal::ReplayOptions{impl_->config.max_jobs});
    if (replayed.status != journal::ReplayStatus::kReplayed) {
      error = replayed.detail;
      Stop();
      return false;
    }
    impl_->session.Reset();
    impl_->runner_gate.Reset(impl_->config.hold_runner_observations_for_test);
    impl_->runner_progress.Reset();
    impl_->runner = std::make_unique<process::RunnerPort>(impl_->config.max_jobs);
    impl_->runner_joined.store(false, std::memory_order_release);
    impl_->last_stop_sealed.store(false, std::memory_order_release);
    impl_->producer = std::make_unique<SessionProducer>(impl_->session);
    core::internal::Config config;
    config.hold_supervisor_for_test = impl_->config.hold_supervisor_for_test;
    config.producer_context = impl_.get();
    config.stop_producers = [](void* context) noexcept {
      static_cast<Impl*>(context)->CloseProducers();
    };
    config.join_producers = [](void* context) noexcept {
      return static_cast<Impl*>(context)->ProducersJoined();
    };
    config.max_jobs = impl_->config.max_jobs;
    config.normal_capacity = 64;
    config.trace_capacity = config.total_capacity();
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (config.max_jobs > limit - 3) throw std::invalid_argument("runner handle count overflow");
    const auto runner_handles = config.max_jobs + 3;
    if (config.total_capacity() == 0 || runner_handles > limit - config.total_capacity() ||
        runner_handles > limit - 16)
      throw std::invalid_argument("runner ingress inventory overflow");
    impl_->other_completion_budget = config.total_capacity();
    config.completion_capacity = config.total_capacity() + runner_handles;
    config.handoff_capacity = 4;
    config.ack_capacity = std::max<std::size_t>(impl_->config.max_jobs, 4);
    config.callback_registration_capacity = 16 + runner_handles;
    config.initial_journal_sequence = replayed.next_sequence;
    config.replayed_jobs = replayed.jobs;
    config.replayed_created_sequences = replayed.created_sequences;
    config.unresolved_jobs = replayed.unresolved;
    config.ports = {impl_->clock.get(), impl_->journal.get(), impl_->runner.get(), &impl_->session,
                    impl_->identity.get()};
    impl_->orchestrator = std::make_unique<JobOrchestrator>(config);
    impl_->producer->Start(*impl_->orchestrator);
    std::vector<process::LaunchSpecification> specifications;
    for (const auto& application : impl_->applications)
      specifications.emplace_back(application.application_id, application.executable,
                                  application.arguments, application.bundle_sha256);
    std::vector<std::unique_ptr<process::IngressHandle>> handles;
    handles.reserve(runner_handles);
    for (std::size_t index = 0; index != runner_handles; ++index)
      handles.push_back(std::make_unique<RunnerIngress>(*impl_->orchestrator, impl_->runner_gate,
                                                        impl_->runner_progress));
    std::vector<std::shared_ptr<process::CapacityWaiter>> waiters;
    waiters.reserve(config.max_jobs + 2);
    for (std::size_t index = 0; index != config.max_jobs + 2; ++index)
      waiters.push_back(std::make_shared<RunnerCapacity>(*impl_->orchestrator));
    impl_->runner_failure = std::make_unique<RunnerFailure>(*impl_->orchestrator);
    impl_->process_producer = std::make_unique<process::RunnerProducer>(
        *impl_->runner, std::move(specifications), std::move(handles), std::move(waiters),
        *impl_->runner_failure);
    std::vector<core::internal::JobLaunchInputs> inputs;
    inputs.reserve(impl_->applications.size());
    for (const auto& application : impl_->applications)
      inputs.emplace_back(core::StableId{application.application_id}, application.version,
                          core::Digest{application.bundle_sha256});
    impl_->backend = MakeJobBackend(*impl_->orchestrator, config.max_jobs, replayed.jobs, inputs);
    impl_->server = std::make_unique<http::Server>(impl_->config.http, *impl_->backend);
    if (!impl_->server->Start(error)) {
      Stop();
      return false;
    }
    impl_->started = true;
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    Stop();
    return false;
  } catch (...) {
    error = "composition startup failed";
    Stop();
    return false;
  }
}

void Phase1Composition::Stop() noexcept {
  if (!impl_) return;
  if (impl_->server) {
    impl_->server->Stop();
    impl_->server.reset();
  }
  core::internal::IngressResult marker;
  if (impl_->orchestrator) marker = impl_->orchestrator->SubmitShutdown();
  impl_->CloseProducers();
  if (impl_->process_producer) {
    impl_->process_producer->RequestStop();
    impl_->process_producer->Join();
  }
  if (impl_->producer) (void)impl_->producer->Join();
  impl_->runner_joined.store(true, std::memory_order_release);
  if (impl_->orchestrator) {
    // Re-drive deferred sealing after external joins, even without another FIFO/capacity event.
    impl_->orchestrator->NotifyProducerExited();
    (void)impl_->orchestrator->WaitForWriterIdle();
    if (marker.code == IngressCode::kAdmitted) {
      (void)impl_->orchestrator->AwaitCompletion(marker.ingress_sequence);
      (void)impl_->orchestrator->BeginShutdown();
    }
    impl_->last_stop_sealed.store(impl_->orchestrator->sealed(), std::memory_order_release);
    impl_->orchestrator.reset();
  }
  impl_->process_producer.reset();
  impl_->runner_failure.reset();
  impl_->producer.reset();
  impl_->runner.reset();
  impl_->backend.reset();
  impl_->journal.reset();
  impl_->clock.reset();
  impl_->identity.reset();
  impl_->applications.clear();
  impl_->started = false;
}

bool Phase1Composition::HandoffProbeSelfTestForTest() {
  if (!impl_->orchestrator) return false;
  RunnerCapacity capacity(*impl_->orchestrator);
  RunnerProgress progress;
  const auto mutex_before = process::internal::HandoffMutexEntries();
  const auto core_before = process::internal::HandoffCoreEntries();
  {
    const process::internal::HandoffProbeScope scope(true);
    progress.Reset();  // Actual owned bridge mutex, not a synthetic increment.
    capacity.Set();    // Actual bridge entry into a core capacity flag.
  }
  return process::internal::HandoffMutexEntries() == mutex_before + 1 &&
         process::internal::HandoffCoreEntries() == core_before + 1;
}
bool Phase1Composition::LatchFailureForTest() {
  return impl_->orchestrator && impl_->orchestrator->LatchReadinessFailure();
}
bool Phase1Composition::FailureSealedForTest() const noexcept {
  return impl_->orchestrator ? impl_->orchestrator->sealed() : impl_->last_stop_sealed.load();
}
bool Phase1Composition::FillOtherCompletionBudgetForTest() {
  if (!impl_->orchestrator) return false;
  const auto created = impl_->orchestrator->LastCreated();
  if (!created) return false;
  // Intentionally retain the entire pre-runner budget of completed normal inputs. They reject
  // without changing the preparing Job, free normal permits, but keep their completion slots.
  for (std::size_t index = 0; index != impl_->other_completion_budget; ++index) {
    const auto result = impl_->orchestrator->SubmitCandidate(
        {1, *created, "session_retained", R"({"session_id":")" + created->value + R"("})"});
    if (result.code != IngressCode::kAdmitted || !impl_->orchestrator->WaitForWriterIdle())
      return false;
  }
  return true;
}
bool Phase1Composition::WaitForRunnerExitForTest(std::size_t count) {
  return impl_->runner_progress.Wait(count);
}
void Phase1Composition::WaitForRunnerObservationForTest() const noexcept {
  impl_->runner_gate.Wait();
}
void Phase1Composition::ReleaseRunnerObservationForTest() noexcept { impl_->runner_gate.Release(); }
bool Phase1Composition::WaitForSupervisorIdleForTest() {
  return impl_->orchestrator && impl_->orchestrator->WaitForSupervisorIdleForTest() &&
         impl_->session.WaitIdle() && impl_->orchestrator->WaitForSupervisorIdleForTest();
}
bool Phase1Composition::ArmSupervisorBarrierForTest(std::string_view point,
                                                    std::string_view event) {
  return impl_->orchestrator && impl_->orchestrator->ArmSupervisorBarrierForTest(point, event);
}
bool Phase1Composition::WaitForSupervisorBarrierForTest() {
  return impl_->orchestrator && impl_->orchestrator->WaitForSupervisorBarrierForTest();
}
bool Phase1Composition::ReleaseSupervisorBarrierForTest() {
  return impl_->orchestrator && impl_->orchestrator->ReleaseSupervisorBarrierForTest();
}
http::Backend& Phase1Composition::backend() noexcept { return *impl_->backend; }

std::size_t Phase1Composition::clock_reads() const noexcept { return impl_->clock_read_count; }

std::size_t Phase1Composition::identity_reads() const noexcept {
  return impl_->identity_read_count;
}

std::size_t Phase1Composition::writer_turn_count() const noexcept {
  return impl_->orchestrator ? impl_->orchestrator->writer_turn_count() : 0;
}

std::uint16_t Phase1Composition::listen_port() const noexcept {
  return impl_->server ? impl_->server->port() : 0;
}

}  // namespace sitometron::daemon
