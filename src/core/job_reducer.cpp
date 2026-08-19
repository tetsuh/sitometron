#include "sitometron/core/job_reducer.hpp"

#include <algorithm>
#include <array>
#include <boost/hash2/sha2.hpp>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace sitometron::core {
namespace {
using Json = nlohmann::json;

Decision Reject(RejectionReason reason) { return Decision{Rejection{1, reason}}; }
Decision Accept(const Snapshot& snapshot, EventType type, EventPayload payload) {
  return Decision{PreEnvelopeProposal{1, snapshot.job_id, type, std::move(payload)}};
}
NormalizedCandidate Normalize(const Snapshot& snapshot, EventType type, EventPayload payload) {
  return NormalizedCandidate{InternalEvent{1, snapshot.job_id, type, std::move(payload)}};
}
NormalizedCandidate RejectCandidate(RejectionReason reason) {
  return NormalizedCandidate{Rejection{1, reason}};
}
bool IsTerminal(JobState state) {
  return state == JobState::kSucceeded || state == JobState::kFailed ||
         state == JobState::kCancelled || state == JobState::kTerminated ||
         state == JobState::kTimedOut;
}
bool IsAsciiDigit(char value) { return value >= '0' && value <= '9'; }
bool IsAsciiUpper(char value) { return value >= 'A' && value <= 'Z'; }
bool IsAsciiLower(char value) { return value >= 'a' && value <= 'z'; }
bool IsAsciiAlphanumeric(char value) {
  return IsAsciiDigit(value) || IsAsciiUpper(value) || IsAsciiLower(value);
}
bool IsLowerHex(char value) { return IsAsciiDigit(value) || (value >= 'a' && value <= 'f'); }
bool IsUuid(std::string_view s, char version) {
  if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!IsLowerHex(s[i])) return false;
  }
  return s[14] == version && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}
bool IsStable(std::string_view s) {
  if (s.empty() || s.size() > 128 || !IsAsciiAlphanumeric(s.front())) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return IsAsciiAlphanumeric(c) || c == '.' || c == '_' || c == ':' || c == '-';
  });
}
struct Utf8Sequence {
  std::size_t length;
  int second_min;
  int second_max;
};

std::optional<Utf8Sequence> Utf8SequenceForLead(unsigned char lead) {
  if (lead <= 0x7F) return Utf8Sequence{1, 0x80, 0xBF};
  if (lead >= 0xC2 && lead <= 0xDF) return Utf8Sequence{2, 0x80, 0xBF};
  if (lead >= 0xE0 && lead <= 0xEF)
    return Utf8Sequence{3, lead == 0xE0 ? 0xA0 : 0x80, lead == 0xED ? 0x9F : 0xBF};
  if (lead >= 0xF0 && lead <= 0xF4)
    return Utf8Sequence{4, lead == 0xF0 ? 0x90 : 0x80, lead == 0xF4 ? 0x8F : 0xBF};
  return std::nullopt;
}

bool HasValidUtf8Continuation(std::string_view value, std::size_t index,
                              const Utf8Sequence& sequence) {
  if (index + sequence.length > value.size()) return false;
  if (sequence.length == 1) return true;
  const auto continuation = value.substr(index + 2, sequence.length - 2);
  if (const auto second = static_cast<unsigned char>(value[index + 1]);
      second < sequence.second_min || second > sequence.second_max)
    return false;
  return std::ranges::all_of(continuation, [](char byte) {
    const auto value = static_cast<unsigned char>(byte);
    return value >= 0x80 && value <= 0xBF;
  });
}

bool IsUtf8ScalarString(std::string_view value, std::size_t max_scalars) {
  if (value.empty()) return false;
  std::size_t scalars = 0;
  for (std::size_t index = 0; index < value.size();) {
    const auto sequence = Utf8SequenceForLead(static_cast<unsigned char>(value[index]));
    if (!sequence || !HasValidUtf8Continuation(value, index, *sequence)) return false;
    index += sequence->length;
    if (++scalars > max_scalars) return false;
  }
  return true;
}
bool IsPrincipal(std::string_view s) { return IsUtf8ScalarString(s, 256); }
bool IsCommandType(CommandType type) {
  return static_cast<int>(type) >= 0 && static_cast<int>(type) <= 1;
}
bool IsEventType(EventType type) {
  return static_cast<int>(type) >= 0 && static_cast<int>(type) <= 18;
}
bool IsTimeoutPhase(TimeoutPhase phase) {
  return static_cast<int>(phase) >= 0 && static_cast<int>(phase) <= 3;
}
bool IsTerminalOutcome(TerminalOutcome outcome) {
  return static_cast<int>(outcome) >= 0 && static_cast<int>(outcome) <= 4;
}
bool IsCompletionMode(CompletionMode mode) {
  return static_cast<int>(mode) >= 0 && static_cast<int>(mode) <= 3;
}
bool IsResourceStatus(ResourceStatus status) {
  return static_cast<int>(status) >= 0 && static_cast<int>(status) <= 2;
}
bool IsLaunchStatus(LaunchStatus status) {
  return static_cast<int>(status) >= 0 && static_cast<int>(status) <= 3;
}
bool IsProcessPresence(ProcessPresence presence) {
  return static_cast<int>(presence) >= 0 && static_cast<int>(presence) <= 2;
}
bool IsRetentionStatus(RetentionStatus status) {
  return static_cast<int>(status) >= 0 && static_cast<int>(status) <= 2;
}
bool IsFinalizationStatus(FinalizationStatus status) {
  return static_cast<int>(status) >= 0 && static_cast<int>(status) <= 3;
}
bool IsCleanupStatus(CleanupStatus status) {
  return static_cast<int>(status) >= 0 && static_cast<int>(status) <= 2;
}

