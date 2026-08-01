#include "sitometron/core/job_reducer.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>

namespace sitometron::test {
namespace {
using Json = nlohmann::json;
using namespace sitometron::core;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "job_reducer_vectors: " << message << '\n';
  return 1;
}

Uuid U(const Json& j) { return Uuid{j.get<std::string>()}; }
std::optional<StableId> Stable(const Json& j) { return j.is_null() ? std::nullopt : std::optional<StableId>{StableId{j.get<std::string>()}}; }
std::optional<Digest> DigestValue(const Json& j) { return j.is_null() ? std::nullopt : std::optional<Digest>{Digest{j.get<std::string>()}}; }
std::optional<Uuid> UuidValue(const Json& j) { return j.is_null() ? std::nullopt : std::optional<Uuid>{U(j)}; }

JobState State(std::string_view value) {
  if (value == "preparing") return JobState::kPreparing;
  if (value == "running") return JobState::kRunning;
  if (value == "stopping") return JobState::kStopping;
  if (value == "finalizing") return JobState::kFinalizing;
  if (value == "succeeded") return JobState::kSucceeded;
  if (value == "failed") return JobState::kFailed;
  if (value == "cancelled") return JobState::kCancelled;
  if (value == "terminated") return JobState::kTerminated;
  return JobState::kTimedOut;
}
std::optional<TerminalOutcome> Outcome(const Json& j) {
  if (j.is_null()) return std::nullopt;
  const auto s = j.get<std::string>();
  if (s == "failed") return TerminalOutcome::kFailed;
  if (s == "cancelled") return TerminalOutcome::kCancelled;
  if (s == "terminated") return TerminalOutcome::kTerminated;
  return TerminalOutcome::kTimedOut;
}
Snapshot SnapshotFrom(const Json& j) {
  if (j.is_null()) return InitialSnapshot(Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"}, Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"});
  Snapshot s;
  s.entity_exists = true;
  s.schema_version = j.at("schema_version").get<std::uint32_t>();
  s.job_id = U(j.at("job_id")); s.session_id = U(j.at("session_id")); s.state = State(j.at("state").get<std::string>());
  s.latched_reason = Outcome(j.at("latched_reason"));
  s.completion_candidate = !j.at("completion_candidate").is_null();
  if (!j.at("completion_mode").is_null()) {
    const auto v = j.at("completion_mode").get<std::string>();
    s.completion_mode = v == "cooperative" ? CompletionMode::kCooperative : v == "forced" ? CompletionMode::kForced : CompletionMode::kProcessAlreadyExited;
  }
  const auto resource = j.at("resource_status").get<std::string>();
  s.resource_status = resource == "committed" ? ResourceStatus::kCommitted : resource == "released" ? ResourceStatus::kReleased : ResourceStatus::kNone;
  s.allocation_id = Stable(j.at("allocation_id")); s.allocation_digest = DigestValue(j.at("allocation_digest"));
  const auto launch = j.at("worker_launch_status").get<std::string>();
  s.worker_launch_status = launch == "intent_recorded" ? LaunchStatus::kIntentRecorded : launch == "observed" ? LaunchStatus::kObserved : launch == "failed" ? LaunchStatus::kFailed : LaunchStatus::kNotStarted;
  s.launch_operation_id = Stable(j.at("launch_operation_id")); s.worker_id = UuidValue(j.at("worker_id"));
  const auto presence = j.at("process_presence").get<std::string>();
  s.process_presence = presence == "present" ? ProcessPresence::kPresent : presence == "unknown" ? ProcessPresence::kUnknown : ProcessPresence::kAbsent;
  s.process_exit_confirmed = j.at("process_exit_confirmed").get<bool>();
  const auto retention = j.at("session_retention_status").get<std::string>();
  s.session_retention_status = retention == "requested" ? RetentionStatus::kRequested : retention == "retained" ? RetentionStatus::kRetained : RetentionStatus::kNotStarted;
  const auto finalization = j.at("finalization_status").get<std::string>();
  s.finalization_status = finalization == "pending" ? FinalizationStatus::kPending : finalization == "completed" ? FinalizationStatus::kCompleted : finalization == "failed" ? FinalizationStatus::kFailed : FinalizationStatus::kNotStarted;
  const auto cleanup = j.at("cleanup_status").get<std::string>();
  s.cleanup_status = cleanup == "completed" ? CleanupStatus::kCompleted : cleanup == "incomplete" ? CleanupStatus::kIncomplete : CleanupStatus::kPending;
  s.pending_worker_event_ack = j.at("pending_worker_event_ack").get<bool>();
  s.pending_worker_id = UuidValue(j.at("pending_worker_id"));
  if (!j.at("pending_worker_event_sequence").is_null()) s.pending_worker_event_sequence = j.at("pending_worker_event_sequence").get<std::uint64_t>();
  return s;
}
Json SnapshotJson(const Snapshot& s) {
  Json j{{"schema_version", s.schema_version}, {"job_id", s.job_id.value}, {"session_id", s.session_id.value},
         {"state", std::string(ToString(s.state))}, {"latched_reason", nullptr}, {"completion_candidate", s.completion_candidate ? Json("succeeded") : Json(nullptr)},
         {"completion_mode", nullptr}, {"resource_status", "none"}, {"allocation_id", nullptr}, {"allocation_digest", nullptr},
         {"worker_launch_status", "not_started"}, {"launch_operation_id", nullptr}, {"worker_id", nullptr}, {"process_presence", "absent"},
         {"process_exit_confirmed", s.process_exit_confirmed}, {"session_retention_status", "not_started"}, {"finalization_status", "not_started"},
         {"cleanup_status", "pending"}, {"pending_worker_event_ack", s.pending_worker_event_ack}, {"pending_worker_id", nullptr}, {"pending_worker_event_sequence", nullptr}};
  if (s.latched_reason) j["latched_reason"] = *s.latched_reason == TerminalOutcome::kFailed ? "failed" : *s.latched_reason == TerminalOutcome::kCancelled ? "cancelled" : *s.latched_reason == TerminalOutcome::kTerminated ? "terminated" : "timed_out";
  if (s.completion_mode != CompletionMode::kNone) j["completion_mode"] = s.completion_mode == CompletionMode::kCooperative ? "cooperative" : s.completion_mode == CompletionMode::kForced ? "forced" : "process_already_exited";
  j["resource_status"] = s.resource_status == ResourceStatus::kCommitted ? "committed" : s.resource_status == ResourceStatus::kReleased ? "released" : "none";
  if (s.allocation_id) j["allocation_id"] = s.allocation_id->value;
  if (s.allocation_digest) j["allocation_digest"] = s.allocation_digest->value;
  j["worker_launch_status"] = s.worker_launch_status == LaunchStatus::kIntentRecorded ? "intent_recorded" : s.worker_launch_status == LaunchStatus::kObserved ? "observed" : s.worker_launch_status == LaunchStatus::kFailed ? "failed" : "not_started";
  if (s.launch_operation_id) j["launch_operation_id"] = s.launch_operation_id->value;
  if (s.worker_id) j["worker_id"] = s.worker_id->value;
  j["process_presence"] = s.process_presence == ProcessPresence::kPresent ? "present" : s.process_presence == ProcessPresence::kUnknown ? "unknown" : "absent";
  j["session_retention_status"] = s.session_retention_status == RetentionStatus::kRequested ? "requested" : s.session_retention_status == RetentionStatus::kRetained ? "retained" : "not_started";
  j["finalization_status"] = s.finalization_status == FinalizationStatus::kPending ? "pending" : s.finalization_status == FinalizationStatus::kCompleted ? "completed" : s.finalization_status == FinalizationStatus::kFailed ? "failed" : "not_started";
  j["cleanup_status"] = s.cleanup_status == CleanupStatus::kCompleted ? "completed" : s.cleanup_status == CleanupStatus::kIncomplete ? "incomplete" : "pending";
  if (s.pending_worker_id) j["pending_worker_id"] = s.pending_worker_id->value;
  if (s.pending_worker_event_sequence) j["pending_worker_event_sequence"] = *s.pending_worker_event_sequence;
  return j;
}

