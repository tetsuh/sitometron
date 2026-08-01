#ifndef SITOMETRON_CORE_JOB_REDUCER_HPP_
#define SITOMETRON_CORE_JOB_REDUCER_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sitometron::core {

struct Uuid {
  std::string value;
  friend bool operator==(const Uuid&, const Uuid&) = default;
};
struct StableId {
  std::string value;
  friend bool operator==(const StableId&, const StableId&) = default;
};
struct Digest {
  std::string value;
  friend bool operator==(const Digest&, const Digest&) = default;
};

enum class JobState { kAdmitted, kPreparing, kRunning, kStopping, kFinalizing,
                      kSucceeded, kFailed, kCancelled, kTerminated, kTimedOut };
enum class CommandType { kCancel, kTerminate };
enum class TimeoutPhase { kPreparation, kExecution, kCooperativeStop, kProcessExitConfirmation };
enum class TerminalOutcome { kSucceeded, kFailed, kCancelled, kTerminated, kTimedOut };
enum class RejectionReason { kJobNotFound, kJobAlreadyExists, kCommandNotAllowedInState,
  kEventNotAllowedInState, kStopCauseAlreadyLatched, kTimeoutPhaseMismatch,
  kTerminalOutcomeMismatch, kRequiredFinalizationFactMissing, kInvalidEventPayload,
  kInvariantViolation };
enum class Disposition { kTransition, kAudit, kLateAudit, kReject };
enum class EffectId { kArmPreparationTimeout, kDisarmPreparationTimeout, kLaunchWorkerOnce,
  kArmExecutionTimeout, kDisarmExecutionTimeout, kRequestCooperativeStop, kRequestForcedStop,
  kRetainSessionSameIdentity, kAckTerminalWorkerEventIfPending, kAckLateWorkerEvent,
  kPublishTerminalResult, kArmProcessExitConfirmationTimeoutIfNeeded, kQuarantineResources,
  kSetReadinessFalse, kArmCooperativeStopTimeout, kDisarmCooperativeStopTimeout,
  kDisarmProcessExitConfirmationTimeout };

enum class ResourceStatus { kNone, kCommitted, kReleased };
enum class LaunchStatus { kNotStarted, kIntentRecorded, kObserved, kFailed };
enum class ProcessPresence { kAbsent, kUnknown, kPresent };
enum class RetentionStatus { kNotStarted, kRequested, kRetained };
enum class FinalizationStatus { kNotStarted, kPending, kCompleted, kFailed };
enum class CleanupStatus { kPending, kCompleted, kIncomplete };
enum class CompletionMode { kNone, kCooperative, kForced, kProcessAlreadyExited };

enum class EventType {
  kJobCreated, kResourcesCommitted, kWorkerLaunchIntent, kWorkerLaunchObserved, kWorkerRunning,
  kCancelAccepted, kTerminateAccepted, kTimeoutExpired, kWorkerCompleted, kWorkerFailed,
  kProcessExitConfirmed, kSessionRetainRequested, kSessionRetained, kFinalizationCompleted,
  kFinalizationFailed, kTerminalOutcomeCommitted, kResourcesReleased, kCleanupStatusRecorded,
  kLateWorkerEvent
};

struct EmptyPayload {};
struct JobCreatedPayload { Uuid session_id; };
struct ResourcesCommittedPayload { StableId allocation_id; Digest allocation_digest;
  StableId schema_id; std::uint32_t schema_version = 0; std::string payload_utf8; };
struct WorkerLaunchIntentPayload { StableId operation_id; StableId application_id; std::string application_version;
  Digest bundle_sha256; StableId allocation_id; Digest allocation_digest; Uuid worker_id; };
struct WorkerLaunchObservedPayload { StableId operation_id; bool started = false; };
struct WorkerRunningPayload { Uuid worker_id; };
struct PrincipalPayload { std::string principal_subject; };
struct TimeoutExpiredPayload { TimeoutPhase phase; std::uint64_t timer_generation = 0; };
struct WorkerEventPayload { Uuid worker_id; std::uint64_t event_sequence = 0; };
struct ProcessExitConfirmedPayload { CompletionMode completion_mode; StableId launch_operation_id; };
struct SessionPayload { Uuid session_id; };
struct TerminalOutcomePayload { TerminalOutcome outcome; };
struct ResourcesReleasedPayload { StableId allocation_id; Digest allocation_digest; };
struct CleanupStatusPayload { CleanupStatus status; };
struct LateWorkerEventPayload { EventType original_event_type; Uuid worker_id; std::uint64_t event_sequence = 0; };

