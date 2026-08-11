#include "job_orchestrator_harness.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "job_orchestrator.hpp"

namespace sitometron::test {
namespace {
using Production = core::internal::JobOrchestrator;
using PIngress = core::internal::IngressResult;
static_assert(std::is_nothrow_move_constructible_v<core::ApplicationLaunchRequest>);
static_assert(std::is_nothrow_move_constructible_v<core::ApplicationStopRequest>);
static_assert(std::is_nothrow_move_constructible_v<core::SessionRetainRequest>);
static_assert(std::is_nothrow_move_assignable_v<core::internal::TraceRecord>);
static_assert(std::is_nothrow_move_assignable_v<TraceRecord>);

IngressResult Convert(PIngress value) {
  return {static_cast<IngressCode>(value.code), value.ingress_sequence,
          value.completion_count_before};
}
Completion Convert(core::internal::Completion value) {
  return {static_cast<Completion::Code>(value.code), std::move(value.rejection)};
}
TraceRecord Convert(core::internal::TraceRecord value) {
  return {value.ordinal,
          static_cast<TraceKind>(value.kind),
          value.journal_sequence,
          value.event_type,
          value.effect_id,
          std::move(value.action),
          value.ingress_mutex_held,
          value.writer_context};
}
class ClockAdapter final : public core::ClockPort {
 public:
  ClockAdapter()
      : fake_(core::ClockReading{core::DiagnosticTimestamp{"2026-08-04T00:00:00Z"},
                                 core::MonotonicInstant{1}}) {}
  core::ClockReading Read() override {
    if (throw_on_read_) throw std::bad_alloc();
    return fake_.Read();
  }
  void ThrowOnRead() noexcept { throw_on_read_ = true; }
  bool Verify() noexcept { return !fake_.verification_failed(); }