Json PayloadJson(EventType type, const EventPayload& payload) {
  switch (type) {
    case EventType::kJobCreated: return Json{{"session_id", std::get<JobCreatedPayload>(payload).session_id.value}};
    case EventType::kResourcesCommitted: { const auto& p = std::get<ResourcesCommittedPayload>(payload); return Json{{"allocation_id", p.allocation_id.value}, {"allocation_digest", p.allocation_digest.value}, {"resolved_allocation", {{"schema_id", p.schema_id.value}, {"schema_version", p.schema_version}, {"payload_utf8", p.payload_utf8}}}}; }
    case EventType::kWorkerLaunchIntent: { const auto& p = std::get<WorkerLaunchIntentPayload>(payload); return Json{{"operation_id", p.operation_id.value}, {"application", {{"application_id", p.application_id.value}, {"version", p.application_version}, {"bundle_sha256", p.bundle_sha256.value}}}, {"allocation_id", p.allocation_id.value}, {"allocation_digest", p.allocation_digest.value}, {"worker_id", p.worker_id.value}}; }
    case EventType::kWorkerLaunchObserved: { const auto& p = std::get<WorkerLaunchObservedPayload>(payload); return Json{{"operation_id", p.operation_id.value}, {"outcome", p.started ? "started" : "failed"}}; }
    case EventType::kWorkerRunning: return Json{{"worker_id", std::get<WorkerRunningPayload>(payload).worker_id.value}};
    case EventType::kCancelAccepted: case EventType::kTerminateAccepted: return Json{{"principal_subject", std::get<PrincipalPayload>(payload).principal_subject}};
    case EventType::kTimeoutExpired: { const auto& p = std::get<TimeoutExpiredPayload>(payload); return Json{{"phase", p.phase == TimeoutPhase::kPreparation ? "preparation" : p.phase == TimeoutPhase::kExecution ? "execution" : p.phase == TimeoutPhase::kCooperativeStop ? "cooperative_stop" : "process_exit_confirmation"}, {"timer_generation", p.timer_generation}}; }
    case EventType::kWorkerCompleted: case EventType::kWorkerFailed: { const auto& p = std::get<WorkerEventPayload>(payload); return Json{{"worker_id", p.worker_id.value}, {"event_sequence", p.event_sequence}}; }
    case EventType::kProcessExitConfirmed: { const auto& p = std::get<ProcessExitConfirmedPayload>(payload); return Json{{"completion_mode", p.completion_mode == CompletionMode::kCooperative ? "cooperative" : p.completion_mode == CompletionMode::kForced ? "forced" : "process_already_exited"}, {"launch_operation_id", p.launch_operation_id.value}}; }
    case EventType::kSessionRetainRequested: case EventType::kSessionRetained: return Json{{"session_id", std::get<SessionPayload>(payload).session_id.value}};
    case EventType::kFinalizationCompleted: case EventType::kFinalizationFailed: return Json::object();
    case EventType::kTerminalOutcomeCommitted: { const auto outcome = std::get<TerminalOutcomePayload>(payload).outcome; return Json{{"outcome", outcome == TerminalOutcome::kSucceeded ? "succeeded" : outcome == TerminalOutcome::kFailed ? "failed" : outcome == TerminalOutcome::kCancelled ? "cancelled" : outcome == TerminalOutcome::kTerminated ? "terminated" : "timed_out"}}; }
    case EventType::kResourcesReleased: { const auto& p = std::get<ResourcesReleasedPayload>(payload); return Json{{"allocation_id", p.allocation_id.value}, {"allocation_digest", p.allocation_digest.value}}; }
    case EventType::kCleanupStatusRecorded: return Json{{"status", std::get<CleanupStatusPayload>(payload).status == CleanupStatus::kCompleted ? "completed" : "incomplete"}};
    case EventType::kLateWorkerEvent: { const auto& p = std::get<LateWorkerEventPayload>(payload); return Json{{"original_event_type", ToString(p.original_event_type)}, {"worker_id", p.worker_id.value}, {"event_sequence", p.event_sequence}}; }
  }
  return Json::object();
}
std::optional<RejectionReason> RejectionOf(const NormalizedCandidate& d) {
  if (!std::holds_alternative<Rejection>(d.value)) return std::nullopt;
  return std::get<Rejection>(d.value).reason;
}
int CheckExpected(const Json& vector, const Snapshot& before, const Decision& decision) {
  const auto& expected = vector.at("expected"); int result = 0;
  const bool reject = std::holds_alternative<Rejection>(decision.value);
  result |= Check((expected.at("disposition") == "reject") == reject, vector.at("vector_id").get<std::string>() + " disposition");
  if (reject && !expected.at("rejection_reason").is_null()) result |= Check(ToString(std::get<Rejection>(decision.value).reason) == expected.at("rejection_reason").get<std::string>(), vector.at("vector_id").get<std::string>() + " rejection reason");
  if (!reject) {
    const auto& proposal = std::get<PreEnvelopeProposal>(decision.value);
    if (!expected.at("journal_event").is_null()) {
      result |= Check(expected.at("journal_event").at("event_type").get<std::string>() == std::string(ToString(proposal.event_type)), vector.at("vector_id").get<std::string>() + " pre-envelope event type");
      result |= Check(expected.at("journal_event").at("payload") == PayloadJson(proposal.event_type, proposal.payload), vector.at("vector_id").get<std::string>() + " pre-envelope payload");
    }
    if (vector.at("matrix") == "command") return result;

    const auto applied = Apply(before, proposal);
    result |= Check(!applied.rejection.has_value(), vector.at("vector_id").get<std::string>() + " apply accepted");
    if (!expected.at("next_snapshot").is_null()) result |= Check(SnapshotJson(applied.snapshot) == expected.at("next_snapshot"), vector.at("vector_id").get<std::string>() + " snapshot");
  }
  return result;
}
}  // namespace