bool IsDigest(std::string_view s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool HasOnly(const Json& object, std::initializer_list<const char*> fields) {
  if (!object.is_object()) return false;
  for (const auto& item : object.items()) {
    if (std::find_if(fields.begin(), fields.end(),
                     [&](const char* f) { return item.key() == f; }) == fields.end())
      return false;
  }
  return true;
}
bool StringField(const Json& o, const char* key, std::string* value) {
  if (!o.contains(key) || !o.at(key).is_string()) return false;
  *value = o.at(key).get<std::string>();
  return true;
}
bool UInt64Field(const Json& o, const char* key, std::uint64_t* value) {
  if (!o.contains(key) || !o.at(key).is_number_unsigned()) return false;
  *value = o.at(key).get<std::uint64_t>();
  return *value != 0;
}
bool ParseJson(std::string_view text, Json* result) {
  if (std::find(text.begin(), text.end(), '\0') != text.end()) return false;
  try {
    *result = Json::parse(text.begin(), text.end(), nullptr, true, false);
    return true;
  } catch (const Json::exception&) {
    return false;
  }
}
bool ParseResolvedAllocationJson(std::string_view text, Json* result) {
  return text.size() <= 65536 && ParseJson(text, result);
}
std::string Sha256(std::string_view bytes) {
  boost::hash2::sha2_256 hasher;
  hasher.update(bytes.data(), bytes.size());
  return boost::hash2::to_string(hasher.result());
}
std::optional<TimeoutPhase> ParsePhase(std::string_view value) {
  if (value == "preparation") return TimeoutPhase::kPreparation;
  if (value == "execution") return TimeoutPhase::kExecution;
  if (value == "cooperative_stop") return TimeoutPhase::kCooperativeStop;
  if (value == "process_exit_confirmation") return TimeoutPhase::kProcessExitConfirmation;
  return std::nullopt;
}
std::optional<CompletionMode> ParseMode(std::string_view value) {
  if (value == "cooperative") return CompletionMode::kCooperative;
  if (value == "forced") return CompletionMode::kForced;
  if (value == "process_already_exited") return CompletionMode::kProcessAlreadyExited;
  return std::nullopt;
}
std::optional<TerminalOutcome> ParseOutcome(std::string_view value) {
  if (value == "succeeded") return TerminalOutcome::kSucceeded;
  if (value == "failed") return TerminalOutcome::kFailed;
  if (value == "cancelled") return TerminalOutcome::kCancelled;
  if (value == "terminated") return TerminalOutcome::kTerminated;
  if (value == "timed_out") return TerminalOutcome::kTimedOut;
  return std::nullopt;
}
std::optional<EventType> ParseEvent(std::string_view value) {
#define EVENT(name, text) \
  if (value == (text)) return EventType::k##name;
  EVENT(JobCreated, "job_created")
  EVENT(ResourcesCommitted, "resources_committed")
  EVENT(WorkerLaunchIntent, "worker_launch_intent")
  EVENT(WorkerLaunchObserved, "worker_launch_observed")
  EVENT(WorkerRunning, "worker_running")
  EVENT(CancelAccepted, "cancel_accepted")
  EVENT(TerminateAccepted, "terminate_accepted")
  EVENT(TimeoutExpired, "timeout_expired")
  EVENT(WorkerCompleted, "worker_completed")
  EVENT(WorkerFailed, "worker_failed")
  EVENT(ProcessExitConfirmed, "process_exit_confirmed")
  EVENT(SessionRetainRequested, "session_retain_requested")
  EVENT(SessionRetained, "session_retained")
  EVENT(FinalizationCompleted, "finalization_completed")
  EVENT(FinalizationFailed, "finalization_failed")
  EVENT(TerminalOutcomeCommitted, "terminal_outcome_committed")
  EVENT(ResourcesReleased, "resources_released")
  EVENT(CleanupStatusRecorded, "cleanup_status_recorded")
  EVENT(LateWorkerEvent, "late_worker_event")
#undef EVENT
  return std::nullopt;
}
std::optional<EventPayload> ParseJobCreatedPayload(const Json& payload,
                                                   const Uuid& candidate_job_id) {
  std::string session_id;
  if (!HasOnly(payload, {"session_id"}) || !StringField(payload, "session_id", &session_id) ||
      !IsUuid(session_id, '7') || session_id != candidate_job_id.value)
    return std::nullopt;
  return JobCreatedPayload{Uuid{session_id}};
}

std::optional<EventPayload> ParseResourcesCommittedPayload(const Json& payload) {
  std::string allocation_id;
  std::string allocation_digest;
  if (!HasOnly(payload, {"allocation_id", "allocation_digest", "resolved_allocation"}) ||
      !StringField(payload, "allocation_id", &allocation_id) || !IsStable(allocation_id) ||
      !StringField(payload, "allocation_digest", &allocation_digest) ||
      !IsDigest(allocation_digest) || !payload.contains("resolved_allocation") ||
      !payload.at("resolved_allocation").is_object())
    return std::nullopt;

  const auto& resolved = payload.at("resolved_allocation");
  std::string schema_id;
  std::string payload_utf8;
  std::uint64_t schema_version = 0;
  Json parsed_payload;
  if (!HasOnly(resolved, {"schema_id", "schema_version", "payload_utf8"}) ||
      !StringField(resolved, "schema_id", &schema_id) || !IsStable(schema_id) ||
      !resolved.contains("schema_version") || !resolved.contains("payload_utf8") ||
      !resolved.at("schema_version").is_number_unsigned())
    return std::nullopt;
  schema_version = resolved.at("schema_version").get<std::uint64_t>();
  if (schema_version == 0 || schema_version > UINT32_MAX ||
      !StringField(resolved, "payload_utf8", &payload_utf8) ||
      !ParseResolvedAllocationJson(payload_utf8, &parsed_payload) ||
      Sha256(payload_utf8) != allocation_digest)
    return std::nullopt;

  return ResourcesCommittedPayload{StableId{allocation_id}, Digest{allocation_digest},
                                   StableId{schema_id}, static_cast<std::uint32_t>(schema_version),
                                   payload_utf8};
}

std::optional<EventPayload> ParseWorkerLaunchIntentPayload(const Json& payload) {
  std::string operation_id;
  std::string application_id;
  std::string application_version;
  std::string bundle_sha256;
  std::string allocation_id;
  std::string allocation_digest;
  std::string worker_id;
  if (!HasOnly(payload, {"operation_id", "application", "allocation_id", "allocation_digest",
                         "worker_id"}) ||
      !StringField(payload, "operation_id", &operation_id) || !IsStable(operation_id) ||
      !payload.contains("application") || !payload.at("application").is_object())
    return std::nullopt;

  if (const auto& application = payload.at("application");
      !HasOnly(application, {"application_id", "version", "bundle_sha256"}) ||
      !StringField(application, "application_id", &application_id) || !IsStable(application_id) ||
      !StringField(application, "version", &application_version) ||
      !IsUtf8ScalarString(application_version, 128) ||
      !StringField(application, "bundle_sha256", &bundle_sha256) || !IsDigest(bundle_sha256) ||
      !StringField(payload, "allocation_id", &allocation_id) || !IsStable(allocation_id) ||
      !StringField(payload, "allocation_digest", &allocation_digest) ||
      !IsDigest(allocation_digest) || !StringField(payload, "worker_id", &worker_id) ||
      !IsUuid(worker_id, '4'))
    return std::nullopt;

  return WorkerLaunchIntentPayload{StableId{operation_id},  StableId{application_id},
                                   application_version,     Digest{bundle_sha256},
                                   StableId{allocation_id}, Digest{allocation_digest},
                                   Uuid{worker_id}};
}

std::optional<EventPayload> ParseWorkerLaunchObservedPayload(const Json& payload) {
  std::string operation_id;
  std::string outcome;
  if (!HasOnly(payload, {"operation_id", "outcome"}) ||
      !StringField(payload, "operation_id", &operation_id) || !IsStable(operation_id) ||
      !StringField(payload, "outcome", &outcome) || (outcome != "started" && outcome != "failed"))
    return std::nullopt;
  return WorkerLaunchObservedPayload{StableId{operation_id}, outcome == "started"};
}

std::optional<EventPayload> ParseWorkerRunningPayload(const Json& payload) {
  std::string worker_id;
  if (!HasOnly(payload, {"worker_id"}) || !StringField(payload, "worker_id", &worker_id) ||
      !IsUuid(worker_id, '4'))
    return std::nullopt;
  return WorkerRunningPayload{Uuid{worker_id}};
}

std::optional<EventPayload> ParseTimeoutExpiredPayload(const Json& payload) {
  std::string phase_text;
  std::uint64_t generation = 0;
  if (!HasOnly(payload, {"phase", "timer_generation"}) ||
      !StringField(payload, "phase", &phase_text) ||
      !UInt64Field(payload, "timer_generation", &generation))
    return std::nullopt;
  const auto phase = ParsePhase(phase_text);
  if (!phase) return std::nullopt;
  return TimeoutExpiredPayload{*phase, generation};
}

std::optional<EventPayload> ParseWorkerEventPayload(const Json& payload) {
  std::string worker_id;
  std::uint64_t sequence = 0;
  if (!HasOnly(payload, {"worker_id", "event_sequence"}) ||
      !StringField(payload, "worker_id", &worker_id) || !IsUuid(worker_id, '4') ||
      !UInt64Field(payload, "event_sequence", &sequence))
    return std::nullopt;
  return WorkerEventPayload{Uuid{worker_id}, sequence};
}

std::optional<EventPayload> ParseProcessExitConfirmedPayload(const Json& payload) {
  std::string mode_text;
  std::string operation_id;
  if (!HasOnly(payload, {"completion_mode", "launch_operation_id"}) ||
      !StringField(payload, "completion_mode", &mode_text) ||
      !StringField(payload, "launch_operation_id", &operation_id) || !IsStable(operation_id))
    return std::nullopt;
  const auto mode = ParseMode(mode_text);
  if (!mode) return std::nullopt;
  return ProcessExitConfirmedPayload{*mode, StableId{operation_id}};
}

std::optional<EventPayload> ParseSessionPayload(const Json& payload) {
  std::string session_id;
  if (!HasOnly(payload, {"session_id"}) || !StringField(payload, "session_id", &session_id) ||
      !IsUuid(session_id, '7'))
    return std::nullopt;
  return SessionPayload{Uuid{session_id}};
}

std::optional<EventPayload> ParseEmptyPayload(const Json& payload) {
  return HasOnly(payload, {}) ? std::optional<EventPayload>{EmptyPayload{}} : std::nullopt;
}

std::optional<EventPayload> ParseTerminalOutcomePayload(const Json& payload) {
  std::string outcome_text;
  if (!HasOnly(payload, {"outcome"}) || !StringField(payload, "outcome", &outcome_text))
    return std::nullopt;
  const auto outcome = ParseOutcome(outcome_text);
  if (!outcome) return std::nullopt;
  return TerminalOutcomePayload{*outcome};
}

std::optional<EventPayload> ParseResourcesReleasedPayload(const Json& payload) {
  std::string allocation_id;
  std::string allocation_digest;
  if (!HasOnly(payload, {"allocation_id", "allocation_digest"}) ||
      !StringField(payload, "allocation_id", &allocation_id) || !IsStable(allocation_id) ||
      !StringField(payload, "allocation_digest", &allocation_digest) ||
      !IsDigest(allocation_digest))
    return std::nullopt;
  return ResourcesReleasedPayload{StableId{allocation_id}, Digest{allocation_digest}};
}

std::optional<EventPayload> ParseCleanupStatusPayload(const Json& payload) {
  std::string status;
  if (!HasOnly(payload, {"status"}) || !StringField(payload, "status", &status) ||
      (status != "completed" && status != "incomplete"))
    return std::nullopt;
  return CleanupStatusPayload{status == "completed" ? CleanupStatus::kCompleted
                                                    : CleanupStatus::kIncomplete};
}

std::optional<EventPayload> ParseCandidatePayload(EventType type, const Json& payload,
                                                  const Uuid& candidate_job_id) {
  switch (type) {
    case EventType::kJobCreated:
      return ParseJobCreatedPayload(payload, candidate_job_id);
    case EventType::kResourcesCommitted:
      return ParseResourcesCommittedPayload(payload);
    case EventType::kWorkerLaunchIntent:
      return ParseWorkerLaunchIntentPayload(payload);
    case EventType::kWorkerLaunchObserved:
      return ParseWorkerLaunchObservedPayload(payload);
    case EventType::kWorkerRunning:
      return ParseWorkerRunningPayload(payload);
    case EventType::kTimeoutExpired:
      return ParseTimeoutExpiredPayload(payload);
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed:
      return ParseWorkerEventPayload(payload);
    case EventType::kProcessExitConfirmed:
      return ParseProcessExitConfirmedPayload(payload);
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      return ParseSessionPayload(payload);
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      return ParseEmptyPayload(payload);
    case EventType::kTerminalOutcomeCommitted:
      return ParseTerminalOutcomePayload(payload);
    case EventType::kResourcesReleased:
      return ParseResourcesReleasedPayload(payload);
    case EventType::kCleanupStatusRecorded:
      return ParseCleanupStatusPayload(payload);
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
    case EventType::kLateWorkerEvent:
    case EventType::kInvalid:
      return std::nullopt;
  }
  return std::nullopt;
}

NormalizedCandidate ParseCandidate(const Snapshot& snapshot, const RawCandidateEvent& candidate) {
  using enum RejectionReason;

  try {
    if (candidate.schema_version != 1 || candidate.job_id != snapshot.job_id ||
        !IsUuid(candidate.job_id.value, '7'))
      return RejectCandidate(kInvariantViolation);
    const auto type = ParseEvent(candidate.event_type);
    if (!type || *type == EventType::kCancelAccepted || *type == EventType::kTerminateAccepted)
      return RejectCandidate(kInvalidEventPayload);
    Json payload;
    if (!ParseJson(candidate.payload_json, &payload)) return RejectCandidate(kInvalidEventPayload);
    auto parsed = ParseCandidatePayload(*type, payload, candidate.job_id);
    return parsed ? Normalize(snapshot, *type, std::move(*parsed))
                  : RejectCandidate(kInvalidEventPayload);
  } catch (const Json::exception&) {
    return RejectCandidate(kInvalidEventPayload);
  }
}
bool PayloadMatches(EventType type, const EventPayload& payload);
bool ValidInternalPayload(EventType type, const EventPayload& payload, const Uuid& job_id) {
  if (!IsEventType(type) || !PayloadMatches(type, payload)) return false;
  if (const auto* p = std::get_if<JobCreatedPayload>(&payload))
    return IsUuid(p->session_id.value, '7') && p->session_id == job_id;
  if (const auto* p = std::get_if<ResourcesCommittedPayload>(&payload)) {
    Json parsed;
    return IsStable(p->allocation_id.value) && IsDigest(p->allocation_digest.value) &&
           IsStable(p->schema_id.value) && p->schema_version != 0 &&
           ParseResolvedAllocationJson(p->payload_utf8, &parsed) &&
           Sha256(p->payload_utf8) == p->allocation_digest.value;
  }
  if (const auto* p = std::get_if<WorkerLaunchIntentPayload>(&payload))
    return IsStable(p->operation_id.value) && IsStable(p->application_id.value) &&
           IsUtf8ScalarString(p->application_version, 128) && IsDigest(p->bundle_sha256.value) &&
           IsStable(p->allocation_id.value) && IsDigest(p->allocation_digest.value) &&
           IsUuid(p->worker_id.value, '4');
  if (const auto* p = std::get_if<WorkerLaunchObservedPayload>(&payload))
    return IsStable(p->operation_id.value);
  if (const auto* p = std::get_if<WorkerRunningPayload>(&payload))
    return IsUuid(p->worker_id.value, '4');
  if (const auto* p = std::get_if<PrincipalPayload>(&payload))
    return IsPrincipal(p->principal_subject);
  if (const auto* p = std::get_if<TimeoutExpiredPayload>(&payload))
    return IsTimeoutPhase(p->phase) && p->timer_generation != 0;
  if (const auto* p = std::get_if<WorkerEventPayload>(&payload))
    return IsUuid(p->worker_id.value, '4') && p->event_sequence != 0;
  if (const auto* p = std::get_if<ProcessExitConfirmedPayload>(&payload))
    return IsCompletionMode(p->completion_mode) && p->completion_mode != CompletionMode::kNone &&
           IsStable(p->launch_operation_id.value);
  if (const auto* p = std::get_if<SessionPayload>(&payload))
    return IsUuid(p->session_id.value, '7');
  if (const auto* p = std::get_if<TerminalOutcomePayload>(&payload))
    return IsTerminalOutcome(p->outcome);
  if (const auto* p = std::get_if<ResourcesReleasedPayload>(&payload))
    return IsStable(p->allocation_id.value) && IsDigest(p->allocation_digest.value);
  if (const auto* p = std::get_if<CleanupStatusPayload>(&payload))
    return IsCleanupStatus(p->status) && p->status != CleanupStatus::kPending;
  if (const auto* p = std::get_if<LateWorkerEventPayload>(&payload))
    return (p->original_event_type == EventType::kWorkerCompleted ||
            p->original_event_type == EventType::kWorkerFailed) &&
           IsUuid(p->worker_id.value, '4') && p->event_sequence != 0;
  return std::holds_alternative<EmptyPayload>(payload);
}
bool HasValidSnapshotIdentity(const Snapshot& snapshot) {
  return snapshot.schema_version == 1 && IsUuid(snapshot.job_id.value, '7') &&
         IsUuid(snapshot.session_id.value, '7') && snapshot.job_id == snapshot.session_id;
}

bool HasValidSnapshotEnums(const Snapshot& snapshot) {
  return static_cast<int>(snapshot.state) >= 0 && static_cast<int>(snapshot.state) <= 9 &&
         IsCompletionMode(snapshot.completion_mode) && IsResourceStatus(snapshot.resource_status) &&
         IsLaunchStatus(snapshot.worker_launch_status) &&
         IsProcessPresence(snapshot.process_presence) &&
         IsRetentionStatus(snapshot.session_retention_status) &&
         IsFinalizationStatus(snapshot.finalization_status) &&
         IsCleanupStatus(snapshot.cleanup_status);
}

bool HasValidSnapshotOptionalDomains(const Snapshot& snapshot) {
  if (snapshot.latched_reason.has_value() &&
      (!IsTerminalOutcome(*snapshot.latched_reason) ||
       *snapshot.latched_reason == TerminalOutcome::kSucceeded))
    return false;
  if (snapshot.allocation_id.has_value() && !IsStable(snapshot.allocation_id->value)) return false;
  if (snapshot.allocation_digest.has_value() && !IsDigest(snapshot.allocation_digest->value))
    return false;
  if (snapshot.launch_operation_id.has_value() && !IsStable(snapshot.launch_operation_id->value))
    return false;
  if (snapshot.worker_id.has_value() && !IsUuid(snapshot.worker_id->value, '4')) return false;
  if (snapshot.pending_worker_id.has_value() && !IsUuid(snapshot.pending_worker_id->value, '4'))
    return false;
  return !snapshot.pending_worker_event_sequence.has_value() ||
         *snapshot.pending_worker_event_sequence != 0;
}

bool HasValidResourceFacts(const Snapshot& snapshot) {
  if (snapshot.resource_status == ResourceStatus::kNone)
    return !snapshot.allocation_id && !snapshot.allocation_digest;
  if (!snapshot.allocation_id || !snapshot.allocation_digest) return false;
  return snapshot.resource_status != ResourceStatus::kReleased || snapshot.process_exit_confirmed;
}

bool HasValidExitFacts(const Snapshot& snapshot) {
  if (snapshot.process_exit_confirmed &&
      (snapshot.process_presence != ProcessPresence::kAbsent || snapshot.pending_worker_event_ack ||
       snapshot.pending_worker_id.has_value() ||
       snapshot.pending_worker_event_sequence.has_value()))
    return false;
  return snapshot.completion_mode != CompletionMode::kProcessAlreadyExited ||
         snapshot.process_exit_confirmed;
}

bool HasValidLaunchFacts(const Snapshot& snapshot) {
  if (snapshot.worker_launch_status == LaunchStatus::kNotStarted)
    return !snapshot.launch_operation_id && !snapshot.worker_id;
  return snapshot.launch_operation_id && snapshot.worker_id &&
         IsUuid(snapshot.worker_id->value, '4');
}

bool HasValidPendingAckFacts(const Snapshot& snapshot) {
  if (!snapshot.pending_worker_event_ack)
    return !snapshot.pending_worker_id.has_value() &&
           !snapshot.pending_worker_event_sequence.has_value();
  return snapshot.pending_worker_id.has_value() &&
         snapshot.pending_worker_event_sequence.has_value() &&
         *snapshot.pending_worker_event_sequence != 0;
}

bool HasValidStateFacts(const Snapshot& snapshot) {
  if (snapshot.state == JobState::kAdmitted &&
      (snapshot.resource_status != ResourceStatus::kNone ||
       snapshot.worker_launch_status != LaunchStatus::kNotStarted ||
       snapshot.process_presence != ProcessPresence::kAbsent || snapshot.process_exit_confirmed))
    return false;
  if ((snapshot.state == JobState::kAdmitted || snapshot.state == JobState::kPreparing ||
       snapshot.state == JobState::kRunning || snapshot.state == JobState::kStopping) &&
      snapshot.finalization_status != FinalizationStatus::kNotStarted)
    return false;
  if (snapshot.state == JobState::kFinalizing &&
      snapshot.finalization_status == FinalizationStatus::kNotStarted)
    return false;
  return !IsTerminal(snapshot.state) ||
         snapshot.finalization_status == FinalizationStatus::kCompleted ||
         snapshot.finalization_status == FinalizationStatus::kFailed;
}

bool ValidSnapshot(const Snapshot& snapshot) {
  return HasValidSnapshotIdentity(snapshot) && HasValidSnapshotEnums(snapshot) &&
         HasValidSnapshotOptionalDomains(snapshot) && HasValidResourceFacts(snapshot) &&
         HasValidExitFacts(snapshot) && HasValidLaunchFacts(snapshot) &&
         HasValidPendingAckFacts(snapshot) && HasValidStateFacts(snapshot);
}
Decision RejectState() { return Reject(RejectionReason::kEventNotAllowedInState); }

Decision DecideResourcesCommitted(const Snapshot& snapshot, const InternalEvent& event) {
  return snapshot.state == JobState::kAdmitted ? Accept(snapshot, event.event_type, event.payload)
                                               : RejectState();
}

Decision DecideWorkerLaunchIntent(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state != JobState::kPreparing) return RejectState();
  const auto* payload = std::get_if<WorkerLaunchIntentPayload>(&event.payload);
  if (!payload) return Reject(RejectionReason::kInvalidEventPayload);
  return snapshot.worker_launch_status == LaunchStatus::kNotStarted &&
                 snapshot.allocation_id == std::optional<StableId>(payload->allocation_id) &&
                 snapshot.allocation_digest == std::optional<Digest>(payload->allocation_digest)
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kInvariantViolation);
}