 private:
  FakeClock fake_;
  bool throw_on_read_ = false;
};

bool EqualPayload(const core::EventPayload& lhs, const core::EventPayload& rhs) {
  if (lhs.index() != rhs.index()) return false;
  return std::visit(
      [&rhs](const auto& left) {
        using Value = std::decay_t<decltype(left)>;
        const auto* right = std::get_if<Value>(&rhs);
        if (right == nullptr) return false;
        if constexpr (std::is_same_v<Value, core::EmptyPayload>)
          return true;
        else if constexpr (std::is_same_v<Value, core::JobCreatedPayload>)
          return left.session_id == right->session_id;
        else if constexpr (std::is_same_v<Value, core::ResourcesCommittedPayload>)
          return left.allocation_id == right->allocation_id &&
                 left.allocation_digest == right->allocation_digest &&
                 left.schema_id == right->schema_id &&
                 left.schema_version == right->schema_version &&
                 left.payload_utf8 == right->payload_utf8;
        else if constexpr (std::is_same_v<Value, core::WorkerLaunchIntentPayload>)
          return left.operation_id == right->operation_id &&
                 left.application_id == right->application_id &&
                 left.application_version == right->application_version &&
                 left.bundle_sha256 == right->bundle_sha256 &&
                 left.allocation_id == right->allocation_id &&
                 left.allocation_digest == right->allocation_digest &&
                 left.worker_id == right->worker_id;
        else if constexpr (std::is_same_v<Value, core::WorkerLaunchObservedPayload>)
          return left.operation_id == right->operation_id && left.started == right->started;
        else if constexpr (std::is_same_v<Value, core::WorkerRunningPayload>)
          return left.worker_id == right->worker_id;
        else if constexpr (std::is_same_v<Value, core::PrincipalPayload>)
          return left.principal_subject == right->principal_subject;
        else if constexpr (std::is_same_v<Value, core::TimeoutExpiredPayload>)
          return left.phase == right->phase && left.timer_generation == right->timer_generation;
        else if constexpr (std::is_same_v<Value, core::WorkerEventPayload>)
          return left.worker_id == right->worker_id && left.event_sequence == right->event_sequence;
        else if constexpr (std::is_same_v<Value, core::ProcessExitConfirmedPayload>)
          return left.completion_mode == right->completion_mode &&
                 left.launch_operation_id == right->launch_operation_id;
        else if constexpr (std::is_same_v<Value, core::SessionPayload>)
          return left.session_id == right->session_id;
        else if constexpr (std::is_same_v<Value, core::TerminalOutcomePayload>)
          return left.outcome == right->outcome;
        else if constexpr (std::is_same_v<Value, core::ResourcesReleasedPayload>)
          return left.allocation_id == right->allocation_id &&
                 left.allocation_digest == right->allocation_digest;
        else if constexpr (std::is_same_v<Value, core::CleanupStatusPayload>)
          return left.status == right->status;
        else
          return left.original_event_type == right->original_event_type &&
                 left.worker_id == right->worker_id && left.event_sequence == right->event_sequence;
      },
      lhs);
}

bool EqualEvent(const core::LogicalJobEvent& lhs, const core::LogicalJobEvent& rhs) {
  return lhs.schema_version == rhs.schema_version && lhs.sequence == rhs.sequence &&
         lhs.event_type == rhs.event_type && lhs.recorded_at == rhs.recorded_at &&
         lhs.job_id == rhs.job_id && EqualPayload(lhs.payload, rhs.payload);
}

std::vector<JournalExpectation> SuccessfulJournalScript(const IdFixtures& ids) {
  const auto allocation = EmptyAllocation();
  const auto event = [&](std::uint64_t sequence, core::EventType type, core::EventPayload payload) {
    return core::LogicalJobEvent{1,
                                 sequence,
                                 type,
                                 core::DiagnosticTimestamp{"2026-08-04T00:00:00Z"},
                                 ids.primary_job,
                                 std::move(payload)};
  };
  auto script = std::vector<JournalExpectation>{
      {event(1, core::EventType::kJobCreated, core::JobCreatedPayload{ids.primary_job})},
      {event(2, core::EventType::kResourcesCommitted,
             core::ResourcesCommittedPayload{allocation.id, allocation.digest, allocation.schema_id,
                                             allocation.schema_version, allocation.payload_utf8})},
      {event(3, core::EventType::kWorkerLaunchIntent,
             core::WorkerLaunchIntentPayload{ids.launch_operation, ids.application, "1.0.0",
                                             core::Digest{std::string(64, 'a')}, allocation.id,
                                             allocation.digest, ids.worker})},
      {event(4, core::EventType::kWorkerLaunchObserved,
             core::WorkerLaunchObservedPayload{ids.launch_operation, true})},
      {event(5, core::EventType::kWorkerRunning, core::WorkerRunningPayload{ids.worker})},
      {event(6, core::EventType::kWorkerCompleted, core::WorkerEventPayload{ids.worker, 1})},
      {event(7, core::EventType::kSessionRetainRequested, core::SessionPayload{ids.primary_job})},
      {event(8, core::EventType::kSessionRetained, core::SessionPayload{ids.primary_job})},
      {event(9, core::EventType::kFinalizationCompleted, core::EmptyPayload{})},
      {event(10, core::EventType::kTerminalOutcomeCommitted,
             core::TerminalOutcomePayload{core::TerminalOutcome::kSucceeded})},
      {event(11, core::EventType::kLateWorkerEvent,
             core::LateWorkerEventPayload{core::EventType::kWorkerCompleted, ids.worker, 2})},
      {event(12, core::EventType::kProcessExitConfirmed,
             core::ProcessExitConfirmedPayload{core::CompletionMode::kCooperative,
                                               ids.launch_operation})},
      {event(13, core::EventType::kResourcesReleased,
             core::ResourcesReleasedPayload{allocation.id, allocation.digest})},
      {event(14, core::EventType::kCleanupStatusRecorded,
             core::CleanupStatusPayload{core::CleanupStatus::kCompleted})},
  };
  for (auto& expectation : script) expectation.result = core::LogicalCommitResult::kCommitted;
  return script;
}

class JournalAdapter final : public core::JobJournalPort {
 public:
  JournalAdapter(core::LogicalCommitResult result, std::size_t capacity, const IdFixtures& ids)
      : result_(result),
        capacity_(capacity),
        expected_(SuccessfulJournalScript(ids)),
        journal_(expected_) {
    bounded_.reserve(capacity_);
  }
  core::LogicalCommitResult Commit(const core::LogicalJobEvent& event) noexcept override {
    std::lock_guard lock(mutex_);
    if (!fallback_ && result_ == core::LogicalCommitResult::kCommitted &&
        real_next_ < expected_.size() && EqualEvent(event, expected_[real_next_].event)) {
      const auto result = journal_.Commit(event);
      if (result != core::LogicalCommitResult::kOutcomeUnknown) {
        ++real_next_;
        return result;
      }
      fallback_ = true;
    } else {
      fallback_ = true;
    }
    try {
      if (bounded_.size() == capacity_) {
        verification_failed_ = true;
        return core::LogicalCommitResult::kOutcomeUnknown;
      }
      bounded_.push_back(event);
      return result_;
    } catch (...) {
      verification_failed_ = true;
      return core::LogicalCommitResult::kOutcomeUnknown;
    }
  }
  void SetResult(core::LogicalCommitResult result) noexcept {
    std::lock_guard lock(mutex_);
    result_ = result;
  }
  std::vector<core::LogicalJobEvent> observations() const {
    std::lock_guard lock(mutex_);
    auto result = journal_.CopyObservations();
    result.insert(result.end(), bounded_.begin(), bounded_.end());
    return result;
  }
  std::size_t count() const noexcept {
    std::lock_guard lock(mutex_);
    return real_next_ + bounded_.size();
  }
  bool Verify() noexcept {
    std::lock_guard lock(mutex_);
    if (!fallback_) {
      if (real_next_ == expected_.size()) return journal_.Verify() && !verification_failed_;
      return !journal_.verification_failed() && !verification_failed_;
    }
    return !journal_.verification_failed() && !verification_failed_;
  }

