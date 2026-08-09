#ifndef SITOMETRON_TESTS_SUPPORT_JOB_ORCHESTRATOR_HARNESS_HPP_
#define SITOMETRON_TESTS_SUPPORT_JOB_ORCHESTRATOR_HARNESS_HPP_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core_fakes.hpp"
#include "job_orchestrator.hpp"
#include "sitometron/core/job_ports.hpp"

namespace sitometron::test {

enum class IngressCode {
  kAdmitted,
  kCoalescedPending,
  kAlreadyPending,
  kNormalFull,
  kResidentLimit,
  kAdmissionClosed,
  kServiceFailed,
};

struct IngressResult {
  IngressCode code = IngressCode::kServiceFailed;
  std::uint64_t ingress_sequence = 0;
  // Captured by admission before the submission is linearized, for race-free completion deltas.
  std::size_t completion_count_before = 0;
  friend bool operator==(const IngressResult&, const IngressResult&) = default;
};

enum class WriterPhase {
  kBeforeDequeue,
  kAfterDequeueAuthorized,
  kAfterDecision,
  kBeforeCommit,
  kAfterCommit,
  kAfterApply,
  kAfterEffects,
  kResponseReleased,
  kTurnFinished,
  kFailureDisposed,
  kShutdownMarker,
  kSealed,
};

enum class TraceKind {
  kJournalAttempt,
  kJournalCommitted,
  kSnapshotActivated,
  kEffect,
  kTimerAction,
  kCapabilityHandoff,
  kAckAuthorized,
  kTerminalPublished,
  kSafetyAction,
  kSourceRegistered,
  kResponseReleased,
};

struct TraceRecord {
  std::uint64_t ordinal = 0;
  TraceKind kind = TraceKind::kJournalAttempt;
  std::uint64_t journal_sequence = 0;
  core::EventType event_type = core::EventType::kInvalid;
  core::EffectId effect_id = core::EffectId::kInvalid;
  std::string action;
  bool ingress_mutex_held = false;
  bool writer_context = false;
  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;
};

struct TimerSubmitResult {
  bool discarded = false;
  IngressResult admitted{};
};

struct Completion {
  // A completion is a finite terminal disposition only.  The prospective ApplyResult
  // stays exclusively in the writer-owned resident bank after logical commit.
  enum class Code { kSuccess, kReducerRejection, kServiceFailed };
  Code code = Code::kServiceFailed;
  std::optional<core::Rejection> rejection;
};

struct Config {
  std::size_t max_jobs = 0;
  std::size_t normal_capacity = 0;
  std::size_t trace_capacity = 0;
  std::size_t completion_capacity = 0;
  std::size_t handoff_capacity = 0;
  std::size_t ack_capacity = 0;
  std::size_t callback_registration_capacity = 0;
  std::uint64_t initial_ingress_sequence = 1;
  std::uint64_t initial_journal_sequence = 1;
  core::LogicalCommitResult commit_result = core::LogicalCommitResult::kCommitted;

  [[nodiscard]] constexpr std::size_t critical_reserve() const noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    return max_jobs > (maximum - 1U) / 9U ? 0U : 9U * max_jobs + 1U;
  }
  [[nodiscard]] constexpr std::size_t total_capacity() const noexcept {
    const auto reserve = critical_reserve();
    return reserve > std::numeric_limits<std::size_t>::max() - normal_capacity
               ? 0U
               : normal_capacity + reserve;
  }
};

constexpr Config PositiveConfig() {
  return Config{2, 2, 512, 64, 64, 32, 16, 1, 1, core::LogicalCommitResult::kCommitted};
}
static_assert(PositiveConfig().critical_reserve() == 19);
static_assert(PositiveConfig().total_capacity() == 21);

struct AllocationFixture {
  core::StableId id{"allocation-1"};
  core::Digest digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
  core::StableId schema_id{"allocation.v1"};
  std::uint32_t schema_version = 1;
  std::string payload_utf8{"{}"};
};

struct IdFixtures {
  core::Uuid primary_job{"01890f30-7b54-7cc3-98c4-dc0c0c07398f"};
  core::Uuid secondary_job{"01890f30-7b54-7cc3-98c4-dc0c0c073990"};
  core::Uuid worker{"550e8400-e29b-41d4-a716-446655440000"};
  core::Uuid secondary_worker{"550e8400-e29b-41d4-a716-446655440001"};
  core::StableId launch_operation{"launch-op-1"};
  core::StableId secondary_launch_operation{"launch-op-2"};
  core::StableId application{"application-1"};
};

inline IdFixtures Ids() { return {}; }
inline AllocationFixture EmptyAllocation() { return {}; }

