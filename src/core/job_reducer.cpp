#include "sitometron/core/job_reducer.hpp"

#include <algorithm>
#include <array>
#include <boost/hash2/sha2.hpp>
#include <cctype>
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
bool IsUuid(std::string_view s, char version) {
  if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!std::isxdigit(static_cast<unsigned char>(s[i])) ||
        std::tolower(static_cast<unsigned char>(s[i])) != s[i])
      return false;
  }
  return s[14] == version && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}
bool IsStable(std::string_view s) {
  if (s.empty() || s.size() > 128 || !std::isalnum(static_cast<unsigned char>(s.front())))
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == ':' ||
           c == '-';
  });
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
  if (text.size() > 65536 || std::find(text.begin(), text.end(), '\0') != text.end()) return false;
  try {
    *result = Json::parse(text.begin(), text.end(), nullptr, true, true);
    return true;
  } catch (const Json::exception&) {
    return false;
  }
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
  if (value == text) return EventType::k##name;
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
NormalizedCandidate ParseCandidate(const Snapshot& snapshot, const RawCandidateEvent& candidate) {
  try {
    if (candidate.schema_version != 1 || candidate.job_id != snapshot.job_id ||
        !IsUuid(candidate.job_id.value, '7'))
      return RejectCandidate(RejectionReason::kInvariantViolation);
    const auto type = ParseEvent(candidate.event_type);
    if (!type || *type == EventType::kCancelAccepted || *type == EventType::kTerminateAccepted)
      return RejectCandidate(RejectionReason::kInvalidEventPayload);
    Json p;
    if (!ParseJson(candidate.payload_json, &p))
      return RejectCandidate(RejectionReason::kInvalidEventPayload);
    std::string s;
    switch (*type) {
      case EventType::kJobCreated: {
        if (!HasOnly(p, {"session_id"}) || !StringField(p, "session_id", &s) || !IsUuid(s, '7') ||
            s != candidate.job_id.value)
          break;
        return Normalize(snapshot, *type, JobCreatedPayload{Uuid{s}});
      }
      case EventType::kResourcesCommitted: {
        std::string allocation, digest, schema_id;
        std::uint64_t schema_version = 0;
        if (!HasOnly(p, {"allocation_id", "allocation_digest", "resolved_allocation"}) ||
            !StringField(p, "allocation_id", &allocation) || !IsStable(allocation) ||
            !StringField(p, "allocation_digest", &digest) || !IsDigest(digest) ||
            !p.contains("resolved_allocation") || !p.at("resolved_allocation").is_object())
          break;
        const auto& a = p.at("resolved_allocation");
        if (!HasOnly(a, {"schema_id", "schema_version", "payload_utf8"}) ||
            !StringField(a, "schema_id", &schema_id) || !IsStable(schema_id) ||
            !a.contains("schema_version") || !a.contains("payload_utf8") ||
            !a.at("schema_version").is_number_unsigned() ||
            (schema_version = a.at("schema_version").get<std::uint64_t>()) == 0 ||
            schema_version > UINT32_MAX || !StringField(a, "payload_utf8", &s) ||
            s.size() > 65536 || !ParseJson(s, &p) || Sha256(s) != digest)
          break;
        return Normalize(
            snapshot, *type,
            ResourcesCommittedPayload{StableId{allocation}, Digest{digest}, StableId{schema_id},
                                      static_cast<std::uint32_t>(schema_version), s});
      }
      case EventType::kWorkerLaunchIntent: {
        std::string operation, app, version, bundle, allocation, digest, worker;
        if (!HasOnly(p, {"operation_id", "application", "allocation_id", "allocation_digest",
                         "worker_id"}) ||
            !StringField(p, "operation_id", &operation) || !IsStable(operation) ||
            !p.contains("application") || !p.at("application").is_object() ||
            !StringField(p.at("application"), "application_id", &app) || !IsStable(app) ||
            !StringField(p.at("application"), "version", &version) || version.empty() ||
            version.size() > 128 || !StringField(p.at("application"), "bundle_sha256", &bundle) ||
            !IsDigest(bundle) || !StringField(p, "allocation_id", &allocation) ||
            !IsStable(allocation) || !StringField(p, "allocation_digest", &digest) ||
            !IsDigest(digest) || !StringField(p, "worker_id", &worker) || !IsUuid(worker, '4'))
          break;
        if (!HasOnly(p.at("application"), {"application_id", "version", "bundle_sha256"})) break;
        return Normalize(
            snapshot, *type,
            WorkerLaunchIntentPayload{StableId{operation}, StableId{app}, version, Digest{bundle},
                                      StableId{allocation}, Digest{digest}, Uuid{worker}});
      }
      case EventType::kWorkerLaunchObserved: {
        std::string operation, outcome;
        if (!HasOnly(p, {"operation_id", "outcome"}) ||
            !StringField(p, "operation_id", &operation) || !IsStable(operation) ||
            !StringField(p, "outcome", &outcome) || (outcome != "started" && outcome != "failed"))
          break;
        return Normalize(snapshot, *type,
                         WorkerLaunchObservedPayload{StableId{operation}, outcome == "started"});
      }
      case EventType::kWorkerRunning: {
        if (!HasOnly(p, {"worker_id"}) || !StringField(p, "worker_id", &s) || !IsUuid(s, '4'))
          break;
        return Normalize(snapshot, *type, WorkerRunningPayload{Uuid{s}});
      }
      case EventType::kTimeoutExpired: {
        std::string phase;
        std::uint64_t generation;
        if (!HasOnly(p, {"phase", "timer_generation"}) || !StringField(p, "phase", &phase) ||
            !ParsePhase(phase) || !UInt64Field(p, "timer_generation", &generation))
          break;
        return Normalize(snapshot, *type, TimeoutExpiredPayload{*ParsePhase(phase), generation});
      }
      case EventType::kWorkerCompleted:
      case EventType::kWorkerFailed: {
        std::string worker;
        std::uint64_t sequence;
        if (!HasOnly(p, {"worker_id", "event_sequence"}) || !StringField(p, "worker_id", &worker) ||
            !IsUuid(worker, '4') || !UInt64Field(p, "event_sequence", &sequence))
          break;
        return Normalize(snapshot, *type, WorkerEventPayload{Uuid{worker}, sequence});
      }
      case EventType::kProcessExitConfirmed: {
        std::string mode, operation;
        if (!HasOnly(p, {"completion_mode", "launch_operation_id"}) ||
            !StringField(p, "completion_mode", &mode) || !ParseMode(mode) ||
            !StringField(p, "launch_operation_id", &operation) || !IsStable(operation))
          break;
        return Normalize(snapshot, *type,
                         ProcessExitConfirmedPayload{*ParseMode(mode), StableId{operation}});
      }
      case EventType::kSessionRetainRequested:
      case EventType::kSessionRetained: {
        if (!HasOnly(p, {"session_id"}) || !StringField(p, "session_id", &s) || !IsUuid(s, '7'))
          break;
        return Normalize(snapshot, *type, SessionPayload{Uuid{s}});
      }
      case EventType::kFinalizationCompleted:
      case EventType::kFinalizationFailed:
        if (HasOnly(p, {})) return Normalize(snapshot, *type, EmptyPayload{});
        break;
      case EventType::kTerminalOutcomeCommitted: {
        if (!HasOnly(p, {"outcome"}) || !StringField(p, "outcome", &s) || !ParseOutcome(s)) break;
        return Normalize(snapshot, *type, TerminalOutcomePayload{*ParseOutcome(s)});
      }
      case EventType::kResourcesReleased: {
        std::string allocation, digest;
        if (!HasOnly(p, {"allocation_id", "allocation_digest"}) ||
            !StringField(p, "allocation_id", &allocation) || !IsStable(allocation) ||
            !StringField(p, "allocation_digest", &digest) || !IsDigest(digest))
          break;
        return Normalize(snapshot, *type,
                         ResourcesReleasedPayload{StableId{allocation}, Digest{digest}});
      }
      case EventType::kCleanupStatusRecorded: {
        if (!HasOnly(p, {"status"}) || !StringField(p, "status", &s) ||
            (s != "completed" && s != "incomplete"))
          break;
        return Normalize(snapshot, *type,
                         CleanupStatusPayload{s == "completed" ? CleanupStatus::kCompleted
                                                               : CleanupStatus::kIncomplete});
      }
      case EventType::kLateWorkerEvent: {
        std::string worker;
        std::uint64_t sequence = 0;
        const auto original = p.contains("original_event_type")
                                  ? ParseEvent(p.at("original_event_type").get<std::string>())
                                  : std::nullopt;
        if (!HasOnly(p, {"original_event_type", "worker_id", "event_sequence"}) || !original ||
            (*original != EventType::kWorkerCompleted && *original != EventType::kWorkerFailed) ||
            !StringField(p, "worker_id", &worker) || !IsUuid(worker, '4') ||
            !UInt64Field(p, "event_sequence", &sequence))
          break;
        return Normalize(snapshot, *type,
                         LateWorkerEventPayload{*original, Uuid{worker}, sequence});
      }
      default:
        break;
    }
    return RejectCandidate(RejectionReason::kInvalidEventPayload);
  } catch (const Json::exception&) {
    return RejectCandidate(RejectionReason::kInvalidEventPayload);
  }
}
bool PayloadMatches(EventType type, const EventPayload& payload);
bool ValidSnapshot(const Snapshot& s) {
  if (s.schema_version != 1 || !IsUuid(s.job_id.value, '7') || !IsUuid(s.session_id.value, '7') ||
      s.job_id != s.session_id)
    return false;
  if (static_cast<int>(s.state) < 0 || static_cast<int>(s.state) > 9 ||
      static_cast<int>(s.completion_mode) < 0 || static_cast<int>(s.completion_mode) > 3 ||
      static_cast<int>(s.resource_status) < 0 || static_cast<int>(s.resource_status) > 2 ||
      static_cast<int>(s.worker_launch_status) < 0 ||
      static_cast<int>(s.worker_launch_status) > 3 || static_cast<int>(s.process_presence) < 0 ||
      static_cast<int>(s.process_presence) > 2 ||
      static_cast<int>(s.session_retention_status) < 0 ||
      static_cast<int>(s.session_retention_status) > 2 ||
      static_cast<int>(s.finalization_status) < 0 || static_cast<int>(s.finalization_status) > 3 ||
      static_cast<int>(s.cleanup_status) < 0 || static_cast<int>(s.cleanup_status) > 2)
    return false;
  if (s.resource_status == ResourceStatus::kNone && (s.allocation_id || s.allocation_digest))
    return false;
  if (s.resource_status != ResourceStatus::kNone && (!s.allocation_id || !s.allocation_digest))
    return false;
  if (s.process_exit_confirmed &&
      (s.process_presence != ProcessPresence::kAbsent || s.pending_worker_event_ack ||
       s.pending_worker_id || s.pending_worker_event_sequence))
    return false;
  if (s.completion_mode == CompletionMode::kProcessAlreadyExited && !s.process_exit_confirmed)
    return false;
  if (s.worker_launch_status == LaunchStatus::kNotStarted && (s.launch_operation_id || s.worker_id))
    return false;
  if (s.worker_launch_status != LaunchStatus::kNotStarted &&
      (!s.launch_operation_id || !s.worker_id || !IsUuid(s.worker_id->value, '4')))
    return false;
  if (!s.pending_worker_event_ack && (s.pending_worker_id || s.pending_worker_event_sequence))
    return false;
  if (s.pending_worker_event_ack && (!s.pending_worker_id || !s.pending_worker_event_sequence ||
                                     *s.pending_worker_event_sequence == 0))
    return false;
  if ((s.state == JobState::kAdmitted || s.state == JobState::kPreparing ||
       s.state == JobState::kRunning || s.state == JobState::kStopping) &&
      s.finalization_status != FinalizationStatus::kNotStarted)
    return false;
  if (s.state == JobState::kFinalizing && s.finalization_status == FinalizationStatus::kNotStarted)
    return false;
  if (IsTerminal(s.state) && s.finalization_status != FinalizationStatus::kCompleted &&
      s.finalization_status != FinalizationStatus::kFailed)
    return false;
  return true;
}
Decision DecideInternal(const Snapshot& s, const InternalEvent& e) {
  if (!ValidSnapshot(s)) return Reject(RejectionReason::kInvariantViolation);
  if (e.schema_version != 1 || e.job_id != s.job_id || !PayloadMatches(e.event_type, e.payload))
    return Reject(RejectionReason::kInvalidEventPayload);
  if (!IsUuid(e.job_id.value, '7')) return Reject(RejectionReason::kInvalidEventPayload);
  if (const auto* created = std::get_if<JobCreatedPayload>(&e.payload)) {
    if (!IsUuid(created->session_id.value, '7') || created->session_id != e.job_id)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* worker_event = std::get_if<WorkerEventPayload>(&e.payload)) {
    if (!IsUuid(worker_event->worker_id.value, '4') || worker_event->event_sequence == 0)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* timeout = std::get_if<TimeoutExpiredPayload>(&e.payload)) {
    if (timeout->timer_generation == 0 || static_cast<int>(timeout->phase) < 0 ||
        static_cast<int>(timeout->phase) > 3)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* process_exit = std::get_if<ProcessExitConfirmedPayload>(&e.payload)) {
    if (!IsStable(process_exit->launch_operation_id.value) ||
        static_cast<int>(process_exit->completion_mode) < 0 ||
        static_cast<int>(process_exit->completion_mode) > 3)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* worker_running = std::get_if<WorkerRunningPayload>(&e.payload)) {
    if (!IsUuid(worker_running->worker_id.value, '4'))
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* session = std::get_if<SessionPayload>(&e.payload)) {
    if (!IsUuid(session->session_id.value, '7'))
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* late_worker = std::get_if<LateWorkerEventPayload>(&e.payload)) {
    if ((late_worker->original_event_type != EventType::kWorkerCompleted &&
         late_worker->original_event_type != EventType::kWorkerFailed) ||
        !IsUuid(late_worker->worker_id.value, '4') || late_worker->event_sequence == 0)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* cleanup = std::get_if<CleanupStatusPayload>(&e.payload)) {
    if (static_cast<int>(cleanup->status) < 0 || static_cast<int>(cleanup->status) > 2)
      return Reject(RejectionReason::kInvalidEventPayload);
  } else if (const auto* terminal = std::get_if<TerminalOutcomePayload>(&e.payload)) {
    if (static_cast<int>(terminal->outcome) < 0 || static_cast<int>(terminal->outcome) > 4)
      return Reject(RejectionReason::kInvalidEventPayload);
  }
  const auto reject_state = [&] { return Reject(RejectionReason::kEventNotAllowedInState); };
  if (!s.entity_exists && e.event_type != EventType::kJobCreated)
    return Reject(RejectionReason::kJobNotFound);
  if (e.event_type == EventType::kJobCreated) {
    return s.entity_exists ? Reject(RejectionReason::kJobAlreadyExists)
                           : Accept(s, e.event_type, e.payload);
  }
  switch (e.event_type) {
    case EventType::kResourcesCommitted:
      return s.state == JobState::kAdmitted ? Accept(s, e.event_type, e.payload) : reject_state();
    case EventType::kWorkerLaunchIntent: {
      if (s.state != JobState::kPreparing) return reject_state();
      const auto* p = std::get_if<WorkerLaunchIntentPayload>(&e.payload);
      if (!p) return Reject(RejectionReason::kInvalidEventPayload);
      return s.worker_launch_status == LaunchStatus::kNotStarted &&
                     s.allocation_id == std::optional<StableId>(p->allocation_id) &&
                     s.allocation_digest == std::optional<Digest>(p->allocation_digest)
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    }
    case EventType::kWorkerLaunchObserved: {
      if (s.state != JobState::kPreparing) return reject_state();
      const auto* p = std::get_if<WorkerLaunchObservedPayload>(&e.payload);
      if (!p || !s.launch_operation_id || *s.launch_operation_id != p->operation_id ||
          s.worker_launch_status != LaunchStatus::kIntentRecorded)
        return Reject(RejectionReason::kInvariantViolation);
      return Accept(s, e.event_type, e.payload);
    }
    case EventType::kWorkerRunning: {
      if (s.state != JobState::kPreparing) return reject_state();
      const auto* p = std::get_if<WorkerRunningPayload>(&e.payload);
      return p && s.worker_launch_status == LaunchStatus::kObserved &&
                     s.worker_id == std::optional<Uuid>(p->worker_id)
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    }
    case EventType::kCancelAccepted:
      if (s.state == JobState::kStopping) return Reject(RejectionReason::kStopCauseAlreadyLatched);
      if (s.state == JobState::kFinalizing || IsTerminal(s.state))
        return Reject(RejectionReason::kCommandNotAllowedInState);
      return (s.state == JobState::kAdmitted || s.state == JobState::kPreparing ||
              s.state == JobState::kRunning)
                 ? Accept(s, e.event_type, e.payload)
                 : reject_state();
    case EventType::kTerminateAccepted: {
      if (s.state == JobState::kStopping || s.state == JobState::kFinalizing) {
        return !s.process_exit_confirmed ? Accept(s, e.event_type, e.payload)
                                         : Reject(RejectionReason::kCommandNotAllowedInState);
      }
      if (IsTerminal(s.state)) return Reject(RejectionReason::kCommandNotAllowedInState);
      return (s.state == JobState::kAdmitted || s.state == JobState::kPreparing ||
              s.state == JobState::kRunning)
                 ? Accept(s, e.event_type, e.payload)
                 : reject_state();
    }
    case EventType::kTimeoutExpired: {
      const auto* p = std::get_if<TimeoutExpiredPayload>(&e.payload);
      if (!p) return Reject(RejectionReason::kInvalidEventPayload);
      if (s.state == JobState::kPreparing) {
        return p->phase == TimeoutPhase::kPreparation
                   ? Accept(s, e.event_type, e.payload)
                   : Reject(RejectionReason::kTimeoutPhaseMismatch);
      }
      if (s.state == JobState::kRunning) {
        return p->phase == TimeoutPhase::kExecution
                   ? Accept(s, e.event_type, e.payload)
                   : Reject(RejectionReason::kTimeoutPhaseMismatch);
      }
      if (s.state == JobState::kStopping) {
        return (p->phase == TimeoutPhase::kExecution || p->phase == TimeoutPhase::kCooperativeStop)
                   ? Accept(s, e.event_type, e.payload)
                   : Reject(RejectionReason::kTimeoutPhaseMismatch);
      }
      if (s.state == JobState::kFinalizing) {
        return p->phase == TimeoutPhase::kExecution
                   ? Accept(s, e.event_type, e.payload)
                   : Reject(RejectionReason::kTimeoutPhaseMismatch);
      }
      if (IsTerminal(s.state)) {
        if (p->phase != TimeoutPhase::kProcessExitConfirmation)
          return Reject(RejectionReason::kTimeoutPhaseMismatch);
        return s.process_exit_confirmed ? Reject(RejectionReason::kInvariantViolation)
                                        : Accept(s, e.event_type, e.payload);
      }
      return reject_state();
    }
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed: {
      const auto* p = std::get_if<WorkerEventPayload>(&e.payload);
      if (!p) return Reject(RejectionReason::kInvalidEventPayload);
      if (IsTerminal(s.state) || s.state == JobState::kFinalizing) {
        return s.worker_id && *s.worker_id == p->worker_id
                   ? Accept(s, EventType::kLateWorkerEvent,
                            LateWorkerEventPayload{e.event_type, p->worker_id, p->event_sequence})
                   : Reject(RejectionReason::kInvariantViolation);
      }
      if (s.state != JobState::kRunning && s.state != JobState::kStopping &&
          !(s.state == JobState::kPreparing && e.event_type == EventType::kWorkerFailed))
        return reject_state();
      return s.worker_id && *s.worker_id == p->worker_id
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    }
    case EventType::kProcessExitConfirmed: {
      if (s.state == JobState::kAdmitted ||
          (s.state == JobState::kPreparing && !s.launch_operation_id))
        return reject_state();
      const auto* p = std::get_if<ProcessExitConfirmedPayload>(&e.payload);
      if (!p || !s.launch_operation_id || *s.launch_operation_id != p->launch_operation_id)
        return Reject(RejectionReason::kInvariantViolation);
      if (s.process_exit_confirmed && s.completion_mode != p->completion_mode)
        return Reject(RejectionReason::kInvariantViolation);
      return Accept(s, e.event_type, e.payload);
    }
    case EventType::kSessionRetainRequested: {
      const auto* p = std::get_if<SessionPayload>(&e.payload);
      return s.state == JobState::kFinalizing && p &&
                     s.session_retention_status == RetentionStatus::kNotStarted &&
                     p->session_id == s.session_id
                 ? Accept(s, e.event_type, e.payload)
                 : (s.state != JobState::kFinalizing
                        ? reject_state()
                        : Reject(RejectionReason::kInvariantViolation));
    }
    case EventType::kSessionRetained: {
      const auto* p = std::get_if<SessionPayload>(&e.payload);
      return s.state == JobState::kFinalizing && p &&
                     s.session_retention_status == RetentionStatus::kRequested &&
                     p->session_id == s.session_id
                 ? Accept(s, e.event_type, e.payload)
                 : (s.state != JobState::kFinalizing
                        ? reject_state()
                        : Reject(RejectionReason::kInvariantViolation));
    }
    case EventType::kFinalizationCompleted:
      return s.state != JobState::kFinalizing ? reject_state()
             : s.session_retention_status != RetentionStatus::kRetained
                 ? Reject(RejectionReason::kRequiredFinalizationFactMissing)
             : s.finalization_status == FinalizationStatus::kPending
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    case EventType::kFinalizationFailed:
      return s.state != JobState::kFinalizing ? reject_state()
             : s.finalization_status != FinalizationStatus::kPending
                 ? Reject(RejectionReason::kInvariantViolation)
             : !s.latched_reason && !s.completion_candidate
                 ? Reject(RejectionReason::kInvariantViolation)
                 : Accept(s, e.event_type, e.payload);
    case EventType::kTerminalOutcomeCommitted: {
      if (s.state != JobState::kFinalizing) return reject_state();
      const auto* p = std::get_if<TerminalOutcomePayload>(&e.payload);
      if (!p || s.finalization_status == FinalizationStatus::kPending)
        return Reject(RejectionReason::kRequiredFinalizationFactMissing);
      const auto expected = s.latched_reason.value_or(
          s.completion_candidate ? TerminalOutcome::kSucceeded : TerminalOutcome::kFailed);
      return p->outcome == expected ? Accept(s, e.event_type, e.payload)
                                    : Reject(RejectionReason::kTerminalOutcomeMismatch);
    }
    case EventType::kResourcesReleased: {
      if (!IsTerminal(s.state)) return reject_state();
      if (!s.process_exit_confirmed) return Reject(RejectionReason::kInvariantViolation);
      const auto* p = std::get_if<ResourcesReleasedPayload>(&e.payload);
      if (!p || !s.allocation_id || !s.allocation_digest || p->allocation_id != *s.allocation_id ||
          p->allocation_digest != *s.allocation_digest)
        return Reject(RejectionReason::kInvariantViolation);
      return s.resource_status == ResourceStatus::kCommitted ||
                     s.resource_status == ResourceStatus::kReleased
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    }
    case EventType::kCleanupStatusRecorded: {
      if (!IsTerminal(s.state)) return reject_state();
      const auto* p = std::get_if<CleanupStatusPayload>(&e.payload);
      return p && (s.cleanup_status == CleanupStatus::kPending || s.cleanup_status == p->status)
                 ? Accept(s, e.event_type, e.payload)
                 : Reject(RejectionReason::kInvariantViolation);
    }
    case EventType::kLateWorkerEvent:
      return (IsTerminal(s.state) || s.state == JobState::kFinalizing)
                 ? Accept(s, e.event_type, e.payload)
                 : reject_state();
    case EventType::kJobCreated:
      return Reject(RejectionReason::kJobAlreadyExists);
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
  }
  return false;
}
}  // namespace