 private:
  core::LogicalCommitResult result_;
  std::size_t capacity_;
  std::vector<JournalExpectation> expected_;
  FakeJobJournal journal_;
  std::vector<core::LogicalJobEvent> bounded_;
  std::size_t real_next_ = 0;
  bool fallback_ = false;
  bool verification_failed_ = false;
  mutable std::mutex mutex_;
};

struct RunnerSlot {
  RunnerCallKind kind;
  core::Uuid job;
  std::unique_ptr<FakeApplicationRunner> fake;
  bool called = false;
  bool candidate_pending = false;
};

class RunnerAdapter final : public core::ApplicationRunnerPort {
 public:
  explicit RunnerAdapter(std::size_t capacity) : capacity_(capacity) {
    const auto ids = Ids();
    const auto allocation = EmptyAllocation();
    for (const auto& pair :
         std::array{std::pair{RunnerCallKind::kLaunch, ids.primary_job},
                    std::pair{RunnerCallKind::kLaunch, ids.secondary_job},
                    std::pair{RunnerCallKind::kCooperativeStop, ids.primary_job},
                    std::pair{RunnerCallKind::kCooperativeStop, ids.secondary_job},
                    std::pair{RunnerCallKind::kForcedStop, ids.primary_job},
                    std::pair{RunnerCallKind::kForcedStop, ids.secondary_job}}) {
      const auto worker = pair.second == ids.primary_job ? ids.worker : ids.secondary_worker;
      const auto operation =
          pair.second == ids.primary_job ? ids.launch_operation : ids.secondary_launch_operation;
      if (pair.first == RunnerCallKind::kLaunch) {
        core::ApplicationLaunchRequest request{
            pair.second, core::WorkerLaunchIntentPayload{operation, ids.application, "1.0.0",
                                                         core::Digest{std::string(64, 'a')},
                                                         allocation.id, allocation.digest, worker}};
        slots_.push_back(
            {pair.first, pair.second,
             std::make_unique<FakeApplicationRunner>(
                 std::vector<RunnerExpectation>{ExpectLaunch(std::move(request), true)}, capacity_),
             false, true});
      } else {
        core::ApplicationStopRequest request{pair.second, operation, worker};
        auto expectation = pair.first == RunnerCallKind::kCooperativeStop
                               ? ExpectCooperativeStop(std::move(request))
                               : ExpectForcedStop(std::move(request));
        slots_.push_back({pair.first, pair.second,
                          std::make_unique<FakeApplicationRunner>(
                              std::vector<RunnerExpectation>{std::move(expectation)}, capacity_),
                          false, true});
      }
    }
    call_order_.fill(0);
  }
  void HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept override {
    Handoff(RunnerCallKind::kLaunch, request.job_id,
            [&request](FakeApplicationRunner& fake) { fake.HandoffLaunch(std::move(request)); });
  }
  void HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept override {
    Handoff(RunnerCallKind::kCooperativeStop, request.job_id,
            [&request](FakeApplicationRunner& fake) {
              fake.HandoffCooperativeStop(std::move(request));
            });
  }
  void HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept override {
    Handoff(RunnerCallKind::kForcedStop, request.job_id, [&request](FakeApplicationRunner& fake) {
      fake.HandoffForcedStop(std::move(request));
    });
  }
  std::vector<RunnerObservation> observations() const {
    std::vector<RunnerObservation> result;
    for (std::size_t index = 0; index < call_count_; ++index) {
      const auto calls = slots_[call_order_[index]].fake->CopyObservations();
      result.insert(result.end(), calls.begin(), calls.end());
    }
    return result;
  }
  std::optional<core::RawCandidateEvent> Take() {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (!slots_[index].called || !slots_[index].candidate_pending) continue;
      if (auto candidate = slots_[index].fake->TakeNextCandidate()) {
        slots_[index].candidate_pending = false;
        return candidate;
      }
    }
    return std::nullopt;
  }
  bool Cancel() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (!slots_[index].called || !slots_[index].candidate_pending) continue;
      if (slots_[index].fake->CancelNextCandidate()) {
        slots_[index].candidate_pending = false;
        return true;
      }
    }
    return false;
  }
  bool Verify() noexcept {
    if (verification_failed_) return false;
    for (const auto& slot : slots_)
      if (slot.called && !slot.fake->Verify()) return false;
    return true;
  }

 private:
  template <typename Fn>
  void Handoff(RunnerCallKind kind, const core::Uuid& job, Fn&& handoff) noexcept {
    for (auto& slot : slots_) {
      if (!slot.called && slot.kind == kind && slot.job == job) {
        slot.called = true;
        if (call_count_ < call_order_.size()) call_order_[call_count_++] = &slot - slots_.data();
        handoff(*slot.fake);
        return;
      }
    }
    verification_failed_ = true;
  }
  std::size_t capacity_;
  std::vector<RunnerSlot> slots_;
  std::array<std::size_t, 6> call_order_{};
  std::size_t call_count_ = 0;
  bool verification_failed_ = false;
};

