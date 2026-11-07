#include "job_supervisor.hpp"

namespace sitometron::core::internal {
namespace {
SupervisorDecision FinalizingDecision(const Snapshot& s) {
  using enum RetentionStatus;
  if (s.session_retention_status == kNotStarted)
    return {6, "Submit", "session_retain_requested", "bound_session", {}};
  if (s.session_retention_status == kRequested)
    return {7, "Wait", {}, "none", {"session_retainer"}};
  if (s.session_retention_status == kRetained &&
      s.finalization_status == FinalizationStatus::kPending)
    return {8, "Submit", "finalization_completed", "empty", {}};
  if (s.finalization_status == FinalizationStatus::kCompleted ||
      s.finalization_status == FinalizationStatus::kFailed)
    return {9, "Submit", "terminal_outcome_committed", "latched_reason_or_success_candidate", {}};
  return {13, "Done", {}, "none", {}};
}
SupervisorDecision TerminalDecision(const Snapshot& s) {
  if (!s.process_exit_confirmed && s.cleanup_status == CleanupStatus::kPending)
    return {10, "Wait", {}, "none", {"application_runner", "timer_port"}};
  if (s.process_exit_confirmed && s.resource_status == ResourceStatus::kCommitted)
    return {11, "Submit", "resources_released", "bound_allocation", {}};
  if (s.cleanup_status == CleanupStatus::kPending)
    return {12, "Submit", "cleanup_status_recorded", "cleanup_completed", {}};
  return {13, "Done", {}, "none", {}};
}
}  // namespace
SupervisorDecision DecideSupervisor(const Snapshot* snapshot, bool launch) {
  if (snapshot == nullptr || !snapshot->entity_exists) return {};
  const auto& s = *snapshot;
  if (s.state == JobState::kAdmitted)
    return {2, "Submit", "resources_committed", "fixed_empty_allocation", {}};
  if (s.state == JobState::kPreparing && s.worker_launch_status == LaunchStatus::kNotStarted) {
    if (!launch) return {3, "Done", {}, "none", {}};
    return {4,
            "Submit",
            "worker_launch_intent",
            "creation_launch_identity_and_committed_allocation",
            {}};
  }
  if (s.state == JobState::kPreparing || s.state == JobState::kRunning ||
      s.state == JobState::kStopping)
    return {
        5, "Wait", {}, "none", {"application_runner", "worker_protocol", "timer_port", "commands"}};
  if (s.state == JobState::kFinalizing) return FinalizingDecision(s);
  if (IsTerminalState(s.state)) return TerminalDecision(s);
  return {13, "Done", {}, "none", {}};
}
}  // namespace sitometron::core::internal