Decision DecideWorkerLaunchObserved(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state != JobState::kPreparing) return RejectState();
  if (const auto* payload = std::get_if<WorkerLaunchObservedPayload>(&event.payload);
      payload == nullptr || !snapshot.launch_operation_id ||
      *snapshot.launch_operation_id != payload->operation_id ||
      snapshot.worker_launch_status != LaunchStatus::kIntentRecorded)
    return Reject(RejectionReason::kInvariantViolation);
  return Accept(snapshot, event.event_type, event.payload);
}

Decision DecideWorkerRunning(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state != JobState::kPreparing) return RejectState();
  const auto* payload = std::get_if<WorkerRunningPayload>(&event.payload);
  return payload && snapshot.worker_launch_status == LaunchStatus::kObserved &&
                 snapshot.worker_id == std::optional<Uuid>(payload->worker_id)
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kInvariantViolation);
}

Decision DecideCancelAccepted(const Snapshot& snapshot, const InternalEvent& event) {
  using enum JobState;

  if (snapshot.state == kStopping) return Reject(RejectionReason::kStopCauseAlreadyLatched);
  if (snapshot.state == kFinalizing || IsTerminal(snapshot.state))
    return Reject(RejectionReason::kCommandNotAllowedInState);
  return snapshot.state == kAdmitted || snapshot.state == kPreparing || snapshot.state == kRunning
             ? Accept(snapshot, event.event_type, event.payload)
             : RejectState();
}