using EventPayload = std::variant<EmptyPayload, JobCreatedPayload, ResourcesCommittedPayload,
  WorkerLaunchIntentPayload, WorkerLaunchObservedPayload, WorkerRunningPayload, PrincipalPayload,
  TimeoutExpiredPayload, WorkerEventPayload, ProcessExitConfirmedPayload, SessionPayload,
  TerminalOutcomePayload, ResourcesReleasedPayload, CleanupStatusPayload, LateWorkerEventPayload>;

struct Snapshot {
  std::uint32_t schema_version = 1;
  Uuid job_id;
  Uuid session_id;
  bool entity_exists = false;
  JobState state = JobState::kAdmitted;
  std::optional<TerminalOutcome> latched_reason;
  bool completion_candidate = false;
  CompletionMode completion_mode = CompletionMode::kNone;
  ResourceStatus resource_status = ResourceStatus::kNone;
  std::optional<StableId> allocation_id;
  std::optional<Digest> allocation_digest;
  LaunchStatus worker_launch_status = LaunchStatus::kNotStarted;
  std::optional<StableId> launch_operation_id;
  std::optional<Uuid> worker_id;
  ProcessPresence process_presence = ProcessPresence::kAbsent;
  bool process_exit_confirmed = false;
  RetentionStatus session_retention_status = RetentionStatus::kNotStarted;
  FinalizationStatus finalization_status = FinalizationStatus::kNotStarted;
  CleanupStatus cleanup_status = CleanupStatus::kPending;
  bool pending_worker_event_ack = false;
  std::optional<Uuid> pending_worker_id;
  std::optional<std::uint64_t> pending_worker_event_sequence;
};

struct Command { std::uint32_t schema_version = 1; CommandType command_type;
  Uuid job_id; std::string principal_subject; };
struct RawCandidateEvent { std::uint32_t schema_version = 1; Uuid job_id;
  std::string event_type; std::string payload_json; };
struct InternalEvent { std::uint32_t schema_version = 1; Uuid job_id;
  EventType event_type; EventPayload payload; };
struct PreEnvelopeProposal { std::uint32_t schema_version = 1; Uuid job_id;
  EventType event_type; EventPayload payload; };
struct Rejection { std::uint32_t schema_version = 1; RejectionReason reason; };
struct Decision { std::variant<Rejection, PreEnvelopeProposal> value; };
struct NormalizedCandidate { std::variant<Rejection, InternalEvent> value; };
struct Effect { EffectId id; };
struct ApplyResult {
  Snapshot snapshot;
  std::vector<Effect> effects;
  std::optional<Rejection> rejection;
};
struct TimerArmRequest {
  Uuid job_id;
  TimeoutPhase phase;
  std::uint64_t generation = 0;
};
struct TimerNotification {
  Uuid job_id;
  TimeoutPhase phase;
  std::uint64_t generation = 0;
};

enum class TimerIngressKind { kFailClosed, kDiscardWithoutCandidate, kEmitCandidateEvent };
struct TimerState { bool preparation_armed = false; bool execution_armed = false;
  bool cooperative_stop_armed = false; bool process_exit_confirmation_armed = false;
  std::uint64_t preparation_generation = 0; std::uint64_t execution_generation = 0;
  std::uint64_t cooperative_stop_generation = 0; std::uint64_t process_exit_confirmation_generation = 0; };
struct TimeoutCandidate { Uuid job_id; TimeoutExpiredPayload payload; };
struct TimerIngressInput {
  TimerArmRequest arm_request;
  std::optional<TimerNotification> notification;
};
struct TimerIngressResult { TimerIngressKind kind; std::optional<TimeoutCandidate> candidate;
  std::vector<Effect> effects; };

[[nodiscard]] Snapshot InitialSnapshot(const Uuid& job_id, const Uuid& session_id);
[[nodiscard]] Decision DecideCommand(const Snapshot&, const Command&);
[[nodiscard]] NormalizedCandidate NormalizeCandidate(const Snapshot&, const RawCandidateEvent&);
[[nodiscard]] Decision DecideEvent(const Snapshot&, const InternalEvent&);
[[nodiscard]] ApplyResult Apply(const Snapshot&, const PreEnvelopeProposal&);
[[nodiscard]] TimerIngressResult IngestTimer(const TimerState&, const TimerIngressInput&);
[[nodiscard]] TimerIngressResult IngestTimer(const TimerState&, const TimerNotification&);
[[nodiscard]] TimerIngressResult IngestTimer(const TimerState&, const Uuid&, TimeoutPhase,
                                              std::uint64_t generation);

[[nodiscard]] std::string_view ToString(JobState) noexcept;
[[nodiscard]] std::string_view ToString(EventType) noexcept;
[[nodiscard]] std::string_view ToString(RejectionReason) noexcept;
[[nodiscard]] std::string_view ToString(EffectId) noexcept;

}  // namespace sitometron::core

#endif  // SITOMETRON_CORE_JOB_REDUCER_HPP_
