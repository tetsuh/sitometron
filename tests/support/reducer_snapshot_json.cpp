#include "reducer_snapshot_json.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sitometron::test {
using namespace sitometron::core;
using namespace std::string_view_literals;
using Json = nlohmann::json;

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

Snapshot SnapshotFrom(const Json& j, const Json* absent_input) {
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

}  // namespace sitometron::test