int RunJobReducerVectorChecks(const char* path, const char* selector) {
  std::ifstream input(path); if (!input) return Check(false, "normative vector artifact is readable");
  Json vectors; try { input >> vectors; } catch (const Json::exception&) { return Check(false, "normative vectors parse"); }
  int result = 0;
  result |= Check(vectors.at("contract_version") == 1, "contract version is consumed explicitly");
  const auto& cases = vectors.at("case_vectors"); const auto& invalid = vectors.at("invalid_payload_vectors"); const auto& sequences = vectors.at("sequence_vectors"); const auto& timers = vectors.at("timer_ingress_vectors");
  result |= Check(cases.size() == 324, "all case vectors are present"); result |= Check(invalid.size() == 39, "all invalid-payload vectors are present"); result |= Check(sequences.size() == 14, "all sequence vectors are present"); result |= Check(timers.size() == 4, "all timer-ingress vectors are present");
  constexpr std::array<std::string_view, 9> selectors{"job_closed_state_set", "job_state_event_vectors", "job_command_vectors", "job_first_cause_vectors", "job_finalization_vectors", "job_timeout_vectors", "job_late_cleanup_vectors", "job_ordering_vectors", "job_rejected_input_no_append"};
  result |= Check(selector != nullptr && std::find(selectors.begin(), selectors.end(), selector) != selectors.end(), "selector is addressable");
  for (const auto& vector : cases) {
    const Snapshot before = SnapshotFrom(vector.at("initial_snapshot")); const auto& in = vector.at("input");
    Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
    if (vector.at("matrix") == "command") {
      const Command command{in.at("schema_version").get<std::uint32_t>(), in.at("command_type").get<std::string>() == "cancel" ? CommandType::kCancel : CommandType::kTerminate, U(in.at("job_id")), in.at("principal_subject").get<std::string>()};
      decision = DecideCommand(before, command);
    } else {
      const auto event_name = in.at("event_type").get<std::string>();
      if (event_name == "cancel_accepted" || event_name == "terminate_accepted") {
        const auto type = event_name == "cancel_accepted" ? EventType::kCancelAccepted : EventType::kTerminateAccepted;
        const InternalEvent internal{in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")), type, PrincipalPayload{in.at("payload").at("principal_subject").get<std::string>()}};
        decision = DecideEvent(before, internal);
      } else {
        const RawCandidateEvent candidate{in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")), event_name, in.at("payload").dump()};
        const auto normalized = NormalizeCandidate(before, candidate);
        decision = std::holds_alternative<InternalEvent>(normalized.value) ? DecideEvent(before, std::get<InternalEvent>(normalized.value)) : Decision{std::get<Rejection>(normalized.value)};
      }
    }
    result |= CheckExpected(vector, before, decision);
  }
  for (const auto& vector : invalid) {
    const auto& in = vector.at("input"); const Uuid job = U(in.at("job_id"));
    const std::uint32_t schema_version = in.contains("schema_version") ? in.at("schema_version").get<std::uint32_t>() : std::uint32_t{1};
    const auto normalized = NormalizeCandidate(InitialSnapshot(job, job), RawCandidateEvent{schema_version, job, in.at("event_type").get<std::string>(), in.at("payload").dump()});
    result |= Check(RejectionOf(normalized).value_or(RejectionReason::kInvariantViolation) == RejectionReason::kInvalidEventPayload, vector.at("vector_id").get<std::string>() + " invalid payload");
  }
  for (const auto& sequence : sequences) {
    Snapshot current = SnapshotFrom(sequence.at("initial_snapshot"));
    for (const auto& step : sequence.at("steps")) {
      const auto& in = step.at("input"); Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
      if (in.contains("command_type")) {
        decision = DecideCommand(current, Command{in.at("schema_version").get<std::uint32_t>(), in.at("command_type").get<std::string>() == "cancel" ? CommandType::kCancel : CommandType::kTerminate, U(in.at("job_id")), in.at("principal_subject").get<std::string>()});
      } else {
        const auto normalized = NormalizeCandidate(current, RawCandidateEvent{in.at("schema_version").get<std::uint32_t>(), U(in.at("job_id")), in.at("event_type").get<std::string>(), in.at("payload").dump()});
        decision = std::holds_alternative<InternalEvent>(normalized.value) ? DecideEvent(current, std::get<InternalEvent>(normalized.value)) : Decision{std::get<Rejection>(normalized.value)};
      }
      const auto& expected = step.at("expected");
      result |= Check(!std::holds_alternative<Rejection>(decision.value) && expected.at("disposition") != "reject", sequence.at("vector_id").get<std::string>() + " step accepted");
      if (std::holds_alternative<PreEnvelopeProposal>(decision.value)) {
        const auto applied = Apply(current, std::get<PreEnvelopeProposal>(decision.value));
        result |= Check(!applied.rejection.has_value(), sequence.at("vector_id").get<std::string>() + " step applies");
        current = applied.snapshot;
        if (expected.contains("next_snapshot")) result |= Check(SnapshotJson(current) == expected.at("next_snapshot"), sequence.at("vector_id").get<std::string>() + " step snapshot");
      }
    }
  }
  for (const auto& vector : timers) {
    TimerState state; const auto active = vector.at("active_generation");
    state.execution_armed = vector.at("arm_requested").get<bool>();
    state.execution_generation = active.is_null() ? 0 : active.get<std::uint64_t>();
    const auto notification = vector.at("notification_generation");
    const auto ingress = IngestTimer(state, TimerNotification{Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"}, TimeoutPhase::kExecution, notification.is_null() ? 0 : notification.get<std::uint64_t>()});
    const auto& expected = vector.at("expected");
    const auto kind = ingress.kind == TimerIngressKind::kFailClosed ? "fail_closed" : ingress.kind == TimerIngressKind::kDiscardWithoutCandidate ? "discard_without_candidate" : "emit_candidate_event";
    result |= Check(kind == expected.at("disposition").get<std::string>(), vector.at("vector_id").get<std::string>() + " timer disposition");
    result |= Check(ingress.candidate.has_value() == !expected.at("candidate_event").is_null(), vector.at("vector_id").get<std::string>() + " timer candidate");
    result |= Check(ingress.effects.size() == expected.at("post_sync_effects").size(), vector.at("vector_id").get<std::string>() + " timer effects");
  }
  return result;
}
}  // namespace sitometron::test