struct SessionSlot {
  core::Uuid job;
  std::unique_ptr<FakeSessionRetainer> fake;
  bool called = false;
  bool candidate_pending = false;
};

class SessionAdapter final : public core::SessionRetainerPort {
 public:
  explicit SessionAdapter(std::size_t capacity) : capacity_(capacity) {
    const auto ids = Ids();
    for (const auto& job : std::array{ids.primary_job, ids.secondary_job}) {
      slots_.push_back({job,
                        std::make_unique<FakeSessionRetainer>(
                            std::vector<SessionExpectation>{
                                ExpectSessionRetain(core::SessionRetainRequest{job, job})},
                            capacity_),
                        false, true});
    }
  }
  void HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept override {
    for (auto& slot : slots_) {
      if (!slot.called && slot.job == request.job_id && slot.job == request.session_id) {
        slot.called = true;
        if (call_count_ < call_order_.size()) call_order_[call_count_++] = &slot - slots_.data();
        slot.fake->HandoffRetainSameIdentity(std::move(request));
        return;
      }
    }
    verification_failed_ = true;
  }
  std::vector<core::SessionRetainRequest> observations() const {
    std::vector<core::SessionRetainRequest> result;
    for (std::size_t index = 0; index < call_count_; ++index) {
      const auto calls = slots_[call_order_[index]].fake->CopyObservations();
      result.insert(result.end(), calls.begin(), calls.end());
    }
    return result;
  }
  std::optional<core::RawCandidateEvent> Take() {
    for (std::size_t index = 0; index < slots_.size(); ++index)
      if (slots_[index].called && slots_[index].candidate_pending)
        if (auto candidate = slots_[index].fake->TakeNextCandidate()) {
          slots_[index].candidate_pending = false;
          return candidate;
        }
    return std::nullopt;
  }
  bool Cancel() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index)
      if (slots_[index].called && slots_[index].candidate_pending &&
          slots_[index].fake->CancelNextCandidate()) {
        slots_[index].candidate_pending = false;
        return true;
      }
    return false;
  }
  bool Verify() noexcept {
    if (verification_failed_) return false;
    for (const auto& slot : slots_)
      if (slot.called && !slot.fake->Verify()) return false;
    return true;
  }

 private:
  std::size_t capacity_;
  std::vector<SessionSlot> slots_;
  std::array<std::size_t, 2> call_order_{};
  std::size_t call_count_ = 0;
  bool verification_failed_ = false;
};

class IdentityAdapter final : public core::IdentitySourcePort {
 public:
  explicit IdentityAdapter(const IdFixtures& ids)
      : ids_(ids),
        fake_({core::GeneratedJobSessionIdentity{ids.primary_job},
               core::GeneratedJobSessionIdentity{ids.secondary_job}},
              {core::GeneratedWorkerIdentity{ids.worker},
               core::GeneratedWorkerIdentity{ids.secondary_worker}},
              {core::GeneratedLaunchOperationIdentity{ids.launch_operation},
               core::GeneratedLaunchOperationIdentity{ids.secondary_launch_operation}}) {}
  core::JobSessionIdentityResult GenerateJobSessionIdentity() override {
    return fake_.GenerateJobSessionIdentity();
  }
  core::WorkerIdentityResult GenerateWorkerIdentity() override {
    return fake_.GenerateWorkerIdentity();
  }
  core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    return fake_.GenerateLaunchOperationIdentity();
  }
  bool Verify() noexcept { return !fake_.verification_failed(); }

 private:
  IdFixtures ids_;
  FakeIdentitySource fake_;
};