inline core::RawCandidateEvent MakeJobCreated(const core::Uuid& job) {
  return {1, job, "job_created", "{\"session_id\":\"" + job.value + "\"}"};
}
inline core::RawCandidateEvent MakeResourcesCommitted(const core::Uuid& job,
                                                      const AllocationFixture& allocation) {
  return {1, job, "resources_committed",
          "{\"allocation_id\":\"" + allocation.id.value + "\",\"allocation_digest\":\"" +
              allocation.digest.value + "\",\"resolved_allocation\":{\"schema_id\":\"" +
              allocation.schema_id.value + "\",\"schema_version\":1,\"payload_utf8\":\"{}\"}}"};
}
inline core::RawCandidateEvent MakeLaunchIntent(const core::Uuid& job,
                                                const AllocationFixture& allocation,
                                                const IdFixtures& ids) {
  return {1, job, "worker_launch_intent",
          "{\"operation_id\":\"" + ids.launch_operation.value +
              "\",\"application\":{\"application_id\":\"" + ids.application.value +
              "\",\"version\":\"1.0.0\",\"bundle_sha256\":\"" + std::string(64, 'a') +
              "\"},\"allocation_id\":\"" + allocation.id.value + "\",\"allocation_digest\":\"" +
              allocation.digest.value + "\",\"worker_id\":\"" + ids.worker.value + "\"}"};
}
inline core::RawCandidateEvent MakeWorkerRunning(const core::Uuid& job, const core::Uuid& worker) {
  return {1, job, "worker_running", "{\"worker_id\":\"" + worker.value + "\"}"};
}
inline core::RawCandidateEvent MakeWorkerCompleted(const core::Uuid& job, const core::Uuid& worker,
                                                   std::uint64_t sequence) {
  return {1, job, "worker_completed",
          "{\"worker_id\":\"" + worker.value + "\",\"event_sequence\":" + std::to_string(sequence) +
              "}"};
}
inline core::RawCandidateEvent MakeWorkerFailed(const core::Uuid& job, const core::Uuid& worker,
                                                std::uint64_t sequence) {
  return {1, job, "worker_failed",
          "{\"worker_id\":\"" + worker.value + "\",\"event_sequence\":" + std::to_string(sequence) +
              "}"};
}
inline core::RawCandidateEvent MakeSessionRetainRequested(const core::Uuid& job) {
  return {1, job, "session_retain_requested", "{\"session_id\":\"" + job.value + "\"}"};
}
inline core::RawCandidateEvent MakeSessionRetained(const core::Uuid& job) {
  return {1, job, "session_retained", "{\"session_id\":\"" + job.value + "\"}"};
}
inline core::RawCandidateEvent MakeFinalizationCompleted(const core::Uuid& job) {
  return {1, job, "finalization_completed", "{}"};
}
inline core::RawCandidateEvent MakeTerminalOutcome(const core::Uuid& job,
                                                   core::TerminalOutcome outcome) {
  const std::string_view value = outcome == core::TerminalOutcome::kSucceeded    ? "succeeded"
                                 : outcome == core::TerminalOutcome::kFailed     ? "failed"
                                 : outcome == core::TerminalOutcome::kCancelled  ? "cancelled"
                                 : outcome == core::TerminalOutcome::kTerminated ? "terminated"
                                                                                 : "timed_out";
  return {1, job, "terminal_outcome_committed", "{\"outcome\":\"" + std::string(value) + "\"}"};
}
inline core::RawCandidateEvent MakeProcessExit(const core::Uuid& job, const IdFixtures& ids,
                                               core::CompletionMode mode) {
  const std::string value = mode == core::CompletionMode::kCooperative ? "cooperative" : "forced";
  return {1, job, "process_exit_confirmed",
          "{\"completion_mode\":\"" + value + "\",\"launch_operation_id\":\"" +
              ids.launch_operation.value + "\"}"};
}
inline core::RawCandidateEvent MakeResourcesReleased(const core::Uuid& job,
                                                     const AllocationFixture& allocation) {
  return {1, job, "resources_released",
          "{\"allocation_id\":\"" + allocation.id.value + "\",\"allocation_digest\":\"" +
              allocation.digest.value + "\"}"};
}
inline core::RawCandidateEvent MakeCleanupCompleted(const core::Uuid& job) {
  return {1, job, "cleanup_status_recorded", "{\"status\":\"completed\"}"};
}
inline core::RawCandidateEvent MakeTimeout(const core::Uuid& job, core::TimeoutPhase phase,
                                           std::uint64_t generation) {
  constexpr std::string_view names[] = {"preparation", "execution", "cooperative_stop",
                                        "process_exit_confirmation"};
  return {1, job, "timeout_expired",
          "{\"phase\":\"" + std::string(names[static_cast<std::size_t>(phase)]) +
              "\",\"timer_generation\":" + std::to_string(generation) + "}"};
}