Snapshot InitialSnapshot(const Uuid& job_id, const Uuid& session_id) {
  Snapshot result;
  result.job_id = job_id;
  result.session_id = session_id;
  result.entity_exists = false;
  return result;
}
Decision DecideCommand(const Snapshot& s, const Command& command) {
  if (!ValidSnapshot(s)) return Reject(RejectionReason::kInvariantViolation);
  if (command.schema_version != 1 || command.job_id != s.job_id || !s.entity_exists)
    return Reject(RejectionReason::kJobNotFound);
  if (!IsUuid(command.job_id.value, '7') || command.principal_subject.empty() ||
      command.principal_subject.size() > 256 || static_cast<int>(command.command_type) < 0 ||
      static_cast<int>(command.command_type) > 1)
    return Reject(RejectionReason::kInvalidEventPayload);
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

ApplyResult Apply(const Snapshot& before, const PreEnvelopeProposal& proposal) {
  if (!ValidSnapshot(before))
    return ApplyResult{before, {}, Rejection{1, RejectionReason::kInvariantViolation}};
  if (proposal.schema_version != 1 || proposal.job_id != before.job_id ||
      !PayloadMatches(proposal.event_type, proposal.payload))
    return ApplyResult{before, {}, Rejection{1, RejectionReason::kInvalidEventPayload}};
  Snapshot s = before;
  std::vector<Effect> effects;
  const auto add_ack = [&] {
    s.pending_worker_event_ack = true;
    if (const auto* p = std::get_if<WorkerEventPayload>(&proposal.payload)) {
      s.pending_worker_id = p->worker_id;
      s.pending_worker_event_sequence = p->event_sequence;
    }
  };
  switch (proposal.event_type) {
    case EventType::kJobCreated: {
      const auto& p = std::get<JobCreatedPayload>(proposal.payload);
      s.entity_exists = true;
      s.state = JobState::kAdmitted;
      s.session_id = p.session_id;
      s.latched_reason.reset();
      s.completion_candidate = false;
      s.completion_mode = CompletionMode::kNone;
      s.resource_status = ResourceStatus::kNone;
      s.allocation_id.reset();
      s.allocation_digest.reset();
      s.worker_launch_status = LaunchStatus::kNotStarted;
      s.launch_operation_id.reset();
      s.worker_id.reset();
      s.process_presence = ProcessPresence::kAbsent;
      s.process_exit_confirmed = false;
      s.session_retention_status = RetentionStatus::kNotStarted;
      s.finalization_status = FinalizationStatus::kNotStarted;
      s.cleanup_status = CleanupStatus::kPending;
      s.pending_worker_event_ack = false;
      s.pending_worker_id.reset();
      s.pending_worker_event_sequence.reset();
      break;
    }
    case EventType::kResourcesCommitted: {
      const auto& p = std::get<ResourcesCommittedPayload>(proposal.payload);
      s.state = JobState::kPreparing;
      s.resource_status = ResourceStatus::kCommitted;
      s.allocation_id = p.allocation_id;
      s.allocation_digest = p.allocation_digest;
      Add(&effects, EffectId::kArmPreparationTimeout);
      break;
    }
    case EventType::kWorkerLaunchIntent: {
      const auto& p = std::get<WorkerLaunchIntentPayload>(proposal.payload);
      s.worker_launch_status = LaunchStatus::kIntentRecorded;
      s.launch_operation_id = p.operation_id;
      s.worker_id = p.worker_id;
      s.process_presence = ProcessPresence::kUnknown;
      Add(&effects, EffectId::kLaunchWorkerOnce);
      break;
    }
    case EventType::kWorkerLaunchObserved: {
      const auto& p = std::get<WorkerLaunchObservedPayload>(proposal.payload);
      s.worker_launch_status = p.started ? LaunchStatus::kObserved : LaunchStatus::kFailed;
      s.process_presence = p.started ? ProcessPresence::kPresent : ProcessPresence::kAbsent;
      if (!p.started) {
        s.latched_reason = TerminalOutcome::kFailed;
        s.completion_mode = CompletionMode::kProcessAlreadyExited;
        s.process_exit_confirmed = true;
        BeginFinalizing(&s);
        Add(&effects, EffectId::kDisarmPreparationTimeout);
      }
      break;
    }
    case EventType::kWorkerRunning:
      s.state = JobState::kRunning;
      Add(&effects, EffectId::kDisarmPreparationTimeout);
      Add(&effects, EffectId::kArmExecutionTimeout);
      break;
    case EventType::kCancelAccepted:
      s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kCancelled);
      if (s.process_presence == ProcessPresence::kAbsent) {
        s.completion_mode = CompletionMode::kProcessAlreadyExited;
        s.process_exit_confirmed = true;
        if (s.state == JobState::kPreparing) Add(&effects, EffectId::kDisarmPreparationTimeout);
        BeginFinalizing(&s);
      } else {
        s.completion_mode = CompletionMode::kCooperative;
        if (s.state == JobState::kPreparing) Add(&effects, EffectId::kDisarmPreparationTimeout);
        s.state = JobState::kStopping;
        Add(&effects, EffectId::kRequestCooperativeStop);
        Add(&effects, EffectId::kArmCooperativeStopTimeout);
      }
      break;
    case EventType::kTerminateAccepted:
      if (!s.completion_candidate)
        s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kTerminated);
      if (s.process_presence == ProcessPresence::kAbsent) {
        s.completion_mode = CompletionMode::kProcessAlreadyExited;
        s.process_exit_confirmed = true;
        if (s.state == JobState::kPreparing) Add(&effects, EffectId::kDisarmPreparationTimeout);
        BeginFinalizing(&s);
      } else {
        s.completion_mode = CompletionMode::kForced;
        if (s.state == JobState::kPreparing) Add(&effects, EffectId::kDisarmPreparationTimeout);
        if (s.state != JobState::kFinalizing) s.state = JobState::kStopping;
        Add(&effects, EffectId::kRequestForcedStop);
        Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
        Add(&effects, EffectId::kArmProcessExitConfirmationTimeoutIfNeeded);
      }
      break;
    case EventType::kTimeoutExpired: {
      const auto& p = std::get<TimeoutExpiredPayload>(proposal.payload);
      if (p.phase == TimeoutPhase::kPreparation) {
        s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kTimedOut);
        Add(&effects, EffectId::kDisarmPreparationTimeout);
        if (s.process_presence == ProcessPresence::kAbsent) {
          s.completion_mode = CompletionMode::kProcessAlreadyExited;
          s.process_exit_confirmed = true;
          BeginFinalizing(&s);
        } else {
          s.completion_mode = CompletionMode::kCooperative;
          s.state = JobState::kStopping;
          Add(&effects, EffectId::kRequestCooperativeStop);
          Add(&effects, EffectId::kArmCooperativeStopTimeout);
        }
      } else if (p.phase == TimeoutPhase::kExecution && s.state == JobState::kFinalizing) {
        const bool had_success_candidate = !s.latched_reason && s.completion_candidate;
        if (had_success_candidate) {
          s.latched_reason = TerminalOutcome::kTimedOut;
          s.completion_candidate = false;
        }
        if (s.process_exit_confirmed && had_success_candidate)
          s.completion_mode = CompletionMode::kProcessAlreadyExited;
        else if (!s.process_exit_confirmed) {
          s.completion_mode = CompletionMode::kForced;
          Add(&effects, EffectId::kRequestForcedStop);
          Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
          Add(&effects, EffectId::kArmProcessExitConfirmationTimeoutIfNeeded);
        }
      } else if (p.phase == TimeoutPhase::kExecution && s.state == JobState::kStopping) {
        s.completion_mode = CompletionMode::kForced;
        Add(&effects, EffectId::kRequestForcedStop);
        Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
        Add(&effects, EffectId::kArmProcessExitConfirmationTimeoutIfNeeded);
      } else if (p.phase == TimeoutPhase::kExecution && s.state == JobState::kRunning) {
        s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kTimedOut);
        s.completion_mode = CompletionMode::kCooperative;
        s.state = JobState::kStopping;
        Add(&effects, EffectId::kRequestCooperativeStop);
        Add(&effects, EffectId::kArmCooperativeStopTimeout);
      } else if (p.phase == TimeoutPhase::kCooperativeStop) {
        s.completion_mode = CompletionMode::kForced;
        Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
        Add(&effects, EffectId::kRequestForcedStop);
        Add(&effects, EffectId::kArmProcessExitConfirmationTimeoutIfNeeded);
      } else if (p.phase == TimeoutPhase::kProcessExitConfirmation) {
        s.cleanup_status = CleanupStatus::kIncomplete;
        Add(&effects, EffectId::kRequestForcedStop);
        Add(&effects, EffectId::kQuarantineResources);
        Add(&effects, EffectId::kSetReadinessFalse);
      }
      break;
    }
    case EventType::kWorkerCompleted: {
      s.completion_candidate = !s.latched_reason;
      if (s.state == JobState::kStopping) Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
      BeginFinalizing(&s);
      add_ack();
      break;
    }
    case EventType::kWorkerFailed: {
      s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kFailed);
      if (s.state == JobState::kPreparing) Add(&effects, EffectId::kDisarmPreparationTimeout);
      if (s.state == JobState::kStopping) Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
      BeginFinalizing(&s);
      add_ack();
      break;
    }
    case EventType::kProcessExitConfirmed: {
      const auto& p = std::get<ProcessExitConfirmedPayload>(proposal.payload);
      const auto old_state = s.state;
      s.process_exit_confirmed = true;
      s.process_presence = ProcessPresence::kAbsent;
      s.completion_mode = p.completion_mode;
      const bool had_ack = s.pending_worker_event_ack;
      s.pending_worker_event_ack = false;
      s.pending_worker_id.reset();
      s.pending_worker_event_sequence.reset();
      if (old_state == JobState::kPreparing || old_state == JobState::kRunning) {
        Add(&effects, EffectId::kDisarmPreparationTimeout);
        s.completion_mode = CompletionMode::kProcessAlreadyExited;
        s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kFailed);
        BeginFinalizing(&s);
      }
      if (old_state == JobState::kStopping || old_state == JobState::kFinalizing ||
          IsTerminal(old_state)) {
        Add(&effects, EffectId::kDisarmCooperativeStopTimeout);
        Add(&effects, EffectId::kDisarmProcessExitConfirmationTimeout);
      }
      if (had_ack) Add(&effects, EffectId::kAckTerminalWorkerEventIfPending);
      if (old_state == JobState::kStopping) BeginFinalizing(&s);
      break;
    }
    case EventType::kSessionRetainRequested:
      s.session_retention_status = RetentionStatus::kRequested;
      Add(&effects, EffectId::kRetainSessionSameIdentity);
      break;
    case EventType::kSessionRetained:
      s.session_retention_status = RetentionStatus::kRetained;
      break;
    case EventType::kFinalizationCompleted:
      s.finalization_status = FinalizationStatus::kCompleted;
      break;
    case EventType::kFinalizationFailed:
      s.finalization_status = FinalizationStatus::kFailed;
      s.cleanup_status = CleanupStatus::kIncomplete;
      s.completion_candidate = false;
      s.latched_reason = s.latched_reason.value_or(TerminalOutcome::kFailed);
      break;
    case EventType::kTerminalOutcomeCommitted: {
      const auto& p = std::get<TerminalOutcomePayload>(proposal.payload);
      s.state = p.outcome == TerminalOutcome::kSucceeded    ? JobState::kSucceeded
                : p.outcome == TerminalOutcome::kFailed     ? JobState::kFailed
                : p.outcome == TerminalOutcome::kCancelled  ? JobState::kCancelled
                : p.outcome == TerminalOutcome::kTerminated ? JobState::kTerminated
                                                            : JobState::kTimedOut;
      s.finalization_status = s.finalization_status == FinalizationStatus::kFailed
                                  ? s.finalization_status
                                  : FinalizationStatus::kCompleted;
      Add(&effects, EffectId::kDisarmExecutionTimeout);
      if (s.pending_worker_event_ack) Add(&effects, EffectId::kAckTerminalWorkerEventIfPending);
      Add(&effects, EffectId::kPublishTerminalResult);
      Add(&effects, EffectId::kArmProcessExitConfirmationTimeoutIfNeeded);
      break;
    }
    case EventType::kResourcesReleased:
      s.resource_status = ResourceStatus::kReleased;
      break;
    case EventType::kCleanupStatusRecorded:
      s.cleanup_status = std::get<CleanupStatusPayload>(proposal.payload).status;
      break;
    case EventType::kLateWorkerEvent:
      Add(&effects, EffectId::kAckLateWorkerEvent);
      break;
  }
  return ApplyResult{std::move(s), std::move(effects), std::nullopt};
}