Decision DecideTerminateAccepted(const Snapshot& snapshot, const InternalEvent& event) {
  using enum JobState;

  if (snapshot.state == kStopping || snapshot.state == kFinalizing) {
    return !snapshot.process_exit_confirmed ? Accept(snapshot, event.event_type, event.payload)
                                            : Reject(RejectionReason::kCommandNotAllowedInState);
  }
  if (IsTerminal(snapshot.state)) return Reject(RejectionReason::kCommandNotAllowedInState);
  return snapshot.state == kAdmitted || snapshot.state == kPreparing || snapshot.state == kRunning
             ? Accept(snapshot, event.event_type, event.payload)
             : RejectState();
}

Decision DecideFinalizingTimeout(const Snapshot& snapshot, const InternalEvent& event,
                                 const TimeoutExpiredPayload& payload) {
  using enum TerminalOutcome;

  if (payload.phase != TimeoutPhase::kExecution)
    return Reject(RejectionReason::kTimeoutPhaseMismatch);
  if (!snapshot.latched_reason && snapshot.completion_candidate)
    return Accept(snapshot, event.event_type, event.payload);
  if (snapshot.latched_reason == kFailed || snapshot.latched_reason == kCancelled ||
      snapshot.latched_reason == kTerminated || snapshot.latched_reason == kTimedOut)
    return Accept(snapshot, event.event_type, event.payload);
  return Reject(RejectionReason::kTimeoutPhaseMismatch);
}

Decision DecideTimeoutExpired(const Snapshot& snapshot, const InternalEvent& event) {
  using enum JobState;
  {
    using enum RejectionReason;
    {
      using enum TimeoutPhase;

      const auto* payload = std::get_if<TimeoutExpiredPayload>(&event.payload);
      if (!payload) return Reject(kInvalidEventPayload);
      if (snapshot.state == kPreparing)
        return payload->phase == kPreparation ? Accept(snapshot, event.event_type, event.payload)
                                              : Reject(kTimeoutPhaseMismatch);
      if (snapshot.state == kRunning)
        return payload->phase == kExecution ? Accept(snapshot, event.event_type, event.payload)
                                            : Reject(kTimeoutPhaseMismatch);
      if (snapshot.state == kStopping)
        return payload->phase == kExecution || payload->phase == kCooperativeStop
                   ? Accept(snapshot, event.event_type, event.payload)
                   : Reject(kTimeoutPhaseMismatch);
      if (snapshot.state == kFinalizing) return DecideFinalizingTimeout(snapshot, event, *payload);
      if (!IsTerminal(snapshot.state)) return RejectState();
      if (payload->phase != kProcessExitConfirmation) return Reject(kTimeoutPhaseMismatch);
      return snapshot.process_exit_confirmed ? Reject(kInvariantViolation)
                                             : Accept(snapshot, event.event_type, event.payload);
    }
  }
}

Decision DecideWorkerOutcome(const Snapshot& snapshot, const InternalEvent& event) {
  using enum JobState;
  {
    using enum RejectionReason;

    const auto* payload = std::get_if<WorkerEventPayload>(&event.payload);
    if (!payload) return Reject(kInvalidEventPayload);
    if (IsTerminal(snapshot.state) || snapshot.state == kFinalizing) {
      return snapshot.worker_id && *snapshot.worker_id == payload->worker_id
                 ? Accept(snapshot, EventType::kLateWorkerEvent,
                          LateWorkerEventPayload{event.event_type, payload->worker_id,
                                                 payload->event_sequence})
                 : Reject(kInvariantViolation);
    }
    if (snapshot.state != kRunning && snapshot.state != kStopping &&
        !(snapshot.state == kPreparing && event.event_type == EventType::kWorkerFailed))
      return RejectState();
    return snapshot.worker_id && *snapshot.worker_id == payload->worker_id
               ? Accept(snapshot, event.event_type, event.payload)
               : Reject(kInvariantViolation);
  }
}

Decision DecideProcessExitConfirmed(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state == JobState::kAdmitted) return RejectState();
  const auto* payload = std::get_if<ProcessExitConfirmedPayload>(&event.payload);
  if (!payload || !snapshot.launch_operation_id ||
      *snapshot.launch_operation_id != payload->launch_operation_id)
    return Reject(RejectionReason::kInvariantViolation);
  if ((snapshot.state == JobState::kFinalizing || IsTerminal(snapshot.state)) &&
      snapshot.process_exit_confirmed && snapshot.completion_mode != payload->completion_mode)
    return Reject(RejectionReason::kInvariantViolation);
  return Accept(snapshot, event.event_type, event.payload);
}