struct PortBundle {
  ClockAdapter clock;
  JournalAdapter journal;
  RunnerAdapter runner;
  SessionAdapter session;
  IdentityAdapter identity;
  explicit PortBundle(const Config& value)
      : clock(),
        journal(value.commit_result, value.trace_capacity, Ids()),
        runner(value.handoff_capacity),
        session(value.handoff_capacity),
        identity(Ids()) {}
};

void SetJournalResult(void* context, core::LogicalCommitResult result) noexcept {
  static_cast<PortBundle*>(context)->journal.SetResult(result);
}
core::internal::Config Convert(const Config& value, PortBundle& ports) {
  core::internal::Config result{value.max_jobs,
                                value.normal_capacity,
                                value.trace_capacity,
                                value.completion_capacity,
                                value.handoff_capacity,
                                value.ack_capacity,
                                value.callback_registration_capacity,
                                value.initial_ingress_sequence,
                                value.initial_journal_sequence,
                                value.commit_result};
  result.ports = {&ports.clock,    &ports.journal, &ports.runner,    &ports.session,
                  &ports.identity, &ports,         &SetJournalResult};
  return result;
}
}  // namespace

struct JobOrchestratorHarness::Impl {
  explicit Impl(const Config& value)
      : config(value), ports(value), orchestrator(Convert(value, ports)) {}
  Config config;
  PortBundle ports;
  Production orchestrator;
  core::internal::CallbackHandle held_callback;
};

IngressResult CallbackEndpoint::Invoke(const core::RawCandidateEvent& event) {
  return Convert(handle_.Invoke(event));
}
IngressResult CallbackEndpoint::Invoke(core::RawCandidateEvent&& event) noexcept {
  return Convert(handle_.Invoke(std::move(event)));
}
bool CallbackEndpoint::HoldLease() { return handle_.HoldLease(); }
bool CallbackEndpoint::ReleaseLease() { return handle_.ReleaseLease(); }

JobOrchestratorHarness::JobOrchestratorHarness(Config config)
    : impl_(std::make_unique<Impl>(config)) {}
JobOrchestratorHarness::~JobOrchestratorHarness() = default;

IngressResult JobOrchestratorHarness::Create() { return Convert(impl_->orchestrator.Create()); }
std::optional<GeneratedIdentities> JobOrchestratorHarness::generated_identities() const {
  const auto job = impl_->orchestrator.LastCreated();
  if (!job) return std::nullopt;
  const auto worker = impl_->orchestrator.GeneratedWorker(*job);
  const auto launch = impl_->orchestrator.GeneratedLaunch(*job);
  if (!worker || !launch) return std::nullopt;
  return GeneratedIdentities{*job, *job, *worker, *launch};
}
IngressResult JobOrchestratorHarness::SubmitGeneratedLaunchIntent(const core::Uuid& job,
                                                                  const AllocationFixture& a) {
  const auto worker = impl_->orchestrator.GeneratedWorker(job);
  const auto operation = impl_->orchestrator.GeneratedLaunch(job);
  if (!worker || !operation)
    return {IngressCode::kServiceFailed, 0, impl_->orchestrator.completion_count()};
  auto ids = Ids();
  ids.worker = *worker;
  ids.launch_operation = *operation;
  return SubmitLaunchIntent(MakeLaunchIntent(job, a, ids));
}
IngressResult JobOrchestratorHarness::SubmitGeneratedLaunchIntent(const AllocationFixture& a) {
  const auto generated = impl_->orchestrator.LastCreated();
  if (!generated) return {IngressCode::kServiceFailed, 0, impl_->orchestrator.completion_count()};
  return SubmitGeneratedLaunchIntent(*generated, a);
}
#define CANDIDATE_METHOD(name)                                                   \
  IngressResult JobOrchestratorHarness::name(const core::RawCandidateEvent& e) { \
    return Convert(impl_->orchestrator.SubmitCandidate(e));                      \
  }
