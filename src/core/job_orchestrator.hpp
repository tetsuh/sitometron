#ifndef SITOMETRON_CORE_JOB_ORCHESTRATOR_HPP_
#define SITOMETRON_CORE_JOB_ORCHESTRATOR_HPP_

#include <array>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/core/job_reducer.hpp"

namespace sitometron::core::internal {

enum class IngressCode {
  kAdmitted,
  kCoalescedPending,
  kAlreadyPending,
  kNormalFull,
  kResidentLimit,
  kAdmissionClosed,
  kServiceFailed
};
struct IngressResult {
  IngressCode code = IngressCode::kServiceFailed;
  std::uint64_t ingress_sequence = 0;
  std::size_t completion_count_before = 0;
};
struct Ports {
  ClockPort* clock = nullptr;
  JobJournalPort* journal = nullptr;
  ApplicationRunnerPort* runner = nullptr;
  SessionRetainerPort* session = nullptr;
  IdentitySourcePort* identity = nullptr;
  void* context = nullptr;
  void (*set_commit_result)(void*, LogicalCommitResult) noexcept = nullptr;
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
  kSealed
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
  kResponseReleased
};
struct TraceRecord {
  std::uint64_t ordinal = 0;
  TraceKind kind = TraceKind::kJournalAttempt;
  std::uint64_t journal_sequence = 0;
  EventType event_type = EventType::kInvalid;
  EffectId effect_id = EffectId::kInvalid;
  std::string action;
  bool ingress_mutex_held = false;
  bool writer_context = false;
  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;
};
struct Completion {
  enum class Code { kSuccess, kReducerRejection, kServiceFailed };
  Code code = Code::kServiceFailed;
  std::optional<Rejection> rejection;
};
class CallbackHandle;

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
  LogicalCommitResult commit_result = LogicalCommitResult::kCommitted;
  Ports ports{};
  constexpr std::size_t critical_reserve() const noexcept {
    constexpr auto m = std::numeric_limits<std::size_t>::max();
    return max_jobs > (m - 1U) / 9U ? 0U : 9U * max_jobs + 1U;
  }
  constexpr std::size_t total_capacity() const noexcept {
    const auto reserve = critical_reserve();
    if (reserve == 0U) return 0U;
    return reserve > std::numeric_limits<std::size_t>::max() - normal_capacity
               ? 0U
               : normal_capacity + reserve;
  }
};

class JobOrchestrator final {
 public:
  explicit JobOrchestrator(Config config);
  ~JobOrchestrator();
  JobOrchestrator(const JobOrchestrator&) = delete;
  JobOrchestrator& operator=(const JobOrchestrator&) = delete;