TimerIngressResult IngestTimer(const TimerState& timers, const TimerIngressInput& input) {
  return input.notification
             ? IngestTimer(timers, *input.notification)
             : TimerIngressResult{TimerIngressKind::kDiscardWithoutCandidate, std::nullopt, {}};
}
TimerIngressResult IngestTimer(const TimerState& timers, const TimerNotification& notification) {
  const auto phase = notification.phase;
  const auto generation = notification.generation;
  const bool exhausted =
      (phase == TimeoutPhase::kPreparation && timers.preparation_generation == UINT64_MAX) ||
      (phase == TimeoutPhase::kExecution && timers.execution_generation == UINT64_MAX) ||
      (phase == TimeoutPhase::kCooperativeStop &&
       timers.cooperative_stop_generation == UINT64_MAX) ||
      (phase == TimeoutPhase::kProcessExitConfirmation &&
       timers.process_exit_confirmation_generation == UINT64_MAX);
  if (exhausted)
    return TimerIngressResult{
        TimerIngressKind::kFailClosed, std::nullopt, {Effect{EffectId::kSetReadinessFalse}}};
  bool armed = false;
  std::uint64_t active = 0;
  if (phase == TimeoutPhase::kPreparation) {
    armed = timers.preparation_armed;
    active = timers.preparation_generation;
  }
  if (phase == TimeoutPhase::kExecution) {
    armed = timers.execution_armed;
    active = timers.execution_generation;
  }
  if (phase == TimeoutPhase::kCooperativeStop) {
    armed = timers.cooperative_stop_armed;
    active = timers.cooperative_stop_generation;
  }
  if (phase == TimeoutPhase::kProcessExitConfirmation) {
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