IngressResult JobOrchestratorHarness::SubmitJobCreated(const core::RawCandidateEvent& e) {
  return Convert(impl_->orchestrator.SubmitCandidate(e));
}
CANDIDATE_METHOD(SubmitResourcesCommitted)
CANDIDATE_METHOD(SubmitLaunchIntent)
CANDIDATE_METHOD(SubmitLaunchObserved)
CANDIDATE_METHOD(SubmitWorkerRunning)
CANDIDATE_METHOD(SubmitSessionRetainRequested)
CANDIDATE_METHOD(SubmitSessionRetained)
CANDIDATE_METHOD(SubmitFinalizationCompleted)
CANDIDATE_METHOD(SubmitFinalizationFailed)
CANDIDATE_METHOD(SubmitTerminalOutcome)
#undef CANDIDATE_METHOD
IngressResult JobOrchestratorHarness::SubmitCancel(const core::Command& c) {
  return Convert(impl_->orchestrator.SubmitCommand(c));
}
IngressResult JobOrchestratorHarness::SubmitTerminate(const core::Command& c) {
  return Convert(impl_->orchestrator.SubmitCommand(c));
}
IngressResult JobOrchestratorHarness::SubmitWorker(const core::RawCandidateEvent& e) {
  return Convert(impl_->orchestrator.SubmitWorker(e));
}
TimerSubmitResult JobOrchestratorHarness::SubmitTimeout(const core::TimerNotification& n) {
  const auto result = impl_->orchestrator.SubmitTimeout(n);
  return {result.discarded, Convert(result.admitted)};
}
IngressResult JobOrchestratorHarness::SubmitProcessExit(const core::RawCandidateEvent& e) {
  return Convert(impl_->orchestrator.SubmitCandidate(e));
}
IngressResult JobOrchestratorHarness::SubmitResourcesReleased(const core::RawCandidateEvent& e) {
  return Convert(impl_->orchestrator.SubmitCandidate(e));
}
IngressResult JobOrchestratorHarness::SubmitCleanup(const core::RawCandidateEvent& e) {
  return Convert(impl_->orchestrator.SubmitCandidate(e));
}
IngressResult JobOrchestratorHarness::SubmitConflictingResources(const core::Uuid& id) {
  // Use a genuine reducer binding conflict rather than a payload-text test hook:
  // launch intent names an allocation different from the committed snapshot.
  auto a = EmptyAllocation();
  a.id.value = "other-allocation";
  return SubmitLaunchIntent(MakeLaunchIntent(id, a, Ids()));
}
IngressResult JobOrchestratorHarness::SubmitShutdown() {
  return Convert(impl_->orchestrator.SubmitShutdown());
}
bool JobOrchestratorHarness::RetainTimerLease(const core::Uuid& id, core::TimeoutPhase p) {
  return impl_->orchestrator.RetainTimerLease(id, p);
}
bool JobOrchestratorHarness::RetainWorkerLease(const core::Uuid& id) {
  return impl_->orchestrator.RetainWorkerLease(id);
}
bool JobOrchestratorHarness::RetainProcessExitLease(const core::Uuid& id) {
  return impl_->orchestrator.RetainProcessExitLease(id);
}
bool JobOrchestratorHarness::RetainResourcesReleasedLease(const core::Uuid& id) {
  return impl_->orchestrator.RetainResourcesReleasedLease(id);
}
bool JobOrchestratorHarness::RetainCleanupLease(const core::Uuid& id) {
  return impl_->orchestrator.RetainCleanupLease(id);
}
bool JobOrchestratorHarness::RetireWorkerAck(const core::RawCandidateEvent& event) {
  return impl_->orchestrator.RetireWorkerAck(event);
}
bool JobOrchestratorHarness::RetireWorkerAck(const core::Uuid& id, std::uint64_t s) {
  const auto worker = impl_->orchestrator.GeneratedWorker(id);
  if (!worker) return false;
  return RetireWorkerAck(MakeWorkerCompleted(id, *worker, s));
}
bool JobOrchestratorHarness::LatchReadinessFailure() {
  return impl_->orchestrator.LatchReadinessFailure();
}
bool JobOrchestratorHarness::WaitForWriterIdle() { return impl_->orchestrator.WaitForWriterIdle(); }
bool JobOrchestratorHarness::BeginShutdown() { return impl_->orchestrator.BeginShutdown(); }
bool JobOrchestratorHarness::ArmPause(WriterPhase p) {
  return impl_->orchestrator.ArmPause(static_cast<core::internal::WriterPhase>(p));
}
bool JobOrchestratorHarness::ArmBarrier(WriterPhase p) {
  return impl_->orchestrator.ArmBarrier(static_cast<core::internal::WriterPhase>(p));
}
bool JobOrchestratorHarness::ArmAdmissionPause() { return impl_->orchestrator.ArmAdmissionPause(); }
bool JobOrchestratorHarness::WaitForAdmissionPause() {
  return impl_->orchestrator.WaitForAdmissionPause();
}
std::size_t JobOrchestratorHarness::admission_attempt_count() const noexcept {
  return impl_->orchestrator.admission_attempt_count();
}
bool JobOrchestratorHarness::WaitForAdmissionAttempts(std::size_t c) {
  return impl_->orchestrator.WaitForAdmissionAttempts(c);
}
bool JobOrchestratorHarness::WaitForWaitUntilAttempts(std::size_t c) {
  return impl_->orchestrator.WaitForWaitUntilAttempts(c);
}
bool JobOrchestratorHarness::ReleaseAdmissionPause() {
  return impl_->orchestrator.ReleaseAdmissionPause();
}
bool JobOrchestratorHarness::WaitUntil(std::uint64_t s, WriterPhase p) {
  return impl_->orchestrator.WaitUntil(s, static_cast<core::internal::WriterPhase>(p));
}
bool JobOrchestratorHarness::Release(std::uint64_t s, WriterPhase p) {
  return impl_->orchestrator.Release(s, static_cast<core::internal::WriterPhase>(p));
}
CallbackEndpoint JobOrchestratorHarness::RetainedCallback() {
  return CallbackEndpoint(impl_->orchestrator.RetainedCallback());
}
bool JobOrchestratorHarness::HoldCallbackLease() {
  impl_->held_callback = impl_->orchestrator.RetainedCallback();
  return impl_->held_callback.HoldLease();
}
bool JobOrchestratorHarness::CloseCallbackLease() {
  const auto r = impl_->held_callback.ReleaseLease();
  (void)impl_->orchestrator.BeginShutdown();
  return r;
}
bool JobOrchestratorHarness::ConcurrentCallbackInvocation() {
  // Hold the first invocation at actual production admission; the concurrent
  // second invocation must observe the same registration active and fail closed.
  auto endpoint = RetainedCallback();
  if (!impl_->orchestrator.ArmAdmissionPause()) return false;
  IngressResult first{};
  const auto event = MakeWorkerCompleted(Ids().primary_job, Ids().worker, 1);
  std::thread producer([&] { first = endpoint.Invoke(event); });
  const bool paused = impl_->orchestrator.WaitForAdmissionPause();
  const auto second = endpoint.Invoke(event);
  const bool released = impl_->orchestrator.ReleaseAdmissionPause();
  producer.join();
  return paused && released &&
         (first.code == IngressCode::kAdmitted || first.code == IngressCode::kServiceFailed) &&
         second.code == IngressCode::kServiceFailed && impl_->orchestrator.failed();
}
bool JobOrchestratorHarness::VerifyFakes() {
  return impl_->ports.clock.Verify() && impl_->ports.journal.Verify() &&
         impl_->ports.runner.Verify() && impl_->ports.session.Verify() &&
         impl_->ports.identity.Verify();
}
bool JobOrchestratorHarness::failed() const noexcept { return impl_->orchestrator.failed(); }
bool JobOrchestratorHarness::sealed() const noexcept { return impl_->orchestrator.sealed(); }
bool JobOrchestratorHarness::stopped() const noexcept { return impl_->orchestrator.stopped(); }
std::size_t JobOrchestratorHarness::journal_attempts() const noexcept {
  return impl_->ports.journal.count();
}
std::size_t JobOrchestratorHarness::apply_count() const noexcept {
  return impl_->orchestrator.apply_count();
}
std::size_t JobOrchestratorHarness::writer_turn_count() const noexcept {
  return impl_->orchestrator.writer_turn_count();
}
std::size_t JobOrchestratorHarness::max_concurrent_writer_turns() const noexcept {
  return impl_->orchestrator.max_concurrent_writer_turns();
}
std::size_t JobOrchestratorHarness::disposed_count() const noexcept {
  return impl_->orchestrator.disposed_count();
}
std::size_t JobOrchestratorHarness::completion_count() const noexcept {
  return impl_->orchestrator.completion_count();
}
std::size_t JobOrchestratorHarness::ack_authorization_count() const noexcept {
  return impl_->orchestrator.ack_authorization_count();
}
std::size_t JobOrchestratorHarness::effect_count() const noexcept {
  return impl_->orchestrator.effect_count();
}
std::size_t JobOrchestratorHarness::response_count() const noexcept {
  return impl_->orchestrator.response_count();
}
std::size_t JobOrchestratorHarness::resident_count() const noexcept {
  return impl_->orchestrator.resident_count();
}
std::size_t JobOrchestratorHarness::live_critical_permit_count() const noexcept {
  return impl_->orchestrator.live_critical_permit_count();
}
std::size_t JobOrchestratorHarness::critical_occupancy() const noexcept {
  return impl_->orchestrator.critical_occupancy();
}
std::size_t JobOrchestratorHarness::normal_occupancy() const noexcept {
  return impl_->orchestrator.normal_occupancy();
}
std::size_t JobOrchestratorHarness::total_occupancy() const noexcept {
  return impl_->orchestrator.total_occupancy();
}
std::optional<Completion> JobOrchestratorHarness::TakeCompletion(std::uint64_t s) {
  auto r = impl_->orchestrator.TakeCompletion(s);
  if (!r) return {};
  return Convert(std::move(*r));
}
std::optional<core::Snapshot> JobOrchestratorHarness::Snapshot(const core::Uuid& id) const {
  return impl_->orchestrator.SnapshotFor(id);
}
std::vector<TraceRecord> JobOrchestratorHarness::CopyTrace() const {
  std::vector<TraceRecord> out;
  for (auto& r : impl_->orchestrator.CopyTrace()) out.push_back(Convert(std::move(r)));
  return out;
}
std::vector<core::LogicalJobEvent> JobOrchestratorHarness::CopyJournalAttempts() const {
  return impl_->ports.journal.observations();
}
std::vector<std::uint64_t> JobOrchestratorHarness::CopyIngressSequences() const {
  return impl_->orchestrator.CopyIngressSequences();
}
std::vector<core::ApplicationLaunchRequest> JobOrchestratorHarness::CopyLaunchRequests() const {
  std::vector<core::ApplicationLaunchRequest> result;
  for (const auto& observation : impl_->ports.runner.observations()) {
    if (const auto* request = std::get_if<core::ApplicationLaunchRequest>(&observation.request))
      result.push_back(*request);
  }
  return result;
}
std::vector<core::SessionRetainRequest> JobOrchestratorHarness::CopySessionRequests() const {
  return impl_->ports.session.observations();
}
std::optional<core::RawCandidateEvent> JobOrchestratorHarness::TakeRunnerCandidate() {
  return impl_->ports.runner.Take();
}
std::optional<core::RawCandidateEvent> JobOrchestratorHarness::TakeSessionCandidate() {
  return impl_->ports.session.Take();
}
bool JobOrchestratorHarness::CancelRunnerCandidate() { return impl_->ports.runner.Cancel(); }
bool JobOrchestratorHarness::CancelSessionCandidate() { return impl_->ports.session.Cancel(); }
std::size_t JobOrchestratorHarness::creation_claim_count(const core::Uuid& id) const noexcept {
  return impl_->orchestrator.creation_claim_count(id);
}
bool JobOrchestratorHarness::NoForbiddenPostcommitActions() const noexcept {
  return impl_->orchestrator.NoForbiddenPostcommitActions();
}
bool JobOrchestratorHarness::NoPostcommitAllocationOrCopy() const noexcept {
  return impl_->orchestrator.NoPostcommitAllocationOrCopy();
}
bool JobOrchestratorHarness::DestinationCapacityCheckedBeforeCommit() const noexcept {
  return impl_->orchestrator.DestinationCapacityCheckedBeforeCommit();
}
void JobOrchestratorHarness::InjectPrecommitMaterializationFailure() {
  impl_->orchestrator.InjectPrecommitMaterializationFailure();
}
void JobOrchestratorHarness::InjectClockReadFailure() noexcept { impl_->ports.clock.ThrowOnRead(); }
void JobOrchestratorHarness::InjectAccountingCorruption() {
  impl_->orchestrator.InjectAccountingCorruption();
}
void JobOrchestratorHarness::InjectResidualCriticalPermit() {
  impl_->orchestrator.InjectResidualCriticalPermit();
}
void JobOrchestratorHarness::SetNextCommitResult(core::LogicalCommitResult r) {
  impl_->orchestrator.SetNextCommitResult(r);
}
void JobOrchestratorHarness::SetDestinationCapacity(std::size_t n) {
  impl_->orchestrator.SetDestinationCapacity(n);
}

}  // namespace sitometron::test
