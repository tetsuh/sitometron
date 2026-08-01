#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "sitometron/core/job_reducer.hpp"

namespace sitometron::test {
namespace {
using Json = nlohmann::json;
using namespace sitometron::core;
using namespace std::string_view_literals;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "job_reducer_vectors: " << message << '\n';
  return 1;
}

Uuid U(const Json& j) { return Uuid{j.get<std::string>()}; }
std::optional<StableId> Stable(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<StableId>{StableId{j.get<std::string>()}};
}
std::optional<Digest> DigestValue(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<Digest>{Digest{j.get<std::string>()}};
}
std::optional<Uuid> UuidValue(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<Uuid>{U(j)};
}

template <typename Enum, std::size_t Size>
Enum ClosedEnum(std::string_view value,
                const std::array<std::pair<std::string_view, Enum>, Size>& values,
                std::string_view field) {
  for (const auto& [name, enumeration] : values)
    if (value == name) return enumeration;
  throw std::invalid_argument("unknown " + std::string(field) + ": " + std::string(value));
}

JobState State(std::string_view value) {
  static constexpr std::array values{std::pair{"admitted"sv, JobState::kAdmitted},
                                     std::pair{"preparing"sv, JobState::kPreparing},
                                     std::pair{"running"sv, JobState::kRunning},
                                     std::pair{"stopping"sv, JobState::kStopping},
                                     std::pair{"finalizing"sv, JobState::kFinalizing},
                                     std::pair{"succeeded"sv, JobState::kSucceeded},
                                     std::pair{"failed"sv, JobState::kFailed},
                                     std::pair{"cancelled"sv, JobState::kCancelled},
                                     std::pair{"terminated"sv, JobState::kTerminated},
                                     std::pair{"timed_out"sv, JobState::kTimedOut}};
  return ClosedEnum(value, values, "state");
}
std::optional<TerminalOutcome> Outcome(const Json& j) {
  if (j.is_null()) return std::nullopt;
  static constexpr std::array values{std::pair{"failed"sv, TerminalOutcome::kFailed},
                                     std::pair{"cancelled"sv, TerminalOutcome::kCancelled},
                                     std::pair{"terminated"sv, TerminalOutcome::kTerminated},
                                     std::pair{"timed_out"sv, TerminalOutcome::kTimedOut}};
  return ClosedEnum(j.get<std::string>(), values, "latched_reason");
}
CommandType ArtifactCommandType(std::string_view value) {
  static constexpr std::array values{std::pair{"cancel"sv, CommandType::kCancel},
                                     std::pair{"terminate"sv, CommandType::kTerminate}};
  return ClosedEnum(value, values, "command_type");
}
enum class ArtifactDisposition { kTransition, kAudit, kLateAudit, kReject };
ArtifactDisposition Disposition(std::string_view value) {
  static constexpr std::array values{std::pair{"transition"sv, ArtifactDisposition::kTransition},
                                     std::pair{"audit"sv, ArtifactDisposition::kAudit},
                                     std::pair{"late_audit"sv, ArtifactDisposition::kLateAudit},
                                     std::pair{"reject"sv, ArtifactDisposition::kReject}};
  return ClosedEnum(value, values, "disposition");
}
EventType ArtifactEventType(std::string_view value) {
  static constexpr std::array values{
      std::pair{"job_created"sv, EventType::kJobCreated},
      std::pair{"resources_committed"sv, EventType::kResourcesCommitted},
      std::pair{"worker_launch_intent"sv, EventType::kWorkerLaunchIntent},
      std::pair{"worker_launch_observed"sv, EventType::kWorkerLaunchObserved},
      std::pair{"worker_running"sv, EventType::kWorkerRunning},
      std::pair{"cancel_accepted"sv, EventType::kCancelAccepted},
      std::pair{"terminate_accepted"sv, EventType::kTerminateAccepted},
      std::pair{"timeout_expired"sv, EventType::kTimeoutExpired},
      std::pair{"worker_completed"sv, EventType::kWorkerCompleted},
      std::pair{"worker_failed"sv, EventType::kWorkerFailed},
      std::pair{"process_exit_confirmed"sv, EventType::kProcessExitConfirmed},
      std::pair{"session_retain_requested"sv, EventType::kSessionRetainRequested},
      std::pair{"session_retained"sv, EventType::kSessionRetained},
      std::pair{"finalization_completed"sv, EventType::kFinalizationCompleted},
      std::pair{"finalization_failed"sv, EventType::kFinalizationFailed},
      std::pair{"terminal_outcome_committed"sv, EventType::kTerminalOutcomeCommitted},
      std::pair{"resources_released"sv, EventType::kResourcesReleased},
      std::pair{"cleanup_status_recorded"sv, EventType::kCleanupStatusRecorded},
      std::pair{"late_worker_event"sv, EventType::kLateWorkerEvent}};
  return ClosedEnum(value, values, "event_type");
}
Snapshot SnapshotFrom(const Json& j, const Json* absent_input = nullptr) {
  if (j.is_null()) {
    Uuid job{"01890f3e-7b00-7abc-8abc-0123456789ab"};
    Uuid session = job;
    if (absent_input && absent_input->contains("job_id")) {
      job = U(absent_input->at("job_id"));
      if (absent_input->contains("payload") && absent_input->at("payload").is_object() &&
          absent_input->at("payload").contains("session_id"))
        session = U(absent_input->at("payload").at("session_id"));
      else
        session = job;
    }
    return InitialSnapshot(job, session);
  }
  Snapshot s;
  s.state = JobState::kAdmitted;
  s.completion_mode = CompletionMode::kNone;
  s.resource_status = ResourceStatus::kNone;
  s.worker_launch_status = LaunchStatus::kNotStarted;
  s.process_presence = ProcessPresence::kAbsent;
  s.session_retention_status = RetentionStatus::kNotStarted;
  s.finalization_status = FinalizationStatus::kNotStarted;
  s.cleanup_status = CleanupStatus::kPending;
  s.entity_exists = true;
  s.schema_version = j.at("schema_version").get<std::uint32_t>();
  s.job_id = U(j.at("job_id"));
  s.session_id = U(j.at("session_id"));
  s.state = State(j.at("state").get<std::string>());
  s.latched_reason = Outcome(j.at("latched_reason"));
  s.completion_candidate = !j.at("completion_candidate").is_null();
  if (!j.at("completion_mode").is_null()) {
    static constexpr std::array values{
        std::pair{"cooperative"sv, CompletionMode::kCooperative},
        std::pair{"forced"sv, CompletionMode::kForced},
        std::pair{"process_already_exited"sv, CompletionMode::kProcessAlreadyExited}};
    s.completion_mode =
        ClosedEnum(j.at("completion_mode").get<std::string>(), values, "completion_mode");
  }
  static constexpr std::array resource_values{std::pair{"none"sv, ResourceStatus::kNone},
                                              std::pair{"committed"sv, ResourceStatus::kCommitted},
                                              std::pair{"released"sv, ResourceStatus::kReleased}};
  s.resource_status =
      ClosedEnum(j.at("resource_status").get<std::string>(), resource_values, "resource_status");
  s.allocation_id = Stable(j.at("allocation_id"));
  s.allocation_digest = DigestValue(j.at("allocation_digest"));
  static constexpr std::array launch_values{
      std::pair{"not_started"sv, LaunchStatus::kNotStarted},
      std::pair{"intent_recorded"sv, LaunchStatus::kIntentRecorded},
      std::pair{"observed"sv, LaunchStatus::kObserved},
      std::pair{"failed"sv, LaunchStatus::kFailed}};
  s.worker_launch_status = ClosedEnum(j.at("worker_launch_status").get<std::string>(),
                                      launch_values, "worker_launch_status");
  s.launch_operation_id = Stable(j.at("launch_operation_id"));
  s.worker_id = UuidValue(j.at("worker_id"));
  static constexpr std::array presence_values{std::pair{"absent"sv, ProcessPresence::kAbsent},
                                              std::pair{"present"sv, ProcessPresence::kPresent},
                                              std::pair{"unknown"sv, ProcessPresence::kUnknown}};
  s.process_presence =
      ClosedEnum(j.at("process_presence").get<std::string>(), presence_values, "process_presence");
  s.process_exit_confirmed = j.at("process_exit_confirmed").get<bool>();
  static constexpr std::array retention_values{
      std::pair{"not_started"sv, RetentionStatus::kNotStarted},
      std::pair{"requested"sv, RetentionStatus::kRequested},
      std::pair{"retained"sv, RetentionStatus::kRetained}};
  s.session_retention_status = ClosedEnum(j.at("session_retention_status").get<std::string>(),
                                          retention_values, "session_retention_status");
  static constexpr std::array finalization_values{
      std::pair{"not_started"sv, FinalizationStatus::kNotStarted},
      std::pair{"pending"sv, FinalizationStatus::kPending},
      std::pair{"completed"sv, FinalizationStatus::kCompleted},
      std::pair{"failed"sv, FinalizationStatus::kFailed}};
  s.finalization_status = ClosedEnum(j.at("finalization_status").get<std::string>(),
                                     finalization_values, "finalization_status");
  static constexpr std::array cleanup_values{std::pair{"pending"sv, CleanupStatus::kPending},
                                             std::pair{"completed"sv, CleanupStatus::kCompleted},
                                             std::pair{"incomplete"sv, CleanupStatus::kIncomplete}};
  s.cleanup_status =
      ClosedEnum(j.at("cleanup_status").get<std::string>(), cleanup_values, "cleanup_status");
  s.pending_worker_event_ack = j.at("pending_worker_event_ack").get<bool>();
  s.pending_worker_id = UuidValue(j.at("pending_worker_id"));
  if (!j.at("pending_worker_event_sequence").is_null())
    s.pending_worker_event_sequence = j.at("pending_worker_event_sequence").get<std::uint64_t>();
  return s;
}
Json SnapshotJson(const Snapshot& s) {
  Json j{{"schema_version", s.schema_version},
         {"job_id", s.job_id.value},
         {"session_id", s.session_id.value},
         {"state", std::string(ToString(s.state))},
         {"latched_reason", nullptr},
         {"completion_candidate", s.completion_candidate ? Json("succeeded") : Json(nullptr)},
         {"completion_mode", nullptr},
         {"resource_status", "none"},
         {"allocation_id", nullptr},
         {"allocation_digest", nullptr},
         {"worker_launch_status", "not_started"},
         {"launch_operation_id", nullptr},
         {"worker_id", nullptr},
         {"process_presence", "absent"},
         {"process_exit_confirmed", s.process_exit_confirmed},
         {"session_retention_status", "not_started"},
         {"finalization_status", "not_started"},
         {"cleanup_status", "pending"},
         {"pending_worker_event_ack", s.pending_worker_event_ack},
         {"pending_worker_id", nullptr},
         {"pending_worker_event_sequence", nullptr}};
  if (s.latched_reason)
    j["latched_reason"] = *s.latched_reason == TerminalOutcome::kFailed       ? "failed"
                          : *s.latched_reason == TerminalOutcome::kCancelled  ? "cancelled"
                          : *s.latched_reason == TerminalOutcome::kTerminated ? "terminated"
                                                                              : "timed_out";
  if (s.completion_mode != CompletionMode::kNone)
    j["completion_mode"] = s.completion_mode == CompletionMode::kCooperative ? "cooperative"
                           : s.completion_mode == CompletionMode::kForced
                               ? "forced"
                               : "process_already_exited";
  j["resource_status"] = s.resource_status == ResourceStatus::kCommitted  ? "committed"
                         : s.resource_status == ResourceStatus::kReleased ? "released"
                                                                          : "none";
  if (s.allocation_id) j["allocation_id"] = s.allocation_id->value;
  if (s.allocation_digest) j["allocation_digest"] = s.allocation_digest->value;
  j["worker_launch_status"] = s.worker_launch_status == LaunchStatus::kIntentRecorded
                                  ? "intent_recorded"
                              : s.worker_launch_status == LaunchStatus::kObserved ? "observed"
                              : s.worker_launch_status == LaunchStatus::kFailed   ? "failed"
                                                                                  : "not_started";
  if (s.launch_operation_id) j["launch_operation_id"] = s.launch_operation_id->value;
  if (s.worker_id) j["worker_id"] = s.worker_id->value;
  j["process_presence"] = s.process_presence == ProcessPresence::kPresent   ? "present"
                          : s.process_presence == ProcessPresence::kUnknown ? "unknown"
                                                                            : "absent";
  j["session_retention_status"] =
      s.session_retention_status == RetentionStatus::kRequested  ? "requested"
      : s.session_retention_status == RetentionStatus::kRetained ? "retained"
                                                                 : "not_started";
  const char* finalization_status =
      s.finalization_status == FinalizationStatus::kPending     ? "pending"
      : s.finalization_status == FinalizationStatus::kCompleted ? "completed"
      : s.finalization_status == FinalizationStatus::kFailed    ? "failed"
                                                                : "not_started";
  j["finalization_status"] = finalization_status;
  j["cleanup_status"] = s.cleanup_status == CleanupStatus::kCompleted    ? "completed"
                        : s.cleanup_status == CleanupStatus::kIncomplete ? "incomplete"
                                                                         : "pending";
  if (s.pending_worker_id) j["pending_worker_id"] = s.pending_worker_id->value;
  if (s.pending_worker_event_sequence)
    j["pending_worker_event_sequence"] = *s.pending_worker_event_sequence;
  return j;
}

Json PayloadJson(EventType type, const EventPayload& payload) {
  switch (type) {
    case EventType::kJobCreated:
      return Json{{"session_id", std::get<JobCreatedPayload>(payload).session_id.value}};
    case EventType::kResourcesCommitted: {
      const auto& p = std::get<ResourcesCommittedPayload>(payload);
      return Json{{"allocation_id", p.allocation_id.value},
                  {"allocation_digest", p.allocation_digest.value},
                  {"resolved_allocation",
                   {{"schema_id", p.schema_id.value},
                    {"schema_version", p.schema_version},
                    {"payload_utf8", p.payload_utf8}}}};
    }
    case EventType::kWorkerLaunchIntent: {
      const auto& p = std::get<WorkerLaunchIntentPayload>(payload);
      return Json{{"operation_id", p.operation_id.value},
                  {"application",
                   {{"application_id", p.application_id.value},
                    {"version", p.application_version},
                    {"bundle_sha256", p.bundle_sha256.value}}},
                  {"allocation_id", p.allocation_id.value},
                  {"allocation_digest", p.allocation_digest.value},
                  {"worker_id", p.worker_id.value}};
    }
    case EventType::kWorkerLaunchObserved: {
      const auto& p = std::get<WorkerLaunchObservedPayload>(payload);
      return Json{{"operation_id", p.operation_id.value},
                  {"outcome", p.started ? "started" : "failed"}};
    }
    case EventType::kWorkerRunning:
      return Json{{"worker_id", std::get<WorkerRunningPayload>(payload).worker_id.value}};
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      return Json{{"principal_subject", std::get<PrincipalPayload>(payload).principal_subject}};
    case EventType::kTimeoutExpired: {
      const auto& p = std::get<TimeoutExpiredPayload>(payload);
      return Json{
          {"phase", p.phase == TimeoutPhase::kPreparation       ? "preparation"
                    : p.phase == TimeoutPhase::kExecution       ? "execution"
                    : p.phase == TimeoutPhase::kCooperativeStop ? "cooperative_stop"
                                                                : "process_exit_confirmation"},
          {"timer_generation", p.timer_generation}};
    }
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed: {
      const auto& p = std::get<WorkerEventPayload>(payload);
      return Json{{"worker_id", p.worker_id.value}, {"event_sequence", p.event_sequence}};
    }
    case EventType::kProcessExitConfirmed: {
      const auto& p = std::get<ProcessExitConfirmedPayload>(payload);
      return Json{{"completion_mode",
                   p.completion_mode == CompletionMode::kCooperative ? "cooperative"
                   : p.completion_mode == CompletionMode::kForced    ? "forced"
                                                                     : "process_already_exited"},
                  {"launch_operation_id", p.launch_operation_id.value}};
    }
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      return Json{{"session_id", std::get<SessionPayload>(payload).session_id.value}};
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      return Json::object();
    case EventType::kTerminalOutcomeCommitted: {
      const auto outcome = std::get<TerminalOutcomePayload>(payload).outcome;
      return Json{{"outcome", outcome == TerminalOutcome::kSucceeded    ? "succeeded"
                              : outcome == TerminalOutcome::kFailed     ? "failed"
                              : outcome == TerminalOutcome::kCancelled  ? "cancelled"
                              : outcome == TerminalOutcome::kTerminated ? "terminated"
                                                                        : "timed_out"}};
    }
    case EventType::kResourcesReleased: {
      const auto& p = std::get<ResourcesReleasedPayload>(payload);
      return Json{{"allocation_id", p.allocation_id.value},
                  {"allocation_digest", p.allocation_digest.value}};
    }
    case EventType::kCleanupStatusRecorded:
      return Json{
          {"status", std::get<CleanupStatusPayload>(payload).status == CleanupStatus::kCompleted
                         ? "completed"
                         : "incomplete"}};
    case EventType::kLateWorkerEvent: {
      const auto& p = std::get<LateWorkerEventPayload>(payload);
      return Json{{"original_event_type", ToString(p.original_event_type)},
                  {"worker_id", p.worker_id.value},
                  {"event_sequence", p.event_sequence}};
    }
    case EventType::kInvalid:
      return Json::object();
  }
  return Json::object();
}
std::optional<RejectionReason> RejectionOf(const NormalizedCandidate& d) {
  if (!std::holds_alternative<Rejection>(d.value)) return std::nullopt;
  return std::get<Rejection>(d.value).reason;
}
std::vector<std::string> EffectNames(const std::vector<Effect>& effects) {
  std::vector<std::string> result;
  for (const auto& effect : effects) result.emplace_back(ToString(effect.id));
  return result;
}
Json ProposalJson(const PreEnvelopeProposal& proposal) {
  return Json{{"schema_version", proposal.schema_version},
              {"job_id", proposal.job_id.value},
              {"event_type", ToString(proposal.event_type)},
              {"payload", PayloadJson(proposal.event_type, proposal.payload)}};
}
Json CandidateJson(const TimeoutCandidate& candidate) {
  return Json{{"schema_version", 1},
              {"job_id", candidate.job_id.value},
              {"event_type", "timeout_expired"},
              {"payload", PayloadJson(EventType::kTimeoutExpired, candidate.payload)}};
}
bool IsEvent(const Json& input, std::initializer_list<const char*> event_types) {
  if (!input.contains("event_type")) return false;
  const auto event_type = input.at("event_type").get<std::string>();
  return std::find_if(event_types.begin(), event_types.end(), [&](const char* candidate) {
           return event_type == candidate;
         }) != event_types.end();
}

bool IsTerminalName(const Json& snapshot) {
  if (snapshot.is_null() || !snapshot.is_object() || !snapshot.contains("state") ||
      !snapshot.at("state").is_string())
    return false;
  const auto state = snapshot.at("state").get<std::string>();
  return state == "succeeded" || state == "failed" || state == "cancelled" ||
         state == "terminated" || state == "timed_out";
}

bool HasState(const Json& snapshot, std::string_view state) {
  return !snapshot.is_null() && snapshot.is_object() && snapshot.contains("state") &&
         snapshot.at("state").is_string() && snapshot.at("state").get<std::string>() == state;
}

bool HasLatchedReason(const Json& snapshot) {
  return !snapshot.is_null() && snapshot.is_object() && snapshot.contains("latched_reason") &&
         !snapshot.at("latched_reason").is_null();
}

bool HasExitFact(const Json& snapshot) {
  return !snapshot.is_null() && snapshot.is_object() &&
         snapshot.contains("process_exit_confirmed") &&
         snapshot.at("process_exit_confirmed").is_boolean() &&
         snapshot.at("process_exit_confirmed").get<bool>();
}

bool HasFinalizationFact(const Json& vector) {
  const auto& initial = vector.at("initial_snapshot");
  const auto& next = vector.at("expected").at("next_snapshot");
  return HasState(initial, "finalizing") || HasState(next, "finalizing");
}

bool HasCleanupFact(const Json& vector) {
  const auto& input = vector.at("input");
  const auto& initial = vector.at("initial_snapshot");
  return HasState(initial, "finalizing") || IsTerminalName(initial) || HasExitFact(initial) ||
         IsEvent(input, {"process_exit_confirmed", "resources_released", "cleanup_status_recorded",
                         "late_worker_event"}) ||
         (IsEvent(input, {"worker_completed", "worker_failed"}) &&
          (IsTerminalName(initial) || HasExitFact(initial)));
}

bool HasFirstCauseFact(const Json& vector) {
  const auto& input = vector.at("input");
  const auto& initial = vector.at("initial_snapshot");
  const auto& next = vector.at("expected").at("next_snapshot");
  return input.contains("command_type") || HasLatchedReason(initial) || HasLatchedReason(next) ||
         IsEvent(input, {"cancel_accepted", "terminate_accepted", "worker_failed",
                         "timeout_expired", "terminal_outcome_committed"});
}

bool SelectedCase(const Json& vector, std::string_view selector) {
  const auto& input = vector.at("input");
  if (selector == "job_command_vectors") return vector.at("matrix") == "command";
  if (selector == "job_state_event_vectors") return vector.at("matrix") == "event";
  if (selector == "job_timeout_vectors") return IsEvent(input, {"timeout_expired"});
  if (selector == "job_first_cause_vectors") return HasFirstCauseFact(vector);
  if (selector == "job_finalization_vectors") return HasFinalizationFact(vector);
  if (selector == "job_late_cleanup_vectors") return HasCleanupFact(vector);
  if (selector == "job_rejected_input_no_append")
    return vector.at("expected").at("disposition") == "reject";
  return false;
}

InternalEvent InternalFromInput(const Json& input) {
  const auto type = input.at("event_type").get<std::string>();
  (void)ArtifactEventType(type);
  const auto job = U(input.at("job_id"));
  const auto& p = input.at("payload");
  if (type == "late_worker_event") {
    static constexpr std::array values{std::pair{"worker_completed"sv, EventType::kWorkerCompleted},
                                       std::pair{"worker_failed"sv, EventType::kWorkerFailed}};
    const auto original = ClosedEnum(p.at("original_event_type").get<std::string>(), values,
                                     "late_worker_event.original_event_type");
    return InternalEvent{input.at("schema_version").get<std::uint32_t>(), job,
                         EventType::kLateWorkerEvent,
                         LateWorkerEventPayload{original, U(p.at("worker_id")),
                                                p.at("event_sequence").get<std::uint64_t>()}};
  }
  if (type == "cancel_accepted" || type == "terminate_accepted") {
    return InternalEvent{
        input.at("schema_version").get<std::uint32_t>(), job,
        type == "cancel_accepted" ? EventType::kCancelAccepted : EventType::kTerminateAccepted,
        PrincipalPayload{p.at("principal_subject").get<std::string>()}};
  }
  return InternalEvent{input.at("schema_version").get<std::uint32_t>(), job,
                       EventType::kTimeoutExpired, EmptyPayload{}};
}

Decision DecisionFor(const Json& vector, const Snapshot& before) {
  const auto& in = vector.at("input");
  const auto matrix = vector.at("matrix").get<std::string>();
  if (matrix != "command" && matrix != "event")
    throw std::invalid_argument("unknown matrix: " + matrix);
  if (matrix == "command") {
    return DecideCommand(
        before, Command{in.at("schema_version").get<std::uint32_t>(),
                        ArtifactCommandType(in.at("command_type").get<std::string>()),
                        U(in.at("job_id")), in.at("principal_subject").get<std::string>()});
  }
  (void)ArtifactEventType(in.at("event_type").get<std::string>());
  if (in.at("event_type") == "late_worker_event") return DecideEvent(before, InternalFromInput(in));
  if (in.at("event_type") == "cancel_accepted" || in.at("event_type") == "terminate_accepted")
    return DecideEvent(before, InternalFromInput(in));
  const auto normalized = NormalizeCandidate(
      before, RawCandidateEvent{in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")),
                                in.at("event_type").get<std::string>(), in.at("payload").dump()});
  return std::holds_alternative<InternalEvent>(normalized.value)
             ? DecideEvent(before, std::get<InternalEvent>(normalized.value))
             : Decision{std::get<Rejection>(normalized.value)};
}

ArtifactDisposition ActualDisposition(const Json& input, const Snapshot& before,
                                      const Decision& decision, bool apply_command = false) {
  if (std::holds_alternative<Rejection>(decision.value)) return ArtifactDisposition::kReject;
  if (input.contains("command_type") && !apply_command) return ArtifactDisposition::kAudit;
  const auto& proposal = std::get<PreEnvelopeProposal>(decision.value);
  if (proposal.event_type == EventType::kLateWorkerEvent) return ArtifactDisposition::kLateAudit;
  const auto applied = Apply(before, proposal);
  if (applied.rejection)
    throw std::logic_error("accepted proposal rejected while classifying disposition");
  if (before.entity_exists != applied.snapshot.entity_exists ||
      before.state != applied.snapshot.state)
    return ArtifactDisposition::kTransition;
  return ArtifactDisposition::kAudit;
}

bool SelectedStep(const Json& step, const Snapshot& current, std::string_view selector) {
  const auto& input = step.at("input");
  const auto& expected = step.at("expected");
  if (!expected.is_object() || !expected.contains("disposition") ||
      !expected.contains("rejection_reason") || !expected.contains("journal_event") ||
      !expected.contains("post_sync_effects") || !expected.contains("next_snapshot") ||
      !expected.at("post_sync_effects").is_array())
    return false;
  const Json current_json = SnapshotJson(current);
  const Json& next = expected.at("next_snapshot");
  if (selector == "job_ordering_vectors") return true;
  if (selector == "job_timeout_vectors") return IsEvent(input, {"timeout_expired"});
  if (selector == "job_first_cause_vectors")
    return input.contains("command_type") || HasLatchedReason(current_json) ||
           HasLatchedReason(next) ||
           IsEvent(input, {"cancel_accepted", "terminate_accepted", "worker_failed",
                           "timeout_expired", "terminal_outcome_committed"});
  if (selector == "job_finalization_vectors")
    return HasState(current_json, "finalizing") || HasState(next, "finalizing");
  if (selector == "job_late_cleanup_vectors")
    return HasState(current_json, "finalizing") || IsTerminalName(current_json) ||
           HasExitFact(current_json) ||
           IsEvent(input, {"process_exit_confirmed", "resources_released",
                           "cleanup_status_recorded", "late_worker_event"}) ||
           (IsEvent(input, {"worker_completed", "worker_failed"}) &&
            (IsTerminalName(current_json) || HasExitFact(current_json)));
  if (selector == "job_rejected_input_no_append") return expected.at("disposition") == "reject";
  return false;
}

int CheckExpected(const Json& vector, const Snapshot& before, const Decision& decision) {
  const auto& expected = vector.at("expected");
  const auto id = vector.at("vector_id").get<std::string>();
  int result = 0;
  const bool reject = std::holds_alternative<Rejection>(decision.value);
  const auto has_expected_fields = [&] {
    return expected.is_object() && expected.contains("disposition") &&
           expected.contains("rejection_reason") && expected.contains("journal_event") &&
           expected.contains("post_sync_effects") && expected.contains("next_snapshot") &&
           expected.at("post_sync_effects").is_array();
  };
  result |= Check(has_expected_fields(), id + " expected result shape");
  if (!has_expected_fields()) return result;
  const auto expected_disposition = Disposition(expected.at("disposition").get<std::string>());
  result |= Check(ActualDisposition(vector.at("input"), before, decision) == expected_disposition,
                  id + " disposition");
  if (reject) {
    const auto actual_reason = ToString(std::get<Rejection>(decision.value).reason);
    const auto& expected_reason_value = expected.at("rejection_reason");
    result |= Check(expected_reason_value.is_null() || expected_reason_value.is_string(),
                    id + " rejection reason shape");
    if (!expected_reason_value.is_null() && !expected_reason_value.is_string()) return result;
    const auto expected_reason =
        expected_reason_value.is_null() ? "null" : expected_reason_value.get<std::string>();
    result |= Check(actual_reason == expected_reason,
                    id + " rejection reason (actual=" + std::string(actual_reason) +
                        ", expected=" + expected_reason + ")");
    result |=
        Check(expected.at("journal_event").is_null(), id + " rejected input has no Journal event");
    result |=
        Check(expected.at("post_sync_effects").empty(), id + " rejected input has no effects");
    if (expected.at("next_snapshot").is_null()) {
      result |= Check(!before.entity_exists, id + " rejected absent input has no snapshot update");
    } else {
      result |=
          Check(SnapshotJson(before) == expected.at("next_snapshot"),
                id + " rejected input preserves snapshot (actual=" + SnapshotJson(before).dump() +
                    ", expected=" + expected.at("next_snapshot").dump() + ")");
    }
    return result;
  }
  const auto& proposal = std::get<PreEnvelopeProposal>(decision.value);
  const auto& journal = expected.at("journal_event");
  const bool valid_journal = journal.is_object() && journal.contains("schema_version") &&
                             journal.contains("job_id") && journal.contains("event_type") &&
                             journal.contains("sequence") && journal.contains("payload");
  result |= Check(valid_journal, id + " accepted result journal shape");
  if (!valid_journal) return result;
  const Json expected_proposal{{"schema_version", journal.at("schema_version")},
                               {"job_id", journal.at("job_id")},
                               {"event_type", journal.at("event_type")},
                               {"payload", journal.at("payload")}};
  result |= Check(ProposalJson(proposal) == expected_proposal,
                  id + " complete pre-envelope proposal (actual=" + ProposalJson(proposal).dump() +
                      ", expected=" + expected_proposal.dump() + ")");
  if (vector.at("matrix") == "command") return result;
  const auto applied = Apply(before, proposal);
  result |= Check(!applied.rejection.has_value(), id + " apply accepted");
  result |= Check(SnapshotJson(applied.snapshot) == expected.at("next_snapshot"),
                  id + " snapshot (actual=" + SnapshotJson(applied.snapshot).dump() +
                      ", expected=" + expected.at("next_snapshot").dump() + ")");
  const auto actual_effects = EffectNames(applied.effects);
  const auto expected_effects = expected.at("post_sync_effects").get<std::vector<std::string>>();
  result |= Check(actual_effects == expected_effects,
                  id + " ordered post-sync effects (actual=" + Json(actual_effects).dump() +
                      ", expected=" + Json(expected_effects).dump() + ")");
  return result;
}
}  // namespace