struct ExpectedEnvelope {
  std::uint32_t schema_version = 1;
  std::uint64_t sequence = 0;
  core::EventType event_type = core::EventType::kInvalid;
  std::string recorded_at;
  core::Uuid job_id;
  core::EventPayload payload;
};

struct ExpectedTrace {
  std::vector<TraceRecord> records;

  void Add(TraceKind kind, std::uint64_t sequence, core::EventType event,
           core::EffectId effect = core::EffectId::kInvalid, std::string action = {}) {
    records.push_back({static_cast<std::uint64_t>(records.size() + 1), kind, sequence, event,
                       effect, std::move(action), false, true});
  }

  void Turn(std::uint64_t sequence, core::EventType event,
            const std::vector<std::pair<core::EffectId, std::string>>& effects,
            const std::vector<std::string>& registrations = {}) {
    Add(TraceKind::kJournalAttempt, sequence, event);
    Add(TraceKind::kJournalCommitted, sequence, event);
    Add(TraceKind::kSnapshotActivated, sequence, event);
    for (const auto& action : registrations)
      Add(TraceKind::kSourceRegistered, sequence, event, core::EffectId::kInvalid, action);
    for (const auto& [effect, action] : effects) {
      // The reducer declaration and the private mapping are separate ordered facts.
      Add(TraceKind::kEffect, sequence, event, effect);
      const auto mapping_kind = action.starts_with("timer:")     ? TraceKind::kTimerAction
                                : action.starts_with("ack:")     ? TraceKind::kAckAuthorized
                                : action.starts_with("publish:") ? TraceKind::kTerminalPublished
                                : action.starts_with("safety:")  ? TraceKind::kSafetyAction
                                : action.starts_with("handoff:") ? TraceKind::kCapabilityHandoff
                                                                 : TraceKind::kEffect;
      Add(mapping_kind, sequence, event, effect, action);
    }
    Add(TraceKind::kResponseReleased, sequence, event);
  }

  friend bool operator==(const ExpectedTrace& lhs, const std::vector<TraceRecord>& rhs) {
    return lhs.records == rhs;
  }
};

struct GeneratedIdentities {
  core::Uuid job_id;
  core::Uuid session_id;
  core::Uuid worker_id;
  core::StableId launch_operation_id;
};

class CallbackEndpoint {
 public:
  CallbackEndpoint() = default;
  [[nodiscard]] IngressResult Invoke(const core::RawCandidateEvent& event);
  [[nodiscard]] IngressResult Invoke(core::RawCandidateEvent&& event) noexcept;
  [[nodiscard]] bool HoldLease();
  [[nodiscard]] bool ReleaseLease();

 private:
  explicit CallbackEndpoint(core::internal::CallbackHandle handle) : handle_(std::move(handle)) {}
  core::internal::CallbackHandle handle_;
  friend class JobOrchestratorHarness;
};

class JobOrchestratorHarness final {
 public:
  explicit JobOrchestratorHarness(Config config);
  ~JobOrchestratorHarness();
  JobOrchestratorHarness(const JobOrchestratorHarness&) = delete;
  JobOrchestratorHarness& operator=(const JobOrchestratorHarness&) = delete;