Decision DecideSessionRetain(const Snapshot& snapshot, const InternalEvent& event,
                             RetentionStatus expected_status) {
  if (snapshot.state != JobState::kFinalizing) return RejectState();
  const auto* payload = std::get_if<SessionPayload>(&event.payload);
  return payload && snapshot.session_retention_status == expected_status &&
                 payload->session_id == snapshot.session_id
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kInvariantViolation);
}

Decision DecideFinalizationCompleted(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state != JobState::kFinalizing) return RejectState();
  if (snapshot.session_retention_status != RetentionStatus::kRetained)
    return Reject(RejectionReason::kRequiredFinalizationFactMissing);
  return snapshot.finalization_status == FinalizationStatus::kPending
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kInvariantViolation);
}

Decision DecideFinalizationFailed(const Snapshot& snapshot, const InternalEvent& event) {
  if (snapshot.state != JobState::kFinalizing) return RejectState();
  if (snapshot.finalization_status != FinalizationStatus::kPending ||
      (!snapshot.latched_reason && !snapshot.completion_candidate))
    return Reject(RejectionReason::kInvariantViolation);
  return Accept(snapshot, event.event_type, event.payload);
}

Decision DecideTerminalOutcomeCommitted(const Snapshot& snapshot, const InternalEvent& event) {
  using enum TerminalOutcome;

  if (snapshot.state != JobState::kFinalizing) return RejectState();
  const auto* payload = std::get_if<TerminalOutcomePayload>(&event.payload);
  if (!payload || snapshot.finalization_status == FinalizationStatus::kPending)
    return Reject(RejectionReason::kRequiredFinalizationFactMissing);
  const bool succeeded = payload->outcome == kSucceeded && !snapshot.latched_reason &&
                         snapshot.completion_candidate &&
                         snapshot.finalization_status == FinalizationStatus::kCompleted;
  const bool failed = payload->outcome == kFailed && snapshot.latched_reason == kFailed;
  const bool cancelled = payload->outcome == kCancelled && snapshot.latched_reason == kCancelled;
  const bool terminated = payload->outcome == kTerminated && snapshot.latched_reason == kTerminated;
  const bool timed_out = payload->outcome == kTimedOut && snapshot.latched_reason == kTimedOut;
  return succeeded || failed || cancelled || terminated || timed_out
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kTerminalOutcomeMismatch);
}

Decision DecideResourcesReleased(const Snapshot& snapshot, const InternalEvent& event) {
  using enum RejectionReason;

  if (!IsTerminal(snapshot.state)) return RejectState();
  if (!snapshot.process_exit_confirmed) return Reject(kInvariantViolation);
  if (const auto* payload = std::get_if<ResourcesReleasedPayload>(&event.payload);
      payload == nullptr || !snapshot.allocation_id || !snapshot.allocation_digest ||
      payload->allocation_id != *snapshot.allocation_id ||
      payload->allocation_digest != *snapshot.allocation_digest)
    return Reject(kInvariantViolation);
  return snapshot.resource_status == ResourceStatus::kCommitted ||
                 snapshot.resource_status == ResourceStatus::kReleased
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(kInvariantViolation);
}

Decision DecideCleanupStatusRecorded(const Snapshot& snapshot, const InternalEvent& event) {
  if (!IsTerminal(snapshot.state)) return RejectState();
  const auto* payload = std::get_if<CleanupStatusPayload>(&event.payload);
  return payload && (snapshot.cleanup_status == CleanupStatus::kPending ||
                     snapshot.cleanup_status == payload->status)
             ? Accept(snapshot, event.event_type, event.payload)
             : Reject(RejectionReason::kInvariantViolation);
}

Decision DecideInternal(const Snapshot& snapshot, const InternalEvent& event) {
  using enum EventType;

  if (!ValidSnapshot(snapshot)) return Reject(RejectionReason::kInvariantViolation);
  if (event.schema_version != 1 || event.job_id != snapshot.job_id ||
      !IsUuid(event.job_id.value, '7') ||
      !ValidInternalPayload(event.event_type, event.payload, event.job_id))
    return Reject(RejectionReason::kInvalidEventPayload);
  if (!snapshot.entity_exists && event.event_type != kJobCreated)
    return Reject(RejectionReason::kJobNotFound);
  if (event.event_type == kJobCreated)
    return snapshot.entity_exists ? Reject(RejectionReason::kJobAlreadyExists)
                                  : Accept(snapshot, event.event_type, event.payload);

  switch (event.event_type) {
    case kResourcesCommitted:
      return DecideResourcesCommitted(snapshot, event);
    case kWorkerLaunchIntent:
      return DecideWorkerLaunchIntent(snapshot, event);
    case kWorkerLaunchObserved:
      return DecideWorkerLaunchObserved(snapshot, event);
    case kWorkerRunning:
      return DecideWorkerRunning(snapshot, event);
    case kCancelAccepted:
      return DecideCancelAccepted(snapshot, event);
    case kTerminateAccepted:
      return DecideTerminateAccepted(snapshot, event);
    case kTimeoutExpired:
      return DecideTimeoutExpired(snapshot, event);
    case kWorkerCompleted:
    case kWorkerFailed:
      return DecideWorkerOutcome(snapshot, event);
    case kProcessExitConfirmed:
      return DecideProcessExitConfirmed(snapshot, event);
    case kSessionRetainRequested:
      return DecideSessionRetain(snapshot, event, RetentionStatus::kNotStarted);
    case kSessionRetained:
      return DecideSessionRetain(snapshot, event, RetentionStatus::kRequested);
    case kFinalizationCompleted:
      return DecideFinalizationCompleted(snapshot, event);
    case kFinalizationFailed:
      return DecideFinalizationFailed(snapshot, event);
    case kTerminalOutcomeCommitted:
      return DecideTerminalOutcomeCommitted(snapshot, event);
    case kResourcesReleased:
      return DecideResourcesReleased(snapshot, event);
    case kCleanupStatusRecorded:
      return DecideCleanupStatusRecorded(snapshot, event);
    case kLateWorkerEvent:
      return IsTerminal(snapshot.state) || snapshot.state == JobState::kFinalizing
                 ? Accept(snapshot, event.event_type, event.payload)
                 : RejectState();
    case kJobCreated:
      return Reject(RejectionReason::kJobAlreadyExists);
    case kInvalid:
      return Reject(RejectionReason::kInvalidEventPayload);
  }
  return Reject(RejectionReason::kInvariantViolation);
}
void Add(std::vector<Effect>* effects, EffectId effect) { effects->push_back(Effect{effect}); }
void BeginFinalizing(Snapshot* s) {
  s->state = JobState::kFinalizing;
  s->finalization_status = FinalizationStatus::kPending;
}
bool PayloadMatches(EventType type, const EventPayload& payload) {
  switch (type) {
    case EventType::kJobCreated:
      return std::holds_alternative<JobCreatedPayload>(payload);
    case EventType::kResourcesCommitted:
      return std::holds_alternative<ResourcesCommittedPayload>(payload);
    case EventType::kWorkerLaunchIntent:
      return std::holds_alternative<WorkerLaunchIntentPayload>(payload);
    case EventType::kWorkerLaunchObserved:
      return std::holds_alternative<WorkerLaunchObservedPayload>(payload);
    case EventType::kWorkerRunning:
      return std::holds_alternative<WorkerRunningPayload>(payload);
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      return std::holds_alternative<PrincipalPayload>(payload);
    case EventType::kTimeoutExpired:
      return std::holds_alternative<TimeoutExpiredPayload>(payload);
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed:
      return std::holds_alternative<WorkerEventPayload>(payload);
    case EventType::kProcessExitConfirmed:
      return std::holds_alternative<ProcessExitConfirmedPayload>(payload);
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      return std::holds_alternative<SessionPayload>(payload);
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      return std::holds_alternative<EmptyPayload>(payload);
    case EventType::kTerminalOutcomeCommitted:
      return std::holds_alternative<TerminalOutcomePayload>(payload);
    case EventType::kResourcesReleased:
      return std::holds_alternative<ResourcesReleasedPayload>(payload);
    case EventType::kCleanupStatusRecorded:
      return std::holds_alternative<CleanupStatusPayload>(payload);
    case EventType::kLateWorkerEvent:
      return std::holds_alternative<LateWorkerEventPayload>(payload);
    case EventType::kInvalid:
      return false;
  }
  return false;
}
}  // namespace