int RunJobReducerVectorJsonChecks(const Json& vectors, const char* selector) {
  int result = 0;
  result |= Check(vectors.at("contract_version") == 1, "contract version is consumed explicitly");
  const auto& cases = vectors.at("case_vectors");
  const auto& invalid = vectors.at("invalid_payload_vectors");
  const auto& sequences = vectors.at("sequence_vectors");
  const auto& timers = vectors.at("timer_ingress_vectors");
  result |= Check(cases.size() == 324, "all case vectors are present");
  result |= Check(invalid.size() == 39, "all invalid-payload vectors are present");
  result |= Check(sequences.size() == 14, "all sequence vectors are present");
  result |= Check(timers.size() == 4, "all timer-ingress vectors are present");
  constexpr std::array<std::string_view, 9> selectors{
      "job_closed_state_set",     "job_state_event_vectors",  "job_command_vectors",
      "job_first_cause_vectors",  "job_finalization_vectors", "job_timeout_vectors",
      "job_late_cleanup_vectors", "job_ordering_vectors",     "job_rejected_input_no_append"};
  const std::string_view selected_selector = selector == nullptr ? "" : selector;
  if (selected_selector == "all") {
    for (const auto name : selectors) result |= RunJobReducerVectorJsonChecks(vectors, name.data());
    return result;
  }
  result |=
      Check(std::find(selectors.begin(), selectors.end(), selected_selector) != selectors.end(),
            "selector is addressable");
  if (selected_selector == "job_closed_state_set") {
    constexpr std::array<JobState, 10> states{
        JobState::kAdmitted,   JobState::kPreparing, JobState::kRunning, JobState::kStopping,
        JobState::kFinalizing, JobState::kSucceeded, JobState::kFailed,  JobState::kCancelled,
        JobState::kTerminated, JobState::kTimedOut};
    constexpr std::array<std::string_view, 10> names{
        "admitted",  "preparing", "running",   "stopping",   "finalizing",
        "succeeded", "failed",    "cancelled", "terminated", "timed_out"};
    for (std::size_t i = 0; i < states.size(); ++i) {
      result |= Check(ToString(states[i]) == names[i], "closed state spelling");
      result |= Check(IsTerminalState(states[i]) == (i >= 5), "terminal classification");
    }
    result |= Check(ToString(static_cast<JobState>(255)) == "invalid" &&
                        !IsTerminalState(static_cast<JobState>(255)),
                    "unknown state is safe");
    Snapshot unknown = InitialSnapshot(Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
                                       Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"});
    unknown.state = static_cast<JobState>(255);
    result |= Check(
        std::holds_alternative<Rejection>(
            DecideCommand(unknown, Command{1, CommandType::kCancel, unknown.job_id, "x"}).value),
        "unknown state rejects safely");
  }
  std::size_t selected_cases = 0;
  for (const auto& vector : cases) {
    if (!SelectedCase(vector, selected_selector)) continue;
    ++selected_cases;
    const auto& in = vector.at("input");
    const Snapshot before = SnapshotFrom(vector.at("initial_snapshot"), &in);
    const auto matrix = vector.at("matrix").get<std::string>();
    if (matrix != "command" && matrix != "event")
      throw std::invalid_argument("unknown matrix: " + matrix);
    Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
    if (matrix == "command") {
      const Command command{in.at("schema_version").get<std::uint32_t>(),
                            ArtifactCommandType(in.at("command_type").get<std::string>()),
                            U(in.at("job_id")), in.at("principal_subject").get<std::string>()};
      decision = DecideCommand(before, command);
    } else {
      const auto event_name = in.at("event_type").get<std::string>();
      (void)ArtifactEventType(event_name);
      if (event_name == "cancel_accepted" || event_name == "terminate_accepted") {
        const auto type = event_name == "cancel_accepted" ? EventType::kCancelAccepted
                                                          : EventType::kTerminateAccepted;
        const InternalEvent internal{
            in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")), type,
            PrincipalPayload{in.at("payload").at("principal_subject").get<std::string>()}};
        decision = DecideEvent(before, internal);
      } else if (event_name == "late_worker_event") {
        decision = DecideEvent(before, InternalFromInput(in));
      } else {
        const RawCandidateEvent candidate{in.at("schema_version").get<std::uint32_t>(),
                                          U(in.at("job_id")), event_name, in.at("payload").dump()};
        const auto normalized = NormalizeCandidate(before, candidate);
        decision = std::holds_alternative<InternalEvent>(normalized.value)
                       ? DecideEvent(before, std::get<InternalEvent>(normalized.value))
                       : Decision{std::get<Rejection>(normalized.value)};
      }
    }
    result |= CheckExpected(vector, before, decision);
  }
  if (selected_selector != "job_closed_state_set" && selected_selector != "job_ordering_vectors")
    result |= Check(selected_cases > 0, "selector consumes a non-empty case subset");
  if (selected_selector == "job_command_vectors")
    result |= Check(selected_cases == 24, "command selector consumes all cases");
  if (selected_selector == "job_state_event_vectors")
    result |= Check(selected_cases == 300, "event selector consumes all cases");
  if (selected_selector == "job_timeout_vectors")
    result |= Check(selected_cases == 30, "timeout selector case count");
  if (selected_selector == "job_rejected_input_no_append")
    result |= Check(selected_cases == 218, "rejected-input selector case count");
  if (selected_selector == "job_first_cause_vectors")
    result |= Check(selected_cases == 242, "first-cause selector case count");
  if (selected_selector == "job_finalization_vectors")
    result |= Check(selected_cases == 57, "finalization selector case count");
  if (selected_selector == "job_late_cleanup_vectors")
    result |= Check(selected_cases == 223, "late-cleanup selector case count");
  std::size_t selected_invalid = 0;
  if (selected_selector == "job_state_event_vectors" ||
      selected_selector == "job_rejected_input_no_append") {
    for (const auto& vector : invalid) {
      ++selected_invalid;
      const auto& in = vector.at("input");
      const Uuid job = U(in.at("job_id"));
      const std::uint32_t schema_version = in.contains("schema_version")
                                               ? in.at("schema_version").get<std::uint32_t>()
                                               : std::uint32_t{1};
      const auto normalized = NormalizeCandidate(
          InitialSnapshot(job, job),
          RawCandidateEvent{schema_version, job, in.at("event_type").get<std::string>(),
                            in.at("payload").dump()});
      result |= Check(RejectionOf(normalized).value_or(RejectionReason::kInvariantViolation) ==
                          RejectionReason::kInvalidEventPayload,
                      vector.at("vector_id").get<std::string>() + " invalid payload");
    }
    result |= Check(selected_invalid == 39, "selector consumes all invalid-payload vectors");
  }
  std::size_t selected_steps = 0;
  for (const auto& sequence : sequences) {
    Snapshot current = SnapshotFrom(sequence.at("initial_snapshot"));
    for (const auto& step : sequence.at("steps")) {
      const auto& in = step.at("input");
      Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
      if (in.contains("command_type")) {
        decision = DecideCommand(
            current, Command{in.at("schema_version").get<std::uint32_t>(),
                             ArtifactCommandType(in.at("command_type").get<std::string>()),
                             U(in.at("job_id")), in.at("principal_subject").get<std::string>()});
      } else if ((void)ArtifactEventType(in.at("event_type").get<std::string>()),
                 in.at("event_type") == "late_worker_event") {
        decision = DecideEvent(current, InternalFromInput(in));
      } else {
        const auto normalized = NormalizeCandidate(
            current,
            RawCandidateEvent{in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")),
                              in.at("event_type").get<std::string>(), in.at("payload").dump()});
        decision = std::holds_alternative<InternalEvent>(normalized.value)
                       ? DecideEvent(current, std::get<InternalEvent>(normalized.value))
                       : Decision{std::get<Rejection>(normalized.value)};
      }
      const auto& expected = step.at("expected");
      const bool expected_shape =
          expected.is_object() && expected.contains("disposition") &&
          expected.contains("rejection_reason") && expected.contains("journal_event") &&
          expected.contains("post_sync_effects") && expected.contains("next_snapshot") &&
          expected.at("post_sync_effects").is_array();
      if (!expected_shape)
        result |= Check(
            false, sequence.at("vector_id").get<std::string>() + " step expected result shape");
      const bool selected_step = SelectedStep(step, current, selected_selector);
      if (selected_step) {
        ++selected_steps;
        const bool rejected = std::holds_alternative<Rejection>(decision.value);
        const auto expected_disposition =
            Disposition(expected.at("disposition").get<std::string>());
        result |= Check(ActualDisposition(in, current, decision, true) == expected_disposition,
                        sequence.at("vector_id").get<std::string>() + " step disposition");
        if (rejected) {
          const auto id = sequence.at("vector_id").get<std::string>();
          const auto actual_reason = ToString(std::get<Rejection>(decision.value).reason);
          const auto& expected_reason_value = expected.at("rejection_reason");
          result |= Check(expected_reason_value.is_string(), id + " step rejection reason shape");
          if (!expected_reason_value.is_string()) continue;
          const auto expected_reason = expected_reason_value.get<std::string>();
          result |= Check(actual_reason == expected_reason,
                          id + " step rejection (actual=" + std::string(actual_reason) +
                              ", expected=" + expected_reason + ")");
          result |= Check(
              expected.at("journal_event").is_null() && expected.at("post_sync_effects").empty(),
              id + " rejected step no append/effects");
          if (expected.contains("next_snapshot") && expected.at("next_snapshot").is_null()) {
            result |=
                Check(!current.entity_exists, id + " rejected absent step no snapshot update");
          } else if (expected.contains("next_snapshot")) {
            result |= Check(
                SnapshotJson(current) == expected.at("next_snapshot"),
                id + " rejected step preserves snapshot (actual=" + SnapshotJson(current).dump() +
                    ", expected=" + expected.at("next_snapshot").dump() + ")");
          }
        }
      }
      if (std::holds_alternative<PreEnvelopeProposal>(decision.value)) {
        const auto proposal = std::get<PreEnvelopeProposal>(decision.value);
        const auto applied = Apply(current, proposal);
        if (selected_step) {
          const auto& journal = expected.at("journal_event");
          const bool valid_journal = journal.is_object() && journal.contains("schema_version") &&
                                     journal.contains("job_id") && journal.contains("event_type") &&
                                     journal.contains("sequence") && journal.contains("payload");
          result |= Check(valid_journal, sequence.at("vector_id").get<std::string>() +
                                             " step accepted result journal shape");
          if (!valid_journal) continue;
          const Json expected_proposal{{"schema_version", journal.at("schema_version")},
                                       {"job_id", journal.at("job_id")},
                                       {"event_type", journal.at("event_type")},
                                       {"payload", journal.at("payload")}};
          result |= Check(ProposalJson(proposal) == expected_proposal,
                          sequence.at("vector_id").get<std::string>() +
                              " step proposal (actual=" + ProposalJson(proposal).dump() +
                              ", expected=" + expected_proposal.dump() + ")");
          result |= Check(!applied.rejection.has_value(),
                          sequence.at("vector_id").get<std::string>() + " step applies");
          const auto actual_effects = EffectNames(applied.effects);
          const auto expected_effects =
              expected.at("post_sync_effects").get<std::vector<std::string>>();
          result |= Check(actual_effects == expected_effects,
                          sequence.at("vector_id").get<std::string>() +
                              " step effects (actual=" + Json(actual_effects).dump() +
                              ", expected=" + Json(expected_effects).dump() + ")");
        }
        if (!applied.rejection) current = applied.snapshot;
        if (selected_step && expected.contains("next_snapshot"))
          result |= Check(SnapshotJson(current) == expected.at("next_snapshot"),
                          sequence.at("vector_id").get<std::string>() + " step snapshot");
      }
    }
    if (selected_selector == "job_ordering_vectors")
      result |= Check(SnapshotJson(current) == sequence.at("expected_final_snapshot"),
                      sequence.at("vector_id").get<std::string>() + " final snapshot");
  }
  if (selected_selector == "job_ordering_vectors")
    result |= Check(sequences.size() == 14, "ordering selector consumes all sequences");
  if (selected_selector == "job_first_cause_vectors")
    result |= Check(selected_steps == 21, "first-cause selector sequence-step count");
  if (selected_selector == "job_finalization_vectors")
    result |= Check(selected_steps == 22, "finalization selector sequence-step count");
  if (selected_selector == "job_late_cleanup_vectors")
    result |= Check(selected_steps == 23, "late-cleanup selector sequence-step count");
  if (selected_selector == "job_timeout_vectors")
    result |= Check(selected_steps == 4, "timeout selector sequence-step count");
  if (selected_selector == "job_rejected_input_no_append")
    result |= Check(selected_steps == 4, "rejected-input selector sequence-step count");
  std::size_t selected_timers = 0;
  for (const auto& vector : timers) {
    if (selected_selector == "job_timeout_vectors") ++selected_timers;
    if (selected_selector != "job_timeout_vectors") continue;
    TimerState state;
    state.job_id = Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"};
    const auto active = vector.at("active_generation");
    state.execution_armed = !active.is_null();
    state.execution_generation = active.is_null() ? 0 : active.get<std::uint64_t>();
    const auto notification = vector.at("notification_generation");
    const auto arm = vector.at("arm_requested");
    const std::optional<TimerArmRequest> arm_request =
        arm.get<bool>()
            ? std::optional<TimerArmRequest>{TimerArmRequest{
                  Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"}, TimeoutPhase::kExecution,
                  active.is_null() ? 0 : active.get<std::uint64_t>()}}
            : std::nullopt;
    const std::optional<TimerNotification> timer_notification =
        notification.is_null() ? std::nullopt
                               : std::optional<TimerNotification>{TimerNotification{
                                     Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
                                     TimeoutPhase::kExecution, notification.get<std::uint64_t>()}};
    const auto ingress = IngestTimer(state, TimerIngressInput{arm_request, timer_notification});
    const auto& expected = vector.at("expected");
    const auto kind = ingress.kind == TimerIngressKind::kFailClosed ? "fail_closed"
                      : ingress.kind == TimerIngressKind::kDiscardWithoutCandidate
                          ? "discard_without_candidate"
                          : "emit_candidate_event";
    result |= Check(kind == expected.at("disposition").get<std::string>(),
                    vector.at("vector_id").get<std::string>() + " timer disposition");
    result |= Check(ingress.candidate.has_value() == !expected.at("candidate_event").is_null(),
                    vector.at("vector_id").get<std::string>() + " timer candidate");
    if (ingress.candidate && !expected.at("candidate_event").is_null())
      result |= Check(CandidateJson(*ingress.candidate) == expected.at("candidate_event"),
                      vector.at("vector_id").get<std::string>() + " timer typed candidate");
    result |= Check(EffectNames(ingress.effects) ==
                        expected.at("post_sync_effects").get<std::vector<std::string>>(),
                    vector.at("vector_id").get<std::string>() + " timer ordered effects");
  }
  if (selected_selector == "job_timeout_vectors")
    result |= Check(selected_timers == 4, "timeout selector consumes all timer-ingress vectors");
  return result;
}

int RunJobReducerVectorChecks(const char* path, const char* selector) {
  try {
    std::ifstream input(path);
    if (!input) return Check(false, "normative vector artifact is readable");
    const Json vectors = Json::parse(input, nullptr, true, false);
    return RunJobReducerVectorJsonChecks(vectors, selector);
  } catch (const std::exception& exception) {
    return Check(false, "normative vectors artifact error: " + std::string(exception.what()));
  }
}

int RunJobReducerVectorTextChecks(std::string_view text, const char* selector) {
  try {
    const Json vectors = Json::parse(text.begin(), text.end(), nullptr, true, false);
    return RunJobReducerVectorJsonChecks(vectors, selector);
  } catch (const std::exception& exception) {
    return Check(false, "normative vectors text error: " + std::string(exception.what()));
  }
}

int RunJobReducerParityMutationTextChecks(std::string_view text);

int RunJobReducerTrailingArtifactChecks(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return Check(false, "trailing-data source artifact is readable");
  std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  contents += "\ntrailing-garbage";
  int result = 0;
  result |= Check(RunJobReducerVectorTextChecks(contents, "all") != 0,
                  "normal runner rejects trailing artifact data");
  result |= Check(RunJobReducerParityMutationTextChecks(contents) != 0,
                  "mutation runner rejects trailing artifact data");
  return result;
}

int RunJobReducerClosedEnumArtifactChecks() {
  int result = 0;
  Json snapshot = SnapshotJson(InitialSnapshot(Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
                                               Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"}));
  snapshot["state"] = "not_a_state";
  bool state_rejected = false;
  try {
    (void)SnapshotFrom(snapshot);
  } catch (const std::invalid_argument&) {
    state_rejected = true;
  }
  result |= Check(state_rejected, "unknown snapshot state is rejected as an artifact error");
  bool command_rejected = false;
  try {
    (void)ArtifactCommandType("not_a_command");
  } catch (const std::invalid_argument&) {
    command_rejected = true;
  }
  result |= Check(command_rejected, "unknown command type is rejected as an artifact error");
  bool event_rejected = false;
  try {
    (void)ArtifactEventType("not_an_event");
  } catch (const std::invalid_argument&) {
    event_rejected = true;
  }
  result |= Check(event_rejected, "unknown event type is rejected as an artifact error");
  bool disposition_rejected = false;
  try {
    (void)Disposition("not_a_disposition");
  } catch (const std::invalid_argument&) {
    disposition_rejected = true;
  }
  result |= Check(disposition_rejected, "unknown disposition is rejected as an artifact error");
  return result;
}

int RunJobReducerParityMutationJsonChecks(const Json& vectors) {
  const auto& cases = vectors.at("case_vectors");
  const Json* accepted = nullptr;
  const Json* rejected = nullptr;
  const Json* timeout = nullptr;
  const Json* exit_confirmation = nullptr;
  for (const auto& vector : cases) {
    if (vector.at("vector_id") == "event__running__worker_completed__success_candidate")
      accepted = &vector;
    if (vector.at("vector_id") == "command__absent__cancel__rejected") rejected = &vector;
    if (!timeout && vector.at("input").contains("event_type") &&
        vector.at("input").at("event_type") == "timeout_expired" &&
        vector.at("expected").at("disposition") != "reject")
      timeout = &vector;
    if (vector.at("vector_id") == "event__finalizing__process_exit_confirmed__first_confirmation")
      exit_confirmation = &vector;
  }
  int result = 0;
  int mutation_classes = 0;
  const auto mutation_detected = [&](bool condition, std::string_view label) {
    ++mutation_classes;
    return Check(condition, std::string("mutation detected: ") + std::string(label));
  };
  result |=
      Check(accepted && rejected && timeout && exit_confirmation, "mutation probe source vectors");
  if (!accepted || !rejected || !timeout || !exit_confirmation) return result;
  const auto before = SnapshotFrom(accepted->at("initial_snapshot"));
  const auto decision = DecisionFor(*accepted, before);
  for (const auto& [label, key] : std::array<std::pair<std::string_view, std::string_view>, 2>{
           {{"disposition", "disposition"}, {"event payload", "payload"}}}) {
    Json mutated = *accepted;
    if (key == "disposition")
      mutated["expected"]["disposition"] = "audit";
    else
      mutated["expected"]["journal_event"]["payload"]["worker_id"] =
          "423e4567-e89b-42d3-a456-426614174000";
    result |= mutation_detected(CheckExpected(mutated, before, decision) != 0, label);
  }
  Json identity = *accepted;
  identity["expected"]["journal_event"]["job_id"] = "01890f3e-7b00-7abc-8abc-0123456789ac";
  result |= mutation_detected(CheckExpected(identity, before, decision) != 0, "identity binding");
  Json snapshot = *accepted;
  snapshot["expected"]["next_snapshot"]["pending_worker_event_ack"] = false;
  result |= mutation_detected(CheckExpected(snapshot, before, decision) != 0,
                              "pending-ACK clear invariant");
  Json effects = *accepted;
  effects["expected"]["post_sync_effects"] = {"ack_late_worker_event"};
  result |=
      mutation_detected(CheckExpected(effects, before, decision) != 0, "ordered post-sync effect");

  const auto timeout_before = SnapshotFrom(timeout->at("initial_snapshot"));
  const auto timeout_decision = DecisionFor(*timeout, timeout_before);
  Json generation = *timeout;
  generation["expected"]["journal_event"]["payload"]["timer_generation"] =
      std::numeric_limits<std::uint64_t>::max();
  result |= mutation_detected(CheckExpected(generation, timeout_before, timeout_decision) != 0,
                              "timer generation payload parity");
  Json rejection = *rejected;
  rejection["expected"]["rejection_reason"] = "invariant_violation";
  const auto rejection_before =
      SnapshotFrom(rejected->at("initial_snapshot"), &rejected->at("input"));
  result |= mutation_detected(
      CheckExpected(rejection, rejection_before, DecisionFor(*rejected, rejection_before)) != 0,
      "rejection");

  const auto exit_before = SnapshotFrom(exit_confirmation->at("initial_snapshot"));
  const auto exit_decision = DecisionFor(*exit_confirmation, exit_before);
  Json exit_snapshot = *exit_confirmation;
  exit_snapshot["expected"]["next_snapshot"]["pending_worker_event_ack"] = true;
  result |= mutation_detected(CheckExpected(exit_snapshot, exit_before, exit_decision) != 0,
                              "snapshot update");
  result |= Check(mutation_classes == 8, "all eight parity mutation classes exercised");
  return result;
}

int RunJobReducerParityMutationChecks(const char* path) {
  try {
    std::ifstream input(path);
    if (!input) return Check(false, "mutation vectors are readable");
    const Json vectors = Json::parse(input, nullptr, true, false);
    return RunJobReducerParityMutationJsonChecks(vectors);
  } catch (const std::exception& exception) {
    return Check(false, "mutation vectors artifact error: " + std::string(exception.what()));
  }
}

int RunJobReducerParityMutationTextChecks(std::string_view text) {
  try {
    const Json vectors = Json::parse(text.begin(), text.end(), nullptr, true, false);
    return RunJobReducerParityMutationJsonChecks(vectors);
  } catch (const std::exception& exception) {
    return Check(false, "mutation vectors text error: " + std::string(exception.what()));
  }
}
}  // namespace sitometron::test