  // These private-harness operations model the one creation/materialization path owned by
  // the future writer.  They are deliberately not installed public APIs.
  [[nodiscard]] IngressResult Create();
  [[nodiscard]] std::optional<GeneratedIdentities> generated_identities() const;
  [[nodiscard]] IngressResult SubmitGeneratedLaunchIntent(const core::Uuid&,
                                                          const AllocationFixture&);
  [[nodiscard]] IngressResult SubmitGeneratedLaunchIntent(const AllocationFixture&);
  [[nodiscard]] IngressResult SubmitJobCreated(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitResourcesCommitted(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitLaunchIntent(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitLaunchObserved(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitWorkerRunning(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitSessionRetainRequested(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitSessionRetained(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitFinalizationCompleted(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitTerminalOutcome(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitCancel(const core::Command&);
  [[nodiscard]] IngressResult SubmitTerminate(const core::Command&);
  [[nodiscard]] IngressResult SubmitWorker(const core::RawCandidateEvent&);
  [[nodiscard]] TimerSubmitResult SubmitTimeout(const core::TimerNotification&);
  [[nodiscard]] IngressResult SubmitProcessExit(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitResourcesReleased(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitCleanup(const core::RawCandidateEvent&);
  [[nodiscard]] IngressResult SubmitConflictingResources(const core::Uuid&);
  [[nodiscard]] IngressResult SubmitShutdown();
  // Registration is valid only after the reducer-derived position has made the source possible.
  // Source gates are acquired by the corresponding reducer effect/admission path. These
  // retention probes are legal only after the source has reached its reducer-valid position.
  [[nodiscard]] bool RetainTimerLease(const core::Uuid&, core::TimeoutPhase);
  [[nodiscard]] bool RetainWorkerLease(const core::Uuid&);
  [[nodiscard]] bool RetainProcessExitLease(const core::Uuid&);
  [[nodiscard]] bool RetainResourcesReleasedLease(const core::Uuid&);
  [[nodiscard]] bool RetainCleanupLease(const core::Uuid&);
  [[nodiscard]] bool RetireWorkerAck(const core::Uuid&, std::uint64_t);
  [[nodiscard]] bool LatchReadinessFailure();
  [[nodiscard]] bool BeginShutdown();
  [[nodiscard]] bool ArmPause(WriterPhase);
  [[nodiscard]] bool ArmBarrier(WriterPhase);
  // Admission-stage pause holds the registration-to-first-insertion linearization point.
  [[nodiscard]] bool ArmAdmissionPause();
  [[nodiscard]] bool WaitForAdmissionPause();
  [[nodiscard]] bool WaitForAdmissionAttempts(std::size_t count);
  [[nodiscard]] bool WaitForWaitUntilAttempts(std::size_t count);
  [[nodiscard]] bool ReleaseAdmissionPause();
  [[nodiscard]] bool WaitUntil(std::uint64_t ingress_sequence, WriterPhase);
  [[nodiscard]] bool Release(std::uint64_t ingress_sequence, WriterPhase);
  [[nodiscard]] CallbackEndpoint RetainedCallback();
  [[nodiscard]] bool HoldCallbackLease();
  [[nodiscard]] bool CloseCallbackLease();
  [[nodiscard]] bool ConcurrentCallbackInvocation();
  [[nodiscard]] bool VerifyFakes();
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool sealed() const noexcept;
  [[nodiscard]] bool stopped() const noexcept;
  [[nodiscard]] std::size_t journal_attempts() const noexcept;
  [[nodiscard]] std::size_t apply_count() const noexcept;
  [[nodiscard]] std::size_t writer_turn_count() const noexcept;
  [[nodiscard]] std::size_t max_concurrent_writer_turns() const noexcept;
  [[nodiscard]] std::size_t disposed_count() const noexcept;
  [[nodiscard]] std::size_t completion_count() const noexcept;
  [[nodiscard]] std::size_t ack_authorization_count() const noexcept;
  [[nodiscard]] std::size_t effect_count() const noexcept;
  [[nodiscard]] std::size_t response_count() const noexcept;
  [[nodiscard]] std::size_t resident_count() const noexcept;
  [[nodiscard]] std::size_t critical_occupancy() const noexcept;
  [[nodiscard]] std::size_t normal_occupancy() const noexcept;
  [[nodiscard]] std::size_t total_occupancy() const noexcept;
  [[nodiscard]] std::optional<Completion> TakeCompletion(std::uint64_t ingress_sequence);
  [[nodiscard]] std::optional<core::Snapshot> Snapshot(const core::Uuid&) const;
  [[nodiscard]] std::vector<TraceRecord> CopyTrace() const;
  [[nodiscard]] std::vector<core::LogicalJobEvent> CopyJournalAttempts() const;
  [[nodiscard]] std::vector<std::uint64_t> CopyIngressSequences() const;
  [[nodiscard]] std::vector<ExpectedEnvelope> CopyExpectedEnvelopes() const;
  [[nodiscard]] std::vector<core::ApplicationLaunchRequest> CopyLaunchRequests() const;
  [[nodiscard]] std::vector<core::SessionRetainRequest> CopySessionRequests() const;
  [[nodiscard]] std::optional<core::RawCandidateEvent> TakeRunnerCandidate();
  [[nodiscard]] std::optional<core::RawCandidateEvent> TakeSessionCandidate();
  [[nodiscard]] bool CancelRunnerCandidate();
  [[nodiscard]] bool CancelSessionCandidate();
  [[nodiscard]] std::size_t creation_claim_count(const core::Uuid&) const noexcept;
  [[nodiscard]] bool NoForbiddenPostcommitActions() const noexcept;
  [[nodiscard]] bool NoPostcommitAllocationOrCopy() const noexcept;
  [[nodiscard]] bool DestinationCapacityCheckedBeforeCommit() const noexcept;
  void InjectPrecommitMaterializationFailure();
  void InjectClockReadFailure() noexcept;
  void InjectAccountingCorruption();
  void InjectResidualCriticalPermit();
  void SetNextCommitResult(core::LogicalCommitResult);
  void SetDestinationCapacity(std::size_t);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_JOB_ORCHESTRATOR_HARNESS_HPP_