  IngressResult Create();
  // Production derives normal/critical class, resident reservation, and source
  // gate solely from the closed command/candidate vocabulary.
  IngressResult SubmitCandidate(const RawCandidateEvent&);
  IngressResult SubmitCandidate(RawCandidateEvent&&) noexcept;
  IngressResult SubmitCommand(const Command&);
  IngressResult SubmitCommand(Command&&) noexcept;
  IngressResult SubmitWorker(const RawCandidateEvent&);
  IngressResult SubmitWorker(RawCandidateEvent&&) noexcept;
  // Typed timer ingress validates the registered writer-owned generation
  // before it can become a raw reducer candidate. Discarded notifications do
  // not enter ingress and therefore do not receive an IngressResult.
  struct TimerSubmitResult {
    bool discarded = false;
    IngressResult admitted{};
  };
  TimerSubmitResult SubmitTimeout(const TimerNotification&);
  IngressResult SubmitShutdown();
  bool RetainTimerLease(const Uuid&, TimeoutPhase);
  bool RetainWorkerLease(const Uuid&);
  bool RetainProcessExitLease(const Uuid&);
  bool RetainResourcesReleasedLease(const Uuid&);
  bool RetainCleanupLease(const Uuid&);
  bool RetireWorkerAck(const RawCandidateEvent&);
  bool LatchReadinessFailure();
  bool WaitForWriterIdle();
  bool BeginShutdown();
  bool FinishShutdown();
  bool ArmPause(WriterPhase);
  bool ArmBarrier(WriterPhase);
  bool ArmAdmissionPause();
  bool WaitForAdmissionPause();
  std::size_t admission_attempt_count() const noexcept;
  bool WaitForAdmissionAttempts(std::size_t);
  bool WaitForWaitUntilAttempts(std::size_t);
  bool ReleaseAdmissionPause();
  bool WaitUntil(std::uint64_t, WriterPhase);
  bool Release(std::uint64_t, WriterPhase);
  bool failed() const noexcept;
  bool sealed() const noexcept;
  bool stopped() const noexcept;
  std::optional<Uuid> LastCreated() const;
  std::optional<Uuid> GeneratedJob() const;
  std::optional<Uuid> GeneratedWorker() const;
  std::optional<StableId> GeneratedLaunch() const;
  std::optional<Uuid> GeneratedWorker(const Uuid&) const;
  std::optional<StableId> GeneratedLaunch(const Uuid&) const;
  std::size_t journal_attempts() const noexcept;
  std::size_t apply_count() const noexcept;
  std::size_t writer_turn_count() const noexcept;
  std::size_t max_concurrent_writer_turns() const noexcept;
  std::size_t disposed_count() const noexcept;
  std::size_t completion_count() const noexcept;
  std::size_t ack_authorization_count() const noexcept;
  std::size_t effect_count() const noexcept;
  std::size_t response_count() const noexcept;
  std::size_t resident_count() const noexcept;
  std::size_t live_critical_permit_count() const noexcept;
  std::size_t critical_occupancy() const noexcept;
  std::size_t normal_occupancy() const noexcept;
  std::size_t total_occupancy() const noexcept;
  std::size_t creation_claim_count(const Uuid&) const noexcept;
  std::optional<Completion> TakeCompletion(std::uint64_t);
  std::optional<Snapshot> SnapshotFor(const Uuid&) const;
  std::vector<TraceRecord> CopyTrace() const;
  std::vector<LogicalJobEvent> CopyJournalAttempts() const;
  std::vector<std::uint64_t> CopyIngressSequences() const;
  std::vector<ApplicationLaunchRequest> CopyLaunchRequests() const;
  std::vector<SessionRetainRequest> CopySessionRequests() const;
  std::optional<RawCandidateEvent> TakeRunnerCandidate();
  std::optional<RawCandidateEvent> TakeSessionCandidate();
  bool CancelRunnerCandidate() const;
  bool CancelSessionCandidate() const;
  bool VerifyFakes() const noexcept;
  bool NoForbiddenPostcommitActions() const noexcept;
  bool NoPostcommitAllocationOrCopy() const noexcept;
  bool DestinationCapacityCheckedBeforeCommit() const noexcept;
  void InjectPrecommitMaterializationFailure();
  void InjectAccountingCorruption();
  void InjectResidualCriticalPermit();
  void SetNextCommitResult(LogicalCommitResult);
  void SetDestinationCapacity(std::size_t);
  bool IsWriterThread() const noexcept;
  void Notify() noexcept;
  [[nodiscard]] CallbackHandle RetainedCallback();

 private:
  friend class CallbackHandle;
  struct Impl;
  void Run() noexcept;
  void Stop() noexcept;
  void ScheduleAcceptedEntry() noexcept;
  void LatchFailureFromCallback() noexcept;
  void TrySealFailure(bool allow_one_invocation = false) noexcept;
  std::unique_ptr<Impl> impl_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  bool started_ = false;
  bool stopping_ = false;
  std::size_t pending_ = 0;
  std::size_t active_ = 0;
  std::thread::id writer_id_{};
  std::thread writer_;
};

// A private lifetime-safe ingress endpoint. It retains only a shared control
// block; sealing severs its writer access before writer-owned state is gone.
class CallbackHandle final {
 public:
  CallbackHandle() = default;
  [[nodiscard]] IngressResult Invoke(const RawCandidateEvent&) const;
  [[nodiscard]] IngressResult Invoke(RawCandidateEvent&&) const noexcept;
  [[nodiscard]] bool HoldLease() const;
  [[nodiscard]] bool ReleaseLease() const;

 private:
  struct Control;
  explicit CallbackHandle(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  std::shared_ptr<Control> control_;
  friend class JobOrchestrator;
};

}  // namespace sitometron::core::internal
#endif