Snapshot InitialSnapshot(const Uuid& job_id, const Uuid& session_id) {
  Snapshot result;
  result.job_id = job_id;
  result.session_id = session_id;
  result.entity_exists = false;
  result.state = JobState::kAdmitted;
  result.completion_mode = CompletionMode::kNone;
  result.resource_status = ResourceStatus::kNone;
  result.worker_launch_status = LaunchStatus::kNotStarted;
  result.process_presence = ProcessPresence::kAbsent;
  result.session_retention_status = RetentionStatus::kNotStarted;
  result.finalization_status = FinalizationStatus::kNotStarted;
  result.cleanup_status = CleanupStatus::kPending;
  return result;
}
Decision DecideCommand(const Snapshot& s, const Command& command) {
  if (!ValidSnapshot(s)) return Reject(RejectionReason::kInvariantViolation);
  if (command.schema_version != 1 || !IsUuid(command.job_id.value, '7') ||
      !IsPrincipal(command.principal_subject) || !IsCommandType(command.command_type))
    return Reject(RejectionReason::kInvalidEventPayload);
  if (command.job_id != s.job_id || !s.entity_exists) return Reject(RejectionReason::kJobNotFound);
  if (command.command_type == CommandType::kCancel) {
    if (s.state == JobState::kStopping) return Reject(RejectionReason::kStopCauseAlreadyLatched);
    if (s.state != JobState::kAdmitted && s.state != JobState::kPreparing &&
        s.state != JobState::kRunning)
      return Reject(RejectionReason::kCommandNotAllowedInState);
  } else {
    if ((s.state == JobState::kStopping || s.state == JobState::kFinalizing) &&
        s.process_exit_confirmed)
      return Reject(RejectionReason::kCommandNotAllowedInState);
    if (s.state != JobState::kAdmitted && s.state != JobState::kPreparing &&
        s.state != JobState::kRunning && s.state != JobState::kStopping &&
        s.state != JobState::kFinalizing)
      return Reject(RejectionReason::kCommandNotAllowedInState);
  }
  const auto event = command.command_type == CommandType::kCancel ? EventType::kCancelAccepted
                                                                  : EventType::kTerminateAccepted;
  return Accept(s, event, PrincipalPayload{command.principal_subject});
}
NormalizedCandidate NormalizeCandidate(const Snapshot& s, const RawCandidateEvent& candidate) {
  return ParseCandidate(s, candidate);
}
Decision DecideEvent(const Snapshot& s, const InternalEvent& event) {
  return DecideInternal(s, event);
}

namespace {
void ApplyJobCreated(Snapshot* snapshot, const JobCreatedPayload& payload) {
  snapshot->entity_exists = true;
  snapshot->state = JobState::kAdmitted;
  snapshot->session_id = payload.session_id;
  snapshot->latched_reason.reset();
  snapshot->completion_candidate = false;
  snapshot->completion_mode = CompletionMode::kNone;
  snapshot->resource_status = ResourceStatus::kNone;
  snapshot->allocation_id.reset();
  snapshot->allocation_digest.reset();
  snapshot->worker_launch_status = LaunchStatus::kNotStarted;
  snapshot->launch_operation_id.reset();
  snapshot->worker_id.reset();
  snapshot->process_presence = ProcessPresence::kAbsent;
  snapshot->process_exit_confirmed = false;
  snapshot->session_retention_status = RetentionStatus::kNotStarted;
  snapshot->finalization_status = FinalizationStatus::kNotStarted;
  snapshot->cleanup_status = CleanupStatus::kPending;
  snapshot->pending_worker_event_ack = false;
  snapshot->pending_worker_id.reset();
  snapshot->pending_worker_event_sequence.reset();
}

void ApplyResourcesCommitted(Snapshot* snapshot, std::vector<Effect>* effects,
                             const ResourcesCommittedPayload& payload) {
  snapshot->state = JobState::kPreparing;
  snapshot->resource_status = ResourceStatus::kCommitted;
  snapshot->allocation_id = payload.allocation_id;
  snapshot->allocation_digest = payload.allocation_digest;
  Add(effects, EffectId::kArmPreparationTimeout);
}

void ApplyWorkerLaunchIntent(Snapshot* snapshot, std::vector<Effect>* effects,
                             const WorkerLaunchIntentPayload& payload) {
  snapshot->worker_launch_status = LaunchStatus::kIntentRecorded;
  snapshot->launch_operation_id = payload.operation_id;
  snapshot->worker_id = payload.worker_id;
  snapshot->process_presence = ProcessPresence::kUnknown;
  Add(effects, EffectId::kLaunchWorkerOnce);
}

void ApplyWorkerLaunchObserved(Snapshot* snapshot, std::vector<Effect>* effects,
                               const WorkerLaunchObservedPayload& payload) {
  snapshot->worker_launch_status =
      payload.started ? LaunchStatus::kObserved : LaunchStatus::kFailed;
  snapshot->process_presence =
      payload.started ? ProcessPresence::kPresent : ProcessPresence::kAbsent;
  if (payload.started) return;
  snapshot->latched_reason = TerminalOutcome::kFailed;
  snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
  snapshot->process_exit_confirmed = true;
  BeginFinalizing(snapshot);
  Add(effects, EffectId::kDisarmPreparationTimeout);
}

void ApplyWorkerRunning(Snapshot* snapshot, std::vector<Effect>* effects) {
  snapshot->state = JobState::kRunning;
  snapshot->process_presence = ProcessPresence::kPresent;
  Add(effects, EffectId::kDisarmPreparationTimeout);
  Add(effects, EffectId::kArmExecutionTimeout);
}

void ApplyCancelAccepted(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  const auto old_state = snapshot->state;
  snapshot->latched_reason = TerminalOutcome::kCancelled;
  if (old_state == JobState::kAdmitted ||
      (old_state == JobState::kPreparing &&
       snapshot->process_presence == ProcessPresence::kAbsent)) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    snapshot->process_exit_confirmed = true;
    if (old_state == JobState::kPreparing) Add(effects, kDisarmPreparationTimeout);
    BeginFinalizing(snapshot);
    return;
  }
  snapshot->completion_mode = CompletionMode::kCooperative;
  if (old_state == JobState::kPreparing) Add(effects, kDisarmPreparationTimeout);
  snapshot->state = JobState::kStopping;
  Add(effects, kRequestCooperativeStop);
  Add(effects, kArmCooperativeStopTimeout);
}

void ApplyTerminateAccepted(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  if (snapshot->state == JobState::kStopping || snapshot->state == JobState::kFinalizing) {
    snapshot->completion_mode = CompletionMode::kForced;
    Add(effects, kRequestForcedStop);
    Add(effects, kDisarmCooperativeStopTimeout);
    Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
    return;
  }
  const auto old_state = snapshot->state;
  snapshot->latched_reason = TerminalOutcome::kTerminated;
  if (old_state == JobState::kAdmitted ||
      (old_state == JobState::kPreparing &&
       snapshot->process_presence == ProcessPresence::kAbsent)) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    snapshot->process_exit_confirmed = true;
    if (old_state == JobState::kPreparing) Add(effects, kDisarmPreparationTimeout);
    BeginFinalizing(snapshot);
    return;
  }
  snapshot->completion_mode = CompletionMode::kForced;
  if (old_state == JobState::kPreparing) Add(effects, kDisarmPreparationTimeout);
  snapshot->state = JobState::kStopping;
  Add(effects, kRequestForcedStop);
  Add(effects, kDisarmCooperativeStopTimeout);
  Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
}

void ApplyPreparationTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  snapshot->latched_reason = TerminalOutcome::kTimedOut;
  Add(effects, kDisarmPreparationTimeout);
  if (snapshot->process_presence == ProcessPresence::kAbsent) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    snapshot->process_exit_confirmed = true;
    BeginFinalizing(snapshot);
    return;
  }
  snapshot->completion_mode = CompletionMode::kCooperative;
  snapshot->state = JobState::kStopping;
  Add(effects, kRequestCooperativeStop);
  Add(effects, kArmCooperativeStopTimeout);
}

void ApplyFinalizingExecutionTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  const bool had_success_candidate = !snapshot->latched_reason && snapshot->completion_candidate;
  if (had_success_candidate) {
    snapshot->latched_reason = TerminalOutcome::kTimedOut;
    snapshot->completion_candidate = false;
  }
  if (snapshot->process_exit_confirmed && had_success_candidate) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    return;
  }
  if (snapshot->process_exit_confirmed) return;
  snapshot->completion_mode = CompletionMode::kForced;
  Add(effects, kRequestForcedStop);
  Add(effects, kDisarmCooperativeStopTimeout);
  Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
}

void ApplyStoppingExecutionTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  snapshot->completion_mode = CompletionMode::kForced;
  Add(effects, kDisarmExecutionTimeout);
  Add(effects, kDisarmCooperativeStopTimeout);
  Add(effects, kRequestForcedStop);
  Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
}

void ApplyRunningExecutionTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  snapshot->latched_reason = TerminalOutcome::kTimedOut;
  snapshot->completion_mode = CompletionMode::kCooperative;
  snapshot->state = JobState::kStopping;
  Add(effects, EffectId::kRequestCooperativeStop);
  Add(effects, EffectId::kArmCooperativeStopTimeout);
}

void ApplyExecutionTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum JobState;

  if (snapshot->state == kFinalizing) {
    ApplyFinalizingExecutionTimeout(snapshot, effects);
    return;
  }
  if (snapshot->state == kStopping) {
    ApplyStoppingExecutionTimeout(snapshot, effects);
    return;
  }
  if (snapshot->state == kRunning) ApplyRunningExecutionTimeout(snapshot, effects);
}

void ApplyCooperativeStopTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  snapshot->completion_mode = CompletionMode::kForced;
  Add(effects, kDisarmCooperativeStopTimeout);
  Add(effects, kRequestForcedStop);
  Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
}

void ApplyProcessExitConfirmationTimeout(Snapshot* snapshot, std::vector<Effect>* effects) {
  using enum EffectId;

  snapshot->cleanup_status = CleanupStatus::kIncomplete;
  Add(effects, kRequestForcedStop);
  Add(effects, kQuarantineResources);
  Add(effects, kSetReadinessFalse);
}

void ApplyTimeoutExpired(Snapshot* snapshot, std::vector<Effect>* effects,
                         const TimeoutExpiredPayload& payload) {
  using enum TimeoutPhase;

  switch (payload.phase) {
    case kPreparation:
      ApplyPreparationTimeout(snapshot, effects);
      return;
    case kExecution:
      ApplyExecutionTimeout(snapshot, effects);
      return;
    case kCooperativeStop:
      ApplyCooperativeStopTimeout(snapshot, effects);
      return;
    case kProcessExitConfirmation:
      ApplyProcessExitConfirmationTimeout(snapshot, effects);
      return;
    case kInvalid:
      return;
  }
}

void AddWorkerAck(Snapshot* snapshot, const WorkerEventPayload& payload) {
  snapshot->pending_worker_event_ack = true;
  snapshot->pending_worker_id = payload.worker_id;
  snapshot->pending_worker_event_sequence = payload.event_sequence;
}

void ApplyWorkerCompleted(Snapshot* snapshot, std::vector<Effect>* effects,
                          const WorkerEventPayload& payload) {
  const auto old_state = snapshot->state;
  if (old_state == JobState::kRunning) snapshot->completion_candidate = true;
  if (old_state == JobState::kStopping) Add(effects, EffectId::kDisarmCooperativeStopTimeout);
  BeginFinalizing(snapshot);
  AddWorkerAck(snapshot, payload);
}

void ApplyWorkerFailed(Snapshot* snapshot, std::vector<Effect>* effects,
                       const WorkerEventPayload& payload) {
  const auto old_state = snapshot->state;
  if (old_state == JobState::kPreparing || old_state == JobState::kRunning)
    snapshot->latched_reason = TerminalOutcome::kFailed;
  if (old_state == JobState::kPreparing) Add(effects, EffectId::kDisarmPreparationTimeout);
  if (old_state == JobState::kStopping) Add(effects, EffectId::kDisarmCooperativeStopTimeout);
  BeginFinalizing(snapshot);
  AddWorkerAck(snapshot, payload);
}

void ClearPendingWorkerAck(Snapshot* snapshot) {
  snapshot->pending_worker_event_ack = false;
  snapshot->pending_worker_id.reset();
  snapshot->pending_worker_event_sequence.reset();
}

void ApplyProcessExitConfirmed(Snapshot* snapshot, std::vector<Effect>* effects,
                               const ProcessExitConfirmedPayload& payload) {
  const auto old_state = snapshot->state;
  const bool first_confirmation = !snapshot->process_exit_confirmed;
  if (old_state == JobState::kFinalizing || IsTerminal(old_state)) {
    if (!first_confirmation) return;
    snapshot->process_exit_confirmed = true;
    snapshot->process_presence = ProcessPresence::kAbsent;
    snapshot->completion_mode = payload.completion_mode;
    ClearPendingWorkerAck(snapshot);
    Add(effects, EffectId::kDisarmCooperativeStopTimeout);
    Add(effects, EffectId::kDisarmProcessExitConfirmationTimeout);
    return;
  }
  snapshot->process_exit_confirmed = true;
  snapshot->process_presence = ProcessPresence::kAbsent;
  ClearPendingWorkerAck(snapshot);
  if (old_state == JobState::kPreparing) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    snapshot->latched_reason = TerminalOutcome::kFailed;
    Add(effects, EffectId::kDisarmPreparationTimeout);
    BeginFinalizing(snapshot);
  } else if (old_state == JobState::kRunning) {
    snapshot->completion_mode = CompletionMode::kProcessAlreadyExited;
    snapshot->latched_reason = TerminalOutcome::kFailed;
    BeginFinalizing(snapshot);
  } else if (old_state == JobState::kStopping) {
    Add(effects, EffectId::kDisarmCooperativeStopTimeout);
    Add(effects, EffectId::kDisarmProcessExitConfirmationTimeout);
    BeginFinalizing(snapshot);
  }
}

void ApplyFinalizationFailed(Snapshot* snapshot) {
  snapshot->finalization_status = FinalizationStatus::kFailed;
  snapshot->cleanup_status = CleanupStatus::kIncomplete;
  if (!snapshot->latched_reason && snapshot->completion_candidate) {
    snapshot->latched_reason = TerminalOutcome::kFailed;
    snapshot->completion_candidate = false;
  }
}

JobState JobStateForOutcome(TerminalOutcome outcome) {
  switch (outcome) {
    case TerminalOutcome::kSucceeded:
      return JobState::kSucceeded;
    case TerminalOutcome::kFailed:
      return JobState::kFailed;
    case TerminalOutcome::kCancelled:
      return JobState::kCancelled;
    case TerminalOutcome::kTerminated:
      return JobState::kTerminated;
    case TerminalOutcome::kTimedOut:
    case TerminalOutcome::kInvalid:
      return JobState::kTimedOut;
  }
  return JobState::kTimedOut;
}

void ApplyTerminalOutcomeCommitted(Snapshot* snapshot, std::vector<Effect>* effects,
                                   const TerminalOutcomePayload& payload) {
  using enum EffectId;

  snapshot->state = JobStateForOutcome(payload.outcome);
  if (snapshot->finalization_status != FinalizationStatus::kFailed)
    snapshot->finalization_status = FinalizationStatus::kCompleted;
  Add(effects, kDisarmExecutionTimeout);
  Add(effects, kAckTerminalWorkerEventIfPending);
  Add(effects, kPublishTerminalResult);
  Add(effects, kArmProcessExitConfirmationTimeoutIfNeeded);
}

bool ApplyAuthorizedEvent(Snapshot* snapshot, std::vector<Effect>* effects,
                          const PreEnvelopeProposal& proposal) {
  using enum EventType;

  switch (proposal.event_type) {
    case kJobCreated:
      ApplyJobCreated(snapshot, std::get<JobCreatedPayload>(proposal.payload));
      return true;
    case kResourcesCommitted:
      ApplyResourcesCommitted(snapshot, effects,
                              std::get<ResourcesCommittedPayload>(proposal.payload));
      return true;
    case kWorkerLaunchIntent:
      ApplyWorkerLaunchIntent(snapshot, effects,
                              std::get<WorkerLaunchIntentPayload>(proposal.payload));
      return true;
    case kWorkerLaunchObserved:
      ApplyWorkerLaunchObserved(snapshot, effects,
                                std::get<WorkerLaunchObservedPayload>(proposal.payload));
      return true;
    case kWorkerRunning:
      ApplyWorkerRunning(snapshot, effects);
      return true;
    case kCancelAccepted:
      ApplyCancelAccepted(snapshot, effects);
      return true;
    case kTerminateAccepted:
      ApplyTerminateAccepted(snapshot, effects);
      return true;
    case kTimeoutExpired:
      ApplyTimeoutExpired(snapshot, effects, std::get<TimeoutExpiredPayload>(proposal.payload));
      return true;
    case kWorkerCompleted:
      ApplyWorkerCompleted(snapshot, effects, std::get<WorkerEventPayload>(proposal.payload));
      return true;
    case kWorkerFailed:
      ApplyWorkerFailed(snapshot, effects, std::get<WorkerEventPayload>(proposal.payload));
      return true;
    case kProcessExitConfirmed:
      ApplyProcessExitConfirmed(snapshot, effects,
                                std::get<ProcessExitConfirmedPayload>(proposal.payload));
      return true;
    case kSessionRetainRequested:
      snapshot->session_retention_status = RetentionStatus::kRequested;
      Add(effects, EffectId::kRetainSessionSameIdentity);
      return true;
    case kSessionRetained:
      snapshot->session_retention_status = RetentionStatus::kRetained;
      return true;
    case kFinalizationCompleted:
      snapshot->finalization_status = FinalizationStatus::kCompleted;
      return true;
    case kFinalizationFailed:
      ApplyFinalizationFailed(snapshot);
      return true;
    case kTerminalOutcomeCommitted:
      ApplyTerminalOutcomeCommitted(snapshot, effects,
                                    std::get<TerminalOutcomePayload>(proposal.payload));
      return true;
    case kResourcesReleased:
      snapshot->resource_status = ResourceStatus::kReleased;
      return true;
    case kCleanupStatusRecorded:
      snapshot->cleanup_status = std::get<CleanupStatusPayload>(proposal.payload).status;
      return true;
    case kLateWorkerEvent:
      Add(effects, EffectId::kAckLateWorkerEvent);
      return true;
    case kInvalid:
      return false;
  }
  return false;
}
}  // namespace

