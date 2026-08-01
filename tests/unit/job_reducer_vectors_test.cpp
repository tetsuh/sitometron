#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

#include "sitometron/core/job_reducer.hpp"

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
std::optional<StableId> Stable(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<StableId>{StableId{j.get<std::string>()}};
}
std::optional<Digest> DigestValue(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<Digest>{Digest{j.get<std::string>()}};
}
std::optional<Uuid> UuidValue(const Json& j) {
  return j.is_null() ? std::nullopt : std::optional<Uuid>{U(j)};
}

JobState State(std::string_view value) {
  if (value == "admitted") return JobState::kAdmitted;
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
  s.entity_exists = true;
  s.schema_version = j.at("schema_version").get<std::uint32_t>();
  s.job_id = U(j.at("job_id"));
  s.session_id = U(j.at("session_id"));
  s.state = State(j.at("state").get<std::string>());
  s.latched_reason = Outcome(j.at("latched_reason"));
  s.completion_candidate = !j.at("completion_candidate").is_null();
  if (!j.at("completion_mode").is_null()) {
    const auto v = j.at("completion_mode").get<std::string>();
    s.completion_mode = v == "cooperative" ? CompletionMode::kCooperative
                        : v == "forced"    ? CompletionMode::kForced
                                           : CompletionMode::kProcessAlreadyExited;
  }
  const auto resource = j.at("resource_status").get<std::string>();
  s.resource_status = resource == "committed"  ? ResourceStatus::kCommitted
                      : resource == "released" ? ResourceStatus::kReleased
                                               : ResourceStatus::kNone;
  s.allocation_id = Stable(j.at("allocation_id"));
  s.allocation_digest = DigestValue(j.at("allocation_digest"));
  const auto launch = j.at("worker_launch_status").get<std::string>();
  s.worker_launch_status = launch == "intent_recorded" ? LaunchStatus::kIntentRecorded
                           : launch == "observed"      ? LaunchStatus::kObserved
                           : launch == "failed"        ? LaunchStatus::kFailed
                                                       : LaunchStatus::kNotStarted;
  s.launch_operation_id = Stable(j.at("launch_operation_id"));
  s.worker_id = UuidValue(j.at("worker_id"));
  const auto presence = j.at("process_presence").get<std::string>();
  s.process_presence = presence == "present"   ? ProcessPresence::kPresent
                       : presence == "unknown" ? ProcessPresence::kUnknown
                                               : ProcessPresence::kAbsent;
  s.process_exit_confirmed = j.at("process_exit_confirmed").get<bool>();
  const auto retention = j.at("session_retention_status").get<std::string>();
  s.session_retention_status = retention == "requested"  ? RetentionStatus::kRequested
                               : retention == "retained" ? RetentionStatus::kRetained
                                                         : RetentionStatus::kNotStarted;
  const auto finalization = j.at("finalization_status").get<std::string>();
  s.finalization_status = finalization == "pending"     ? FinalizationStatus::kPending
                          : finalization == "completed" ? FinalizationStatus::kCompleted
                          : finalization == "failed"    ? FinalizationStatus::kFailed
                                                        : FinalizationStatus::kNotStarted;
  const auto cleanup = j.at("cleanup_status").get<std::string>();
  s.cleanup_status = cleanup == "completed"    ? CleanupStatus::kCompleted
                     : cleanup == "incomplete" ? CleanupStatus::kIncomplete
                                               : CleanupStatus::kPending;
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

bool HasFinalizationFact(const Json& vector) {
  const auto& initial = vector.at("initial_snapshot");
  const auto& expected = vector.at("expected");
  return (!initial.is_null() && initial.at("state") == "finalizing") ||
         IsEvent(vector.at("input"), {"session_retain_requested", "session_retained",
                                      "finalization_completed", "finalization_failed"}) ||
         (!expected.at("next_snapshot").is_null() &&
          expected.at("next_snapshot").at("state") == "finalizing");
}

bool HasCleanupFact(const Json& vector) {
  const auto& initial = vector.at("initial_snapshot");
  return IsEvent(vector.at("input"), {"process_exit_confirmed", "resources_released",
                                      "cleanup_status_recorded", "late_worker_event"}) ||
         (!initial.is_null() &&
          (initial.at("state") == "finalizing" || initial.at("state") == "succeeded" ||
           initial.at("state") == "failed" || initial.at("state") == "cancelled" ||
           initial.at("state") == "terminated" || initial.at("state") == "timed_out"));
}

bool HasFirstCauseFact(const Json& vector) {
  const auto& initial = vector.at("initial_snapshot");
  const auto& expected = vector.at("expected");
  return (!initial.is_null() && !initial.at("latched_reason").is_null()) ||
         IsEvent(vector.at("input"), {"cancel_accepted", "terminate_accepted", "worker_failed",
                                      "timeout_expired", "terminal_outcome_committed"}) ||
         (!expected.at("next_snapshot").is_null() &&
          !expected.at("next_snapshot").at("latched_reason").is_null());
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
  const auto job = U(input.at("job_id"));
  const auto& p = input.at("payload");
  if (type == "late_worker_event") {
    const auto original = p.at("original_event_type").get<std::string>() == "worker_completed"
                              ? EventType::kWorkerCompleted
                              : EventType::kWorkerFailed;
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
  if (vector.at("matrix") == "command") {
    return DecideCommand(
        before,
        Command{in.at("schema_version").get<std::uint32_t>(),
                in.at("command_type").get<std::string>() == "cancel" ? CommandType::kCancel
                                                                     : CommandType::kTerminate,
                U(in.at("job_id")), in.at("principal_subject").get<std::string>()});
  }
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

bool SelectedStep(const Json& step, std::string_view selector) {
  const auto& input = step.at("input");
  if (selector == "job_ordering_vectors") return true;
  if (selector == "job_timeout_vectors") return IsEvent(input, {"timeout_expired"});
  if (selector == "job_first_cause_vectors") {
    return IsEvent(input, {"cancel_accepted", "terminate_accepted", "worker_failed",
                           "timeout_expired", "terminal_outcome_committed"});
  }
  if (selector == "job_finalization_vectors") {
    return input.contains("event_type") &&
           IsEvent(input, {"session_retain_requested", "session_retained", "finalization_completed",
                           "finalization_failed"});
  }
  if (selector == "job_late_cleanup_vectors") {
    return IsEvent(input, {"process_exit_confirmed", "resources_released",
                           "cleanup_status_recorded", "late_worker_event"});
  }
  if (selector == "job_rejected_input_no_append")
    return step.at("expected").at("disposition") == "reject";
  return false;
}

int CheckExpected(const Json& vector, const Snapshot& before, const Decision& decision) {
  const auto& expected = vector.at("expected");
  const auto id = vector.at("vector_id").get<std::string>();
  int result = 0;
  const bool reject = std::holds_alternative<Rejection>(decision.value);
  result |= Check((expected.at("disposition") == "reject") == reject, id + " disposition");
  if (reject) {
    const auto actual_reason = ToString(std::get<Rejection>(decision.value).reason);
    const auto expected_reason = expected.at("rejection_reason").is_null()
                                     ? "null"
                                     : expected.at("rejection_reason").get<std::string>();
    result |= Check(actual_reason == expected_reason,
                    id + " rejection reason (actual=" + std::string(actual_reason) +
                        ", expected=" + expected_reason + ")");
    result |=
        Check(expected.at("journal_event").is_null() && expected.at("post_sync_effects").empty(),
              id + " rejected input has no append/effects");
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
  const Json expected_proposal{{"schema_version", journal.at("schema_version")},
                               {"job_id", journal.at("job_id")},
                               {"event_type", journal.at("event_type")},
                               {"payload", journal.at("payload")}};
  result |= Check(!journal.is_null() && ProposalJson(proposal) == expected_proposal,
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

int RunJobReducerVectorChecks(const char* path, const char* selector) {
  std::ifstream input(path);
  if (!input) return Check(false, "normative vector artifact is readable");
  Json vectors;
  try {
    input >> vectors;
  } catch (const Json::exception&) {
    return Check(false, "normative vectors parse");
  }
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
    for (const auto name : selectors) result |= RunJobReducerVectorChecks(path, name.data());
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
    Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
    if (vector.at("matrix") == "command") {
      const Command command{in.at("schema_version").get<std::uint32_t>(),
                            in.at("command_type").get<std::string>() == "cancel"
                                ? CommandType::kCancel
                                : CommandType::kTerminate,
                            U(in.at("job_id")), in.at("principal_subject").get<std::string>()};
      decision = DecideCommand(before, command);
    } else {
      const auto event_name = in.at("event_type").get<std::string>();
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
  if (selected_selector == "job_state_event_vectors" ||
      selected_selector == "job_rejected_input_no_append") {
    for (const auto& vector : invalid) {
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
  }
  for (const auto& sequence : sequences) {
    Snapshot current = SnapshotFrom(sequence.at("initial_snapshot"));
    for (const auto& step : sequence.at("steps")) {
      const auto& in = step.at("input");
      Decision decision{Rejection{1, RejectionReason::kInvalidEventPayload}};
      if (in.contains("command_type")) {
        decision = DecideCommand(
            current,
            Command{in.at("schema_version").get<std::uint32_t>(),
                    in.at("command_type").get<std::string>() == "cancel" ? CommandType::kCancel
                                                                         : CommandType::kTerminate,
                    U(in.at("job_id")), in.at("principal_subject").get<std::string>()});
      } else if (in.at("event_type") == "late_worker_event") {
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
      const bool selected_step = SelectedStep(step, selected_selector);
      if (selected_step) {
        const bool rejected = std::holds_alternative<Rejection>(decision.value);
        result |= Check(rejected == (expected.at("disposition") == "reject"),
                        sequence.at("vector_id").get<std::string>() + " step disposition");
        if (rejected) {
          const auto id = sequence.at("vector_id").get<std::string>();
          const auto actual_reason = ToString(std::get<Rejection>(decision.value).reason);
          const auto expected_reason = expected.at("rejection_reason").get<std::string>();
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
          const Json expected_proposal{{"schema_version", journal.at("schema_version")},
                                       {"job_id", journal.at("job_id")},
                                       {"event_type", journal.at("event_type")},
                                       {"payload", journal.at("payload")}};
          result |= Check(!journal.is_null() && ProposalJson(proposal) == expected_proposal,
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
  for (const auto& vector : timers) {
    if (selected_selector != "job_timeout_vectors") continue;
    TimerState state;
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
  return result;
}

int RunJobReducerParityMutationChecks(const char* path) {
  std::ifstream input(path);
  if (!input) return Check(false, "mutation vectors are readable");
  Json vectors;
  input >> vectors;
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
  result |=
      Check(accepted && rejected && timeout && exit_confirmation, "mutation probe source vectors");
  if (!accepted || !rejected || !timeout || !exit_confirmation) return result;
  const auto before = SnapshotFrom(accepted->at("initial_snapshot"));
  const auto decision = DecisionFor(*accepted, before);
  for (const auto& [label, key] : std::array<std::pair<std::string_view, std::string_view>, 2>{
           {{"disposition", "disposition"}, {"event payload", "payload"}}}) {
    Json mutated = *accepted;
    if (key == "disposition")
      mutated["expected"]["disposition"] = "reject";
    else
      mutated["expected"]["journal_event"]["payload"]["worker_id"] =
          "423e4567-e89b-42d3-a456-426614174000";
    result |= Check(CheckExpected(mutated, before, decision) != 0,
                    std::string("mutation detected: ") + std::string(label));
  }
  Json identity = *accepted;
  identity["expected"]["journal_event"]["job_id"] = "01890f3e-7b00-7abc-8abc-0123456789ac";
  result |=
      Check(CheckExpected(identity, before, decision) != 0, "mutation detected: identity binding");
  Json snapshot = *accepted;
  snapshot["expected"]["next_snapshot"]["pending_worker_event_ack"] = false;
  result |= Check(CheckExpected(snapshot, before, decision) != 0,
                  "mutation detected: pending-ACK update");
  Json effects = *accepted;
  effects["expected"]["post_sync_effects"] = {"ack_late_worker_event"};
  result |= Check(CheckExpected(effects, before, decision) != 0,
                  "mutation detected: ordered post-sync effect");

  const auto timeout_before = SnapshotFrom(timeout->at("initial_snapshot"));
  const auto timeout_decision = DecisionFor(*timeout, timeout_before);
  Json generation = *timeout;
  generation["expected"]["journal_event"]["payload"]["timer_generation"] =
      std::numeric_limits<std::uint64_t>::max();
  result |= Check(CheckExpected(generation, timeout_before, timeout_decision) != 0,
                  "mutation detected: uint64 bound");
  Json rejection = *rejected;
  rejection["expected"]["rejection_reason"] = "invariant_violation";
  const auto rejection_before =
      SnapshotFrom(rejected->at("initial_snapshot"), &rejected->at("input"));
  result |= Check(
      CheckExpected(rejection, rejection_before, DecisionFor(*rejected, rejection_before)) != 0,
      "mutation detected: rejection");

  const auto exit_before = SnapshotFrom(exit_confirmation->at("initial_snapshot"));
  const auto exit_decision = DecisionFor(*exit_confirmation, exit_before);
  Json exit_snapshot = *exit_confirmation;
  exit_snapshot["expected"]["next_snapshot"]["pending_worker_event_ack"] = true;
  result |= Check(CheckExpected(exit_snapshot, exit_before, exit_decision) != 0,
                  "mutation detected: snapshot update");
  return result;
}
}  // namespace sitometron::test