ApplyResult Apply(const Snapshot& before, const PreEnvelopeProposal& proposal) {
  using enum RejectionReason;

  if (!ValidSnapshot(before)) return ApplyResult{before, {}, Rejection{1, kInvariantViolation}};
  const InternalEvent event{proposal.schema_version, proposal.job_id, proposal.event_type,
                            proposal.payload};
  const Decision decision = DecideInternal(before, event);
  if (std::holds_alternative<Rejection>(decision.value))
    return ApplyResult{before, {}, std::get<Rejection>(decision.value)};
  const auto& authorized = std::get<PreEnvelopeProposal>(decision.value);
  if (authorized.schema_version != proposal.schema_version ||
      authorized.job_id != proposal.job_id || authorized.event_type != proposal.event_type)
    return ApplyResult{before, {}, Rejection{1, kInvariantViolation}};
  Snapshot snapshot = before;
  std::vector<Effect> effects;
  if (!ApplyAuthorizedEvent(&snapshot, &effects, proposal))
    return ApplyResult{before, {}, Rejection{1, kInvalidEventPayload}};
  return ApplyResult{std::move(snapshot), std::move(effects), std::nullopt};
}

TimerIngressResult IngestTimer(const TimerState& timers, const TimerIngressInput& input) {
  if (!IsUuid(timers.job_id.value, '7'))
    return TimerIngressResult{
        TimerIngressKind::kFailClosed, std::nullopt, {Effect{EffectId::kSetReadinessFalse}}};
  if (input.arm_request) {
    const auto phase = input.arm_request->phase;
    const bool valid_phase =
        phase == TimeoutPhase::kPreparation || phase == TimeoutPhase::kExecution ||
        phase == TimeoutPhase::kCooperativeStop || phase == TimeoutPhase::kProcessExitConfirmation;
    std::uint64_t active_generation = 0;
    if (phase == TimeoutPhase::kPreparation)
      active_generation = timers.preparation_generation;
    else if (phase == TimeoutPhase::kExecution)
      active_generation = timers.execution_generation;
    else if (phase == TimeoutPhase::kCooperativeStop)
      active_generation = timers.cooperative_stop_generation;
    else if (phase == TimeoutPhase::kProcessExitConfirmation)
      active_generation = timers.process_exit_confirmation_generation;
    if (!valid_phase || !IsUuid(input.arm_request->job_id.value, '7') ||
        input.arm_request->job_id != timers.job_id || input.arm_request->generation == 0 ||
        input.arm_request->generation == UINT64_MAX || active_generation == UINT64_MAX)
      return TimerIngressResult{
          TimerIngressKind::kFailClosed, std::nullopt, {Effect{EffectId::kSetReadinessFalse}}};
  }
  return input.notification
             ? IngestTimer(timers, *input.notification)
             : TimerIngressResult{TimerIngressKind::kDiscardWithoutCandidate, std::nullopt, {}};
}
TimerIngressResult IngestTimer(const TimerState& timers, const TimerNotification& notification) {
  if (!IsUuid(timers.job_id.value, '7') || !IsUuid(notification.job_id.value, '7') ||
      notification.job_id != timers.job_id)
    return TimerIngressResult{
        TimerIngressKind::kFailClosed, std::nullopt, {Effect{EffectId::kSetReadinessFalse}}};
  const auto phase = notification.phase;
  const auto generation = notification.generation;
  if (!IsTimeoutPhase(phase))
    return TimerIngressResult{TimerIngressKind::kDiscardWithoutCandidate, std::nullopt, {}};
  bool armed = false;
  std::uint64_t active = 0;
  if (phase == TimeoutPhase::kPreparation) {
    armed = timers.preparation_armed;
    active = timers.preparation_generation;
  } else if (phase == TimeoutPhase::kExecution) {
    armed = timers.execution_armed;
    active = timers.execution_generation;
  } else if (phase == TimeoutPhase::kCooperativeStop) {
    armed = timers.cooperative_stop_armed;
    active = timers.cooperative_stop_generation;
  } else {
    armed = timers.process_exit_confirmation_armed;
    active = timers.process_exit_confirmation_generation;
  }
  if (!armed || generation != active || generation == 0)
    return TimerIngressResult{TimerIngressKind::kDiscardWithoutCandidate, std::nullopt, {}};
  return TimerIngressResult{
      TimerIngressKind::kEmitCandidateEvent,
      TimeoutCandidate{notification.job_id, TimeoutExpiredPayload{phase, generation}},
      {}};
}
TimerIngressResult IngestTimer(const TimerState& timers, const Uuid& job_id, TimeoutPhase phase,
                               std::uint64_t generation) {
  return IngestTimer(timers, TimerNotification{job_id, phase, generation});
}

bool IsTerminalState(JobState state) noexcept { return IsTerminal(state); }

std::string_view ToString(JobState state) noexcept {
  constexpr std::array<std::string_view, 10> names{
      "admitted",  "preparing", "running",   "stopping",   "finalizing",
      "succeeded", "failed",    "cancelled", "terminated", "timed_out"};
  const auto index = static_cast<std::size_t>(state);
  return index < names.size() ? names[index] : "invalid";
}
std::string_view ToString(EventType type) noexcept {
  constexpr std::array<std::string_view, 19> names{"job_created",
                                                   "resources_committed",
                                                   "worker_launch_intent",
                                                   "worker_launch_observed",
                                                   "worker_running",
                                                   "cancel_accepted",
                                                   "terminate_accepted",
                                                   "timeout_expired",
                                                   "worker_completed",
                                                   "worker_failed",
                                                   "process_exit_confirmed",
                                                   "session_retain_requested",
                                                   "session_retained",
                                                   "finalization_completed",
                                                   "finalization_failed",
                                                   "terminal_outcome_committed",
                                                   "resources_released",
                                                   "cleanup_status_recorded",
                                                   "late_worker_event"};
  const auto index = static_cast<std::size_t>(type);
  return index < names.size() ? names[index] : "invalid";
}
std::string_view ToString(RejectionReason reason) noexcept {
  constexpr std::array<std::string_view, 10> names{"job_not_found",
                                                   "job_already_exists",
                                                   "command_not_allowed_in_state",
                                                   "event_not_allowed_in_state",
                                                   "stop_cause_already_latched",
                                                   "timeout_phase_mismatch",
                                                   "terminal_outcome_mismatch",
                                                   "required_finalization_fact_missing",
                                                   "invalid_event_payload",
                                                   "invariant_violation"};
  const auto index = static_cast<std::size_t>(reason);
  return index < names.size() ? names[index] : "invalid";
}
std::string_view ToString(EffectId effect) noexcept {
  constexpr std::array<std::string_view, 17> names{
      "arm_preparation_timeout",
      "disarm_preparation_timeout",
      "launch_worker_once",
      "arm_execution_timeout",
      "disarm_execution_timeout",
      "request_cooperative_stop",
      "request_forced_stop",
      "retain_session_same_identity",
      "ack_terminal_worker_event_if_pending",
      "ack_late_worker_event",
      "publish_terminal_result",
      "arm_process_exit_confirmation_timeout_if_needed",
      "quarantine_resources",
      "set_readiness_false",
      "arm_cooperative_stop_timeout",
      "disarm_cooperative_stop_timeout",
      "disarm_process_exit_confirmation_timeout"};
  const auto index = static_cast<std::size_t>(effect);
  return index < names.size() ? names[index] : "invalid";
}
}  // namespace sitometron::core
