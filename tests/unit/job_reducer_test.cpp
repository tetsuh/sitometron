#include "sitometron/core/job_reducer.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sitometron::test {
int RunJobReducerVectorChecks(const char* path, std::string_view selector);
int RunJobReducerVectorTextChecks(std::string_view text, const char* selector);
int RunJobReducerParityMutationChecks(const char* path);
int RunJobReducerParityMutationTextChecks(std::string_view text);
int RunJobReducerTrailingArtifactChecks(const char* path);
int RunJobReducerClosedEnumArtifactChecks();
}  // namespace sitometron::test

namespace {
using namespace sitometron::core;
constexpr std::string_view k_job = "01890f3e-7b00-7abc-8abc-0123456789ab";
constexpr std::string_view k_worker = "123e4567-e89b-42d3-a456-426614174000";

int Check(bool condition, std::string_view message) {
  if (condition) return 0;
  std::cerr << "job_reducer: " << message << '\n';
  return 1;
}

bool SameSnapshot(const Snapshot& lhs, const Snapshot& rhs) {
  return lhs.schema_version == rhs.schema_version && lhs.job_id == rhs.job_id &&
         lhs.session_id == rhs.session_id && lhs.entity_exists == rhs.entity_exists &&
         lhs.state == rhs.state && lhs.latched_reason == rhs.latched_reason &&
         lhs.completion_candidate == rhs.completion_candidate &&
         lhs.completion_mode == rhs.completion_mode && lhs.resource_status == rhs.resource_status &&
         lhs.allocation_id == rhs.allocation_id && lhs.allocation_digest == rhs.allocation_digest &&
         lhs.worker_launch_status == rhs.worker_launch_status &&
         lhs.launch_operation_id == rhs.launch_operation_id && lhs.worker_id == rhs.worker_id &&
         lhs.process_presence == rhs.process_presence &&
         lhs.process_exit_confirmed == rhs.process_exit_confirmed &&
         lhs.session_retention_status == rhs.session_retention_status &&
         lhs.finalization_status == rhs.finalization_status &&
         lhs.cleanup_status == rhs.cleanup_status &&
         lhs.pending_worker_event_ack == rhs.pending_worker_event_ack &&
         lhs.pending_worker_id == rhs.pending_worker_id &&
         lhs.pending_worker_event_sequence == rhs.pending_worker_event_sequence;
}

bool IsRejection(const Decision& decision, RejectionReason reason) {
  return std::holds_alternative<Rejection>(decision.value) &&
         std::get<Rejection>(decision.value).reason == reason;
}

int RunUtf8ScalarBoundaryMatrix() {
  struct Utf8Case {
    std::string_view label;
    std::string_view value;
    bool valid;
  };
  constexpr std::array<Utf8Case, 28> k_cases{{
      {"ASCII scalar", std::string_view{"A", 1}, true},
      {"two-byte scalar", std::string_view{"\xC2\xA2", 2}, true},
      {"three-byte scalar", std::string_view{"\xE3\x81\x82", 3}, true},
      {"four-byte scalar", std::string_view{"\xF0\x9F\x98\x80", 4}, true},
      {"lowest three-byte scalar", std::string_view{"\xE0\xA0\x80", 3}, true},
      {"scalar before surrogate range", std::string_view{"\xED\x9F\xBF", 3}, true},
      {"lowest four-byte scalar", std::string_view{"\xF0\x90\x80\x80", 4}, true},
      {"maximum Unicode scalar", std::string_view{"\xF4\x8F\xBF\xBF", 4}, true},
      {"embedded NUL scalar", std::string_view{"A\0B", 3}, true},
      {"empty string", std::string_view{}, false},
      {"overlong two-byte scalar", std::string_view{"\xC0\xAF", 2}, false},
      {"overlong three-byte scalar", std::string_view{"\xE0\x80\xAF", 3}, false},
      {"overlong four-byte scalar", std::string_view{"\xF0\x80\x80\xAF", 4}, false},
      {"isolated continuation", std::string_view{"\x80", 1}, false},
      {"truncated two-byte scalar", std::string_view{"\xC2", 1}, false},
      {"truncated three-byte scalar", std::string_view{"\xE3\x81", 2}, false},
      {"truncated four-byte scalar", std::string_view{"\xF0\x9F\x98", 3}, false},
      {"invalid two-byte continuation", std::string_view{"\xC2\x41", 2}, false},
      {"invalid three-byte second byte", std::string_view{"\xE3\x41\x82", 3}, false},
      {"invalid three-byte later byte", std::string_view{"\xE3\x81\x41", 3}, false},
      {"invalid four-byte second byte", std::string_view{"\xF0\x41\x98\x80", 4}, false},
      {"invalid four-byte third byte", std::string_view{"\xF0\x9F\x41\x80", 4}, false},
      {"invalid four-byte fourth byte", std::string_view{"\xF0\x9F\x98\x41", 4}, false},
      {"UTF-8 encoded surrogate", std::string_view{"\xED\xA0\x80", 3}, false},
      {"code point above U+10FFFF", std::string_view{"\xF4\x90\x80\x80", 4}, false},
      {"invalid F5 lead byte", std::string_view{"\xF5\x80\x80\x80", 4}, false},
      {"invalid FF lead byte", std::string_view{"\xFF", 1}, false},
      {"isolated maximum continuation", std::string_view{"\xBF", 1}, false},
  }};

  const Uuid job{std::string(k_job)};
  Snapshot admitted = InitialSnapshot(job, job);
  admitted.entity_exists = true;
  Snapshot preparing = admitted;
  preparing.state = JobState::kPreparing;
  preparing.resource_status = ResourceStatus::kCommitted;
  preparing.allocation_id = StableId{"allocation-1"};
  preparing.allocation_digest =
      Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};

  const auto principal_decision = [&](std::string value) {
    return DecideCommand(admitted, Command{1, CommandType::kCancel, job, std::move(value)});
  };
  const auto version_decision = [&](std::string value) {
    return DecideEvent(
        preparing,
        InternalEvent{
            1, job, EventType::kWorkerLaunchIntent,
            WorkerLaunchIntentPayload{
                StableId{"launch-op-1"}, StableId{"application-1"}, std::move(value),
                Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"},
                StableId{"allocation-1"},
                Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"},
                Uuid{std::string(k_worker)}}});
  };

  int result = 0;
  for (const auto& test : k_cases) {
    const auto principal = principal_decision(std::string(test.value));
    const auto version = version_decision(std::string(test.value));
    const bool principal_valid = std::holds_alternative<PreEnvelopeProposal>(principal.value);
    const bool version_valid = std::holds_alternative<PreEnvelopeProposal>(version.value);
    result |=
        Check(principal_valid == test.valid, std::string(test.label) + " principal UTF-8 boundary");
    result |= Check(version_valid == test.valid,
                    std::string(test.label) + " application-version UTF-8 boundary");
    if (!test.valid) {
      result |= Check(IsRejection(principal, RejectionReason::kInvalidEventPayload),
                      std::string(test.label) + " principal rejection reason");
      result |= Check(IsRejection(version, RejectionReason::kInvalidEventPayload),
                      std::string(test.label) + " application-version rejection reason");
    }
  }

  struct ScalarLimitCase {
    std::size_t count;
    bool principal_valid;
    bool version_valid;
  };
  constexpr std::array<ScalarLimitCase, 4> k_limits{{
      {128, true, true},
      {129, true, false},
      {256, true, false},
      {257, false, false},
  }};
  for (const auto& test : k_limits) {
    std::string value;
    value.reserve(test.count * 2);
    for (std::size_t index = 0; index < test.count; ++index) value += "\xC2\xA2";
    const bool principal_valid =
        std::holds_alternative<PreEnvelopeProposal>(principal_decision(value).value);
    const bool version_valid =
        std::holds_alternative<PreEnvelopeProposal>(version_decision(value).value);
    result |=
        Check(principal_valid == test.principal_valid, "principal Unicode scalar-count boundary");
    result |= Check(version_valid == test.version_valid,
                    "application-version Unicode scalar-count boundary");
  }
  return result;
}

int RunSnapshotInvariantFailClosedMatrix() {
  const Uuid job{std::string(k_job)};
  const auto admitted = [&] {
    Snapshot snapshot = InitialSnapshot(job, job);
    snapshot.entity_exists = true;
    return snapshot;
  };
  const auto preparing = [&] {
    Snapshot snapshot = admitted();
    snapshot.state = JobState::kPreparing;
    snapshot.resource_status = ResourceStatus::kCommitted;
    snapshot.allocation_id = StableId{"allocation-1"};
    snapshot.allocation_digest =
        Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
    snapshot.worker_launch_status = LaunchStatus::kIntentRecorded;
    snapshot.launch_operation_id = StableId{"launch-op-1"};
    snapshot.worker_id = Uuid{std::string(k_worker)};
    snapshot.process_presence = ProcessPresence::kUnknown;
    return snapshot;
  };
  const auto running = [&] {
    Snapshot snapshot = preparing();
    snapshot.state = JobState::kRunning;
    snapshot.worker_launch_status = LaunchStatus::kObserved;
    snapshot.process_presence = ProcessPresence::kPresent;
    return snapshot;
  };
  const auto finalizing = [&] {
    Snapshot snapshot = running();
    snapshot.state = JobState::kFinalizing;
    snapshot.finalization_status = FinalizationStatus::kPending;
    return snapshot;
  };
  const auto terminal = [&] {
    Snapshot snapshot = finalizing();
    snapshot.state = JobState::kFailed;
    snapshot.latched_reason = TerminalOutcome::kFailed;
    snapshot.finalization_status = FinalizationStatus::kCompleted;
    return snapshot;
  };

  struct SnapshotCase {
    std::string_view label;
    Snapshot snapshot;
  };
  std::vector<SnapshotCase> cases;
  cases.reserve(48);
  const auto add = [&](std::string_view label, Snapshot snapshot, const auto& mutate) {
    mutate(snapshot);
    cases.push_back(SnapshotCase{label, std::move(snapshot)});
  };

  add("schema version", admitted(), [](Snapshot& s) { s.schema_version = 0; });
  add("Job UUID", admitted(), [](Snapshot& s) { s.job_id = Uuid{"not-a-uuid"}; });
  add("Session UUID", admitted(), [](Snapshot& s) { s.session_id = Uuid{"not-a-uuid"}; });
  add("Job and Session identity", admitted(),
      [](Snapshot& s) { s.session_id = Uuid{"01890f3e-7b00-7abc-8abc-0123456789ac"}; });
  add("Job state enum", admitted(), [](Snapshot& s) { s.state = static_cast<JobState>(255); });
  add("completion mode enum", admitted(),
      [](Snapshot& s) { s.completion_mode = static_cast<CompletionMode>(255); });
  add("resource status enum", admitted(),
      [](Snapshot& s) { s.resource_status = static_cast<ResourceStatus>(255); });
  add("launch status enum", admitted(),
      [](Snapshot& s) { s.worker_launch_status = static_cast<LaunchStatus>(255); });
  add("process presence enum", admitted(),
      [](Snapshot& s) { s.process_presence = static_cast<ProcessPresence>(255); });
  add("retention status enum", admitted(),
      [](Snapshot& s) { s.session_retention_status = static_cast<RetentionStatus>(255); });
  add("finalization status enum", admitted(),
      [](Snapshot& s) { s.finalization_status = static_cast<FinalizationStatus>(255); });
  add("cleanup status enum", admitted(),
      [](Snapshot& s) { s.cleanup_status = static_cast<CleanupStatus>(255); });
  add("successful latched reason", admitted(),
      [](Snapshot& s) { s.latched_reason = TerminalOutcome::kSucceeded; });
  add("invalid latched reason", admitted(),
      [](Snapshot& s) { s.latched_reason = TerminalOutcome::kInvalid; });
  add("allocation ID domain", preparing(),
      [](Snapshot& s) { s.allocation_id = StableId{"invalid allocation"}; });
  add("allocation digest domain", preparing(),
      [](Snapshot& s) { s.allocation_digest = Digest{"invalid"}; });
  add("launch operation ID domain", preparing(),
      [](Snapshot& s) { s.launch_operation_id = StableId{"invalid operation"}; });
  add("Worker UUID domain", preparing(),
      [](Snapshot& s) { s.worker_id = Uuid{"not-a-worker-uuid"}; });
  add("pending Worker UUID domain", finalizing(), [](Snapshot& s) {
    s.pending_worker_event_ack = true;
    s.pending_worker_id = Uuid{"not-a-worker-uuid"};
    s.pending_worker_event_sequence = 1;
  });
  add("pending Worker sequence domain", finalizing(), [](Snapshot& s) {
    s.pending_worker_event_ack = true;
    s.pending_worker_id = Uuid{std::string(k_worker)};
    s.pending_worker_event_sequence = 0;
  });
  add("resource none with allocation", admitted(), [](Snapshot& s) {
    s.allocation_id = StableId{"allocation-1"};
    s.allocation_digest =
        Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
  });
  add("committed resource without allocation ID", preparing(),
      [](Snapshot& s) { s.allocation_id.reset(); });
  add("committed resource without allocation digest", preparing(),
      [](Snapshot& s) { s.allocation_digest.reset(); });
  add("released resource without confirmed exit", preparing(),
      [](Snapshot& s) { s.resource_status = ResourceStatus::kReleased; });
  add("confirmed exit with live process", finalizing(), [](Snapshot& s) {
    s.process_exit_confirmed = true;
    s.completion_mode = CompletionMode::kForced;
  });
  add("confirmed exit with pending ACK", finalizing(), [](Snapshot& s) {
    s.process_exit_confirmed = true;
    s.process_presence = ProcessPresence::kAbsent;
    s.completion_mode = CompletionMode::kForced;
    s.pending_worker_event_ack = true;
    s.pending_worker_id = Uuid{std::string(k_worker)};
    s.pending_worker_event_sequence = 1;
  });
  add("already-exited completion without confirmed exit", finalizing(),
      [](Snapshot& s) { s.completion_mode = CompletionMode::kProcessAlreadyExited; });
  add("not-started launch with bindings", preparing(),
      [](Snapshot& s) { s.worker_launch_status = LaunchStatus::kNotStarted; });
  add("started launch without operation", preparing(),
      [](Snapshot& s) { s.launch_operation_id.reset(); });
  add("started launch without Worker", preparing(), [](Snapshot& s) { s.worker_id.reset(); });
  add("pending identity without ACK", finalizing(),
      [](Snapshot& s) { s.pending_worker_id = Uuid{std::string(k_worker)}; });
  add("pending sequence without ACK", finalizing(),
      [](Snapshot& s) { s.pending_worker_event_sequence = 1; });
  add("ACK without pending identity", finalizing(), [](Snapshot& s) {
    s.pending_worker_event_ack = true;
    s.pending_worker_event_sequence = 1;
  });
  add("ACK without pending sequence", finalizing(), [](Snapshot& s) {
    s.pending_worker_event_ack = true;
    s.pending_worker_id = Uuid{std::string(k_worker)};
  });
  add("admitted resources", admitted(), [](Snapshot& s) {
    s.resource_status = ResourceStatus::kCommitted;
    s.allocation_id = StableId{"allocation-1"};
    s.allocation_digest =
        Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
  });
  add("admitted launch", admitted(), [](Snapshot& s) {
    s.worker_launch_status = LaunchStatus::kIntentRecorded;
    s.launch_operation_id = StableId{"launch-op-1"};
    s.worker_id = Uuid{std::string(k_worker)};
  });
  add("admitted process", admitted(),
      [](Snapshot& s) { s.process_presence = ProcessPresence::kPresent; });
  add("admitted confirmed exit", admitted(), [](Snapshot& s) {
    s.process_exit_confirmed = true;
    s.completion_mode = CompletionMode::kForced;
  });
  add("admitted finalization", admitted(),
      [](Snapshot& s) { s.finalization_status = FinalizationStatus::kPending; });
  add("preparing finalization", preparing(),
      [](Snapshot& s) { s.finalization_status = FinalizationStatus::kPending; });
  add("running finalization", running(),
      [](Snapshot& s) { s.finalization_status = FinalizationStatus::kPending; });
  add("stopping finalization", running(), [](Snapshot& s) {
    s.state = JobState::kStopping;
    s.finalization_status = FinalizationStatus::kPending;
  });
  add("finalizing without finalization fact", finalizing(),
      [](Snapshot& s) { s.finalization_status = FinalizationStatus::kNotStarted; });
  add("terminal without completed finalization", terminal(),
      [](Snapshot& s) { s.finalization_status = FinalizationStatus::kPending; });

  const Command command{1, CommandType::kCancel, job, "operator@example"};
  const PreEnvelopeProposal proposal{1, job, EventType::kCancelAccepted,
                                     PrincipalPayload{"operator@example"}};
  struct ValidSnapshotCase {
    std::string_view label;
    Snapshot snapshot;
  };
  const std::array<ValidSnapshotCase, 5> valid_fixtures{{
      {"admitted", admitted()},
      {"preparing", preparing()},
      {"running", running()},
      {"finalizing", finalizing()},
      {"terminal", terminal()},
  }};
  int result = 0;
  for (const auto& fixture : valid_fixtures) {
    result |= Check(!IsRejection(DecideCommand(fixture.snapshot, command),
                                 RejectionReason::kInvariantViolation),
                    std::string(fixture.label) + " Snapshot fixture is valid for decisions");
    const auto applied = Apply(fixture.snapshot, proposal);
    result |= Check(!applied.rejection.has_value() ||
                        applied.rejection->reason != RejectionReason::kInvariantViolation,
                    std::string(fixture.label) + " Snapshot fixture is valid for apply");
  }
  for (const auto& test : cases) {
    result |= Check(
        IsRejection(DecideCommand(test.snapshot, command), RejectionReason::kInvariantViolation),
        std::string(test.label) + " command fails closed");
    const auto applied = Apply(test.snapshot, proposal);
    result |= Check(applied.rejection.has_value() &&
                        applied.rejection->reason == RejectionReason::kInvariantViolation &&
                        applied.effects.empty() && SameSnapshot(applied.snapshot, test.snapshot),
                    std::string(test.label) + " apply fails closed without mutation");
  }
  return result;
}

int RunMalformedArtifactChecks() {
  int result = 0;
  const auto check_artifact = [&](std::string_view contents, std::string_view label) {
    const auto normal = sitometron::test::RunJobReducerVectorTextChecks(contents, "all");
    const auto mutation = sitometron::test::RunJobReducerParityMutationTextChecks(contents);
    int checks = 0;
    checks |= Check(normal != 0, std::string(label) + " normal runner rejects artifact");
    checks |= Check(mutation != 0, std::string(label) + " mutation runner rejects artifact");
    return checks;
  };
  result |= check_artifact("{", "syntactically invalid JSON");
  result |= check_artifact(R"({"contract_version":1})", "missing case_vectors");
  result |= sitometron::test::RunJobReducerClosedEnumArtifactChecks();
  return result;
}

int Run() {
  int result = RunUtf8ScalarBoundaryMatrix();
  result |= RunSnapshotInvariantFailClosedMatrix();
  const Uuid job{std::string(k_job)};
  Snapshot absent = InitialSnapshot(job, job);
  result |= Check(!absent.entity_exists, "initial entity position is absent");
  Command default_command;
  default_command.job_id = job;
  default_command.principal_subject = "operator@example";
  Snapshot present = absent;
  present.entity_exists = true;
  result |=
      Check(std::holds_alternative<Rejection>(DecideCommand(present, default_command).value) &&
                std::get<Rejection>(DecideCommand(present, default_command).value).reason ==
                    RejectionReason::kInvalidEventPayload,
            "default command enum fails closed");
  const auto absent_invalid_command = DecideCommand(absent, default_command);
  result |= Check(std::holds_alternative<Rejection>(absent_invalid_command.value) &&
                      std::get<Rejection>(absent_invalid_command.value).reason ==
                          RejectionReason::kInvalidEventPayload,
                  "invalid command DTO is rejected before entity lookup");
  const auto principal_decision = [&](std::string principal) {
    return DecideCommand(present, Command{1, CommandType::kCancel, job, std::move(principal)});
  };
  std::string principal_256;
  for (std::size_t index = 0; index < 256; ++index) principal_256 += "\xC3\xA9";
  result |=
      Check(std::holds_alternative<PreEnvelopeProposal>(principal_decision(principal_256).value),
            "principal accepts 256 Unicode scalars");
  std::string principal_257 = principal_256 + "\xC3\xA9";
  result |= Check(std::holds_alternative<Rejection>(principal_decision(principal_257).value),
                  "principal rejects 257 Unicode scalars");
  result |= Check(std::holds_alternative<Rejection>(
                      principal_decision(std::string{"\xF0\x28\x8C\x28", 4}).value),
                  "principal rejects malformed UTF-8");
  InternalEvent default_event;
  default_event.schema_version = 1;
  default_event.job_id = job;
  const auto default_event_decision = DecideEvent(absent, default_event);
  result |= Check(std::holds_alternative<Rejection>(default_event_decision.value) &&
                      std::get<Rejection>(default_event_decision.value).reason ==
                          RejectionReason::kInvalidEventPayload,
                  "default internal event enum fails closed");
  const auto default_event_apply =
      Apply(absent, PreEnvelopeProposal{1, job, EventType::kInvalid, EmptyPayload{}});
  result |= Check(
      default_event_apply.rejection.has_value() &&
          default_event_apply.rejection->reason == RejectionReason::kInvalidEventPayload &&
          default_event_apply.snapshot.state == absent.state && default_event_apply.effects.empty(),
      "default invalid event proposal is rejected without mutation");
  result |= Check(ToString(Rejection{}.reason) == "invalid" && ToString(Effect{}.id) == "invalid",
                  "default rejection and effect enums stringify safely");
  result |= Check(TimerIngressResult{}.kind == TimerIngressKind::kInvalid,
                  "default timer result enum fails closed");
  const Snapshot default_snapshot;
  result |= Check(default_snapshot.state == JobState::kInvalid &&
                      default_snapshot.completion_mode == CompletionMode::kInvalid &&
                      default_snapshot.resource_status == ResourceStatus::kInvalid &&
                      default_snapshot.worker_launch_status == LaunchStatus::kInvalid &&
                      default_snapshot.process_presence == ProcessPresence::kInvalid &&
                      default_snapshot.session_retention_status == RetentionStatus::kInvalid &&
                      default_snapshot.finalization_status == FinalizationStatus::kInvalid &&
                      default_snapshot.cleanup_status == CleanupStatus::kInvalid &&
                      TimeoutExpiredPayload{}.phase == TimeoutPhase::kInvalid &&
                      ProcessExitConfirmedPayload{}.completion_mode == CompletionMode::kInvalid &&
                      TerminalOutcomePayload{}.outcome == TerminalOutcome::kInvalid &&
                      CleanupStatusPayload{}.status == CleanupStatus::kInvalid &&
                      LateWorkerEventPayload{}.original_event_type == EventType::kInvalid &&
                      TimerArmRequest{}.phase == TimeoutPhase::kInvalid &&
                      TimerNotification{}.phase == TimeoutPhase::kInvalid,
                  "default snapshot and payload enums fail closed");
  const Command valid_cancel{1, CommandType::kCancel, job, "operator@example"};
  Snapshot released_without_exit = present;
  released_without_exit.resource_status = ResourceStatus::kReleased;
  released_without_exit.allocation_id = StableId{"allocation-1"};
  released_without_exit.allocation_digest =
      Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
  const auto released_decision = DecideCommand(released_without_exit, valid_cancel);
  result |= Check(std::holds_alternative<Rejection>(released_decision.value) &&
                      std::get<Rejection>(released_decision.value).reason ==
                          RejectionReason::kInvariantViolation,
                  "released resources require confirmed process exit");
  const auto admitted_snapshot_rejected = [&](const Snapshot& invalid, std::string_view label) {
    const auto applied = Apply(invalid, PreEnvelopeProposal{1, job, EventType::kCancelAccepted,
                                                            PrincipalPayload{"operator@example"}});
    return Check(applied.rejection.has_value() &&
                     applied.rejection->reason == RejectionReason::kInvariantViolation &&
                     applied.effects.empty() && applied.snapshot.job_id == invalid.job_id &&
                     applied.snapshot.state == invalid.state &&
                     applied.snapshot.resource_status == invalid.resource_status &&
                     applied.snapshot.worker_launch_status == invalid.worker_launch_status &&
                     applied.snapshot.process_presence == invalid.process_presence &&
                     applied.snapshot.process_exit_confirmed == invalid.process_exit_confirmed,
                 label);
  };
  Snapshot admitted_resources = present;
  admitted_resources.resource_status = ResourceStatus::kCommitted;
  admitted_resources.allocation_id = StableId{"allocation-1"};
  admitted_resources.allocation_digest =
      Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"};
  result |= admitted_snapshot_rejected(admitted_resources, "admitted resources fail closed");
  Snapshot admitted_launch = present;
  admitted_launch.worker_launch_status = LaunchStatus::kIntentRecorded;
  admitted_launch.launch_operation_id = StableId{"launch-op-1"};
  admitted_launch.worker_id = Uuid{std::string(k_worker)};
  result |= admitted_snapshot_rejected(admitted_launch, "admitted Worker launch fails closed");
  Snapshot admitted_process = present;
  admitted_process.process_presence = ProcessPresence::kPresent;
  result |= admitted_snapshot_rejected(admitted_process, "admitted process presence fails closed");
  Snapshot admitted_exit = present;
  admitted_exit.process_exit_confirmed = true;
  result |= admitted_snapshot_rejected(admitted_exit, "admitted confirmed exit fails closed");
  const RawCandidateEvent created{1, job, "job_created",
                                  "{\"session_id\":\"" + std::string(k_job) + "\"}"};
  const auto normalized_created = NormalizeCandidate(absent, created);
  const Decision created_decision =
      std::holds_alternative<InternalEvent>(normalized_created.value)
          ? DecideEvent(absent, std::get<InternalEvent>(normalized_created.value))
          : Decision{std::get<Rejection>(normalized_created.value)};
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(created_decision.value),
                  "job_created validates");
  if (std::holds_alternative<PreEnvelopeProposal>(created_decision.value)) {
    const auto applied = Apply(absent, std::get<PreEnvelopeProposal>(created_decision.value));
    result |= Check(applied.snapshot.state == JobState::kAdmitted, "job_created applies admitted");
    absent = applied.snapshot;
  }
  const RawCandidateEvent allocation{1, job, "resources_committed",
                                     "{\"allocation_id\":\"allocation-1\",\"allocation_digest\":"
                                     "\"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61ca"
                                     "aff8a\",\"resolved_allocation\":{\"schema_id\":\"allocation."
                                     "v1\",\"schema_version\":1,\"payload_utf8\":\"{}\"}}"};
  const auto normalized_allocation = NormalizeCandidate(absent, allocation);
  const auto allocation_decision =
      std::holds_alternative<InternalEvent>(normalized_allocation.value)
          ? DecideEvent(absent, std::get<InternalEvent>(normalized_allocation.value))
          : Decision{std::get<Rejection>(normalized_allocation.value)};
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(allocation_decision.value),
                  "allocation validates byte-exactly");
  if (std::holds_alternative<PreEnvelopeProposal>(allocation_decision.value)) {
    absent = Apply(absent, std::get<PreEnvelopeProposal>(allocation_decision.value)).snapshot;
    result |= Check(absent.state == JobState::kPreparing, "allocation enters preparing");
  }
  const auto launch_intent_decision = [&](std::string application_version,
                                          std::string application_id = "application-1") {
    return DecideEvent(
        absent, InternalEvent{
                    1, job, EventType::kWorkerLaunchIntent,
                    WorkerLaunchIntentPayload{
                        StableId{"launch-op-1"}, StableId{std::move(application_id)},
                        std::move(application_version),
                        Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"},
                        StableId{"allocation-1"},
                        Digest{"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"},
                        Uuid{std::string(k_worker)}}});
  };
  std::string version_128;
  for (std::size_t index = 0; index < 128; ++index) version_128 += "\xF0\x9F\x98\x80";
  result |=
      Check(std::holds_alternative<PreEnvelopeProposal>(launch_intent_decision(version_128).value),
            "application version accepts 128 Unicode scalars");
  std::string version_129 = version_128 + "\xF0\x9F\x98\x80";
  result |= Check(std::holds_alternative<Rejection>(launch_intent_decision(version_129).value),
                  "application version rejects 129 Unicode scalars");
  result |= Check(std::holds_alternative<Rejection>(
                      launch_intent_decision(std::string{"\xED\xA0\x80", 3}).value),
                  "application version rejects UTF-8 encoded surrogate");
  result |= Check(std::holds_alternative<Rejection>(
                      launch_intent_decision("1", std::string{"\xC4\xAA", 2}).value),
                  "stable Application ID rejects non-ASCII letters");
  const auto raw_launch_intent = [&](const std::string& application_version) {
    const nlohmann::json payload{
        {"operation_id", "launch-op-1"},
        {"application",
         {{"application_id", "application-1"},
          {"version", application_version},
          {"bundle_sha256", "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"}}},
        {"allocation_id", "allocation-1"},
        {"allocation_digest", "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"},
        {"worker_id", std::string(k_worker)}};
    return NormalizeCandidate(absent,
                              RawCandidateEvent{1, job, "worker_launch_intent", payload.dump()});
  };
  result |= Check(std::holds_alternative<InternalEvent>(raw_launch_intent(version_128).value),
                  "raw application version accepts 128 Unicode scalars");
  result |= Check(std::holds_alternative<Rejection>(raw_launch_intent(version_129).value),
                  "raw application version rejects 129 Unicode scalars");
  const auto boundary_candidate = [&](std::size_t payload_size, std::string_view digest) {
    const std::string payload = "{\"x\":\"" + std::string(payload_size - 8, 'a') + "\"}";
    const nlohmann::json envelope{
        {"allocation_id", "allocation-1"},
        {"allocation_digest", digest},
        {"resolved_allocation",
         {{"schema_id", "allocation.v1"}, {"schema_version", 1}, {"payload_utf8", payload}}}};
    return RawCandidateEvent{1, job, "resources_committed", envelope.dump()};
  };
  const auto exact_boundary = NormalizeCandidate(
      absent, boundary_candidate(
                  65536, "26dd9ded7a4d8e5dbabcaf19f789b959aac342d54abe35c55e2351e04a6bbac1"));
  result |= Check(
      std::holds_alternative<InternalEvent>(exact_boundary.value) &&
          std::get<ResourcesCommittedPayload>(std::get<InternalEvent>(exact_boundary.value).payload)
                  .payload_utf8.size() == 65536,
      "resolved allocation payload accepts exactly 65536 UTF-8 bytes");
  const auto over_boundary = NormalizeCandidate(
      absent, boundary_candidate(
                  65537, "0309821d001f5177926fabb00f494b2803cf3cd07b2a849a0e18d60968580468"));
  result |= Check(std::holds_alternative<Rejection>(over_boundary.value),
                  "resolved allocation payload rejects 65537 UTF-8 bytes");
  const Command cancel{1, CommandType::kCancel, job, "operator@example"};
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(DecideCommand(absent, cancel).value),
                  "cancel accepted before worker");
  const RawCandidateEvent invalid{1, job, "job_created", "{\"unexpected\":true}"};
  const auto invalid_result = NormalizeCandidate(absent, invalid);
  result |= Check(
      std::holds_alternative<Rejection>(invalid_result.value) &&
          std::get<Rejection>(invalid_result.value).reason == RejectionReason::kInvalidEventPayload,
      "unknown payload is rejected before matrix selection");
  const RawCandidateEvent raw_late{
      1, job, "late_worker_event",
      "{\"original_event_type\":\"worker_completed\",\"worker_id\":\"123e4567-e89b-42d3-a456-"
      "426614174000\",\"event_sequence\":1}"};
  const auto raw_late_result = NormalizeCandidate(absent, raw_late);
  result |= Check(std::holds_alternative<Rejection>(raw_late_result.value) &&
                      std::get<Rejection>(raw_late_result.value).reason ==
                          RejectionReason::kInvalidEventPayload,
                  "reducer-owned late event is rejected at raw ingress");
  const auto expect_invalid_payload = [&](std::string_view event_type, std::string_view payload,
                                          const char* label) {
    const auto normalized = NormalizeCandidate(
        absent, RawCandidateEvent{1, job, std::string(event_type), std::string(payload)});
    return Check(
        std::holds_alternative<Rejection>(normalized.value) &&
            std::get<Rejection>(normalized.value).reason == RejectionReason::kInvalidEventPayload,
        label);
  };
  const auto max_timer_generation = NormalizeCandidate(
      absent,
      RawCandidateEvent{1, job, "timeout_expired",
                        R"({"phase":"preparation","timer_generation":18446744073709551615})"});
  result |= Check(std::holds_alternative<InternalEvent>(max_timer_generation.value),
                  "timeout payload accepts UINT64_MAX");
  result |= expect_invalid_payload(
      "timeout_expired", R"({"phase":"preparation","timer_generation":18446744073709551616})",
      "timeout payload rejects uint64 overflow");
  result |= expect_invalid_payload(
      "job_created", R"({"session_id":"01890f3e-7b00-7abc-8abc-0123456789ab"} // comment)",
      "strict JSON rejects line comments");
  result |= expect_invalid_payload(
      "job_created", R"({"session_id":"01890f3e-7b00-7abc-8abc-0123456789ab"} /* comment */)",
      "strict JSON rejects block comments");
  result |= expect_invalid_payload(
      "resources_committed",
      R"({"allocation_id":"allocation-1","allocation_digest":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a","resolved_allocation":{"schema_id":"allocation.v1","schema_version":1,"payload_utf8":"{\"x\":1 // comment\n}"}})",
      "strict JSON rejects comments in resolved allocation payload");
  result |= expect_invalid_payload(
      "resources_committed",
      R"({"allocation_id":"allocation-1","allocation_digest":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a","resolved_allocation":{"schema_id":"allocation.v1","schema_version":1,"payload_utf8":"{\"x\":1 /* comment */}"}})",
      "strict JSON rejects block comments in nested payload");
  result |= Check(
      std::holds_alternative<Rejection>(
          DecideEvent(absent, InternalEvent{1, job, static_cast<EventType>(255), EmptyPayload{}})
              .value),
      "unknown event enum rejects without undefined behavior");
  result |= Check(std::holds_alternative<Rejection>(
                      DecideEvent(absent, InternalEvent{1, job, EventType::kProcessExitConfirmed,
                                                        ProcessExitConfirmedPayload{
                                                            CompletionMode::kNone, StableId{"op"}}})
                          .value),
                  "completion none rejects where disallowed");
  const Snapshot forged_before = absent;
  const ApplyResult forged =
      Apply(forged_before, PreEnvelopeProposal{1, job, EventType::kWorkerRunning,
                                               WorkerRunningPayload{Uuid{std::string(k_worker)}}});
  result |= Check(
      forged.rejection.has_value() && forged.snapshot.state == forged_before.state &&
          forged.snapshot.resource_status == forged_before.resource_status &&
          forged.snapshot.pending_worker_event_ack == forged_before.pending_worker_event_ack &&
          forged.effects.empty(),
      "forged proposal cannot bypass decision");
  Snapshot finalizing = absent;
  finalizing.state = JobState::kFinalizing;
  finalizing.worker_launch_status = LaunchStatus::kObserved;
  finalizing.launch_operation_id = StableId{"launch-op-1"};
  finalizing.worker_id = Uuid{std::string(k_worker)};
  finalizing.process_presence = ProcessPresence::kPresent;
  finalizing.finalization_status = FinalizationStatus::kPending;
  const auto late_worker_decision =
      DecideEvent(finalizing, InternalEvent{1, job, EventType::kWorkerCompleted,
                                            WorkerEventPayload{Uuid{std::string(k_worker)}, 1}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(late_worker_decision.value) &&
                      std::get<PreEnvelopeProposal>(late_worker_decision.value).event_type ==
                          EventType::kLateWorkerEvent,
                  "finalizing snapshot normalizes a late Worker event");
  const auto normalized_forgery =
      Apply(finalizing, PreEnvelopeProposal{1, job, EventType::kWorkerCompleted,
                                            WorkerEventPayload{Uuid{std::string(k_worker)}, 1}});
  result |=
      Check(normalized_forgery.rejection.has_value() &&
                normalized_forgery.rejection->reason == RejectionReason::kInvariantViolation &&
                normalized_forgery.snapshot.state == JobState::kFinalizing &&
                !normalized_forgery.snapshot.pending_worker_event_ack &&
                normalized_forgery.effects.empty(),
            "Apply rejects a proposal that requires late-event normalization");
  const auto empty_finalizing_timeout =
      DecideEvent(finalizing, InternalEvent{1, job, EventType::kTimeoutExpired,
                                            TimeoutExpiredPayload{TimeoutPhase::kExecution, 1}});
  result |= Check(std::holds_alternative<Rejection>(empty_finalizing_timeout.value) &&
                      std::get<Rejection>(empty_finalizing_timeout.value).reason ==
                          RejectionReason::kTimeoutPhaseMismatch,
                  "finalizing execution timeout requires a reason or success candidate");
  Snapshot finalization_complete = finalizing;
  finalization_complete.finalization_status = FinalizationStatus::kCompleted;
  finalization_complete.session_retention_status = RetentionStatus::kRetained;
  const auto missing_terminal_reason = DecideEvent(
      finalization_complete, InternalEvent{1, job, EventType::kTerminalOutcomeCommitted,
                                           TerminalOutcomePayload{TerminalOutcome::kFailed}});
  result |= Check(std::holds_alternative<Rejection>(missing_terminal_reason.value) &&
                      std::get<Rejection>(missing_terminal_reason.value).reason ==
                          RejectionReason::kTerminalOutcomeMismatch,
                  "terminal failed outcome requires a matching latched reason");
  Snapshot success_finalizing = finalizing;
  success_finalizing.completion_candidate = true;
  const auto terminate_finalizing =
      DecideEvent(success_finalizing, InternalEvent{1, job, EventType::kTerminateAccepted,
                                                    PrincipalPayload{"administrator@example"}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(terminate_finalizing.value),
                  "administrator terminate is accepted for a live finalizing process");
  if (std::holds_alternative<PreEnvelopeProposal>(terminate_finalizing.value)) {
    const auto applied_terminate =
        Apply(success_finalizing, std::get<PreEnvelopeProposal>(terminate_finalizing.value));
    result |= Check(!applied_terminate.rejection &&
                        applied_terminate.snapshot.state == JobState::kFinalizing &&
                        applied_terminate.snapshot.completion_candidate &&
                        !applied_terminate.snapshot.latched_reason &&
                        applied_terminate.snapshot.completion_mode == CompletionMode::kForced,
                    "finalizing terminate preserves the success candidate and reason");
  }
  const auto missing_launch_exit = DecideEvent(
      absent, InternalEvent{1, job, EventType::kProcessExitConfirmed,
                            ProcessExitConfirmedPayload{CompletionMode::kProcessAlreadyExited,
                                                        StableId{"launch-op-1"}}});
  result |= Check(std::holds_alternative<Rejection>(missing_launch_exit.value) &&
                      std::get<Rejection>(missing_launch_exit.value).reason ==
                          RejectionReason::kInvariantViolation,
                  "preparing process exit without launch binding is an identity mismatch");

  Snapshot observed = absent;
  observed.worker_launch_status = LaunchStatus::kObserved;
  observed.launch_operation_id = StableId{"launch-op-1"};
  observed.worker_id = Uuid{std::string(k_worker)};
  observed.process_presence = ProcessPresence::kUnknown;
  const auto running_decision =
      DecideEvent(observed, InternalEvent{1, job, EventType::kWorkerRunning,
                                          WorkerRunningPayload{Uuid{std::string(k_worker)}}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(running_decision.value),
                  "observed Worker may enter running");
  Snapshot running = observed;
  if (std::holds_alternative<PreEnvelopeProposal>(running_decision.value)) {
    const auto applied_running =
        Apply(observed, std::get<PreEnvelopeProposal>(running_decision.value));
    result |=
        Check(!applied_running.rejection && applied_running.snapshot.state == JobState::kRunning &&
                  applied_running.snapshot.process_presence == ProcessPresence::kPresent,
              "worker_running establishes process presence");
    running = applied_running.snapshot;
  }
  Snapshot preparing_with_reason = absent;
  preparing_with_reason.latched_reason = TerminalOutcome::kCancelled;
  const auto preparation_timeout = DecideEvent(
      preparing_with_reason, InternalEvent{1, job, EventType::kTimeoutExpired,
                                           TimeoutExpiredPayload{TimeoutPhase::kPreparation, 2}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(preparation_timeout.value),
                  "preparing timeout remains matrix-valid with a prior reason fact");
  if (std::holds_alternative<PreEnvelopeProposal>(preparation_timeout.value)) {
    const auto applied_timeout =
        Apply(preparing_with_reason, std::get<PreEnvelopeProposal>(preparation_timeout.value));
    result |= Check(!applied_timeout.rejection &&
                        applied_timeout.snapshot.latched_reason == TerminalOutcome::kTimedOut,
                    "first-cause transition applies the contract value unconditionally");
  }
  Snapshot running_without_presence = running;
  running_without_presence.process_presence = ProcessPresence::kAbsent;
  running_without_presence.latched_reason = TerminalOutcome::kFailed;
  const auto running_cancel = DecideEvent(
      running_without_presence,
      InternalEvent{1, job, EventType::kCancelAccepted, PrincipalPayload{"operator@example"}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(running_cancel.value),
                  "running cancel is unconditional in the matrix");
  Snapshot stopping = running_without_presence;
  if (std::holds_alternative<PreEnvelopeProposal>(running_cancel.value)) {
    const auto applied_cancel =
        Apply(running_without_presence, std::get<PreEnvelopeProposal>(running_cancel.value));
    result |=
        Check(!applied_cancel.rejection && applied_cancel.snapshot.state == JobState::kStopping &&
                  applied_cancel.snapshot.latched_reason == TerminalOutcome::kCancelled &&
                  applied_cancel.snapshot.completion_mode == CompletionMode::kCooperative,
              "running cancel does not branch on process presence");
    stopping = applied_cancel.snapshot;
  }
  Snapshot running_with_reason = running;
  running_with_reason.latched_reason = TerminalOutcome::kCancelled;
  const auto completed_with_reason = DecideEvent(
      running_with_reason, InternalEvent{1, job, EventType::kWorkerCompleted,
                                         WorkerEventPayload{Uuid{std::string(k_worker)}, 7}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(completed_with_reason.value),
                  "matching running Worker completion is accepted");
  if (std::holds_alternative<PreEnvelopeProposal>(completed_with_reason.value)) {
    const auto applied_completion =
        Apply(running_with_reason, std::get<PreEnvelopeProposal>(completed_with_reason.value));
    result |=
        Check(!applied_completion.rejection && applied_completion.snapshot.completion_candidate &&
                  applied_completion.snapshot.latched_reason == TerminalOutcome::kCancelled,
              "running Worker completion sets the candidate without replacing the reason");
  }
  stopping.completion_candidate = true;
  const auto stopping_failed =
      DecideEvent(stopping, InternalEvent{1, job, EventType::kWorkerFailed,
                                          WorkerEventPayload{Uuid{std::string(k_worker)}, 8}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(stopping_failed.value),
                  "matching stopping Worker failure is accepted");
  if (std::holds_alternative<PreEnvelopeProposal>(stopping_failed.value)) {
    const auto applied_failure =
        Apply(stopping, std::get<PreEnvelopeProposal>(stopping_failed.value));
    result |= Check(!applied_failure.rejection && applied_failure.snapshot.completion_candidate &&
                        applied_failure.snapshot.latched_reason == TerminalOutcome::kCancelled,
                    "stopping Worker failure preserves reason and candidate facts");
  }
  Snapshot reasoned_finalizing = finalizing;
  reasoned_finalizing.latched_reason = TerminalOutcome::kCancelled;
  reasoned_finalizing.completion_candidate = true;
  const auto finalization_failure = DecideEvent(
      reasoned_finalizing, InternalEvent{1, job, EventType::kFinalizationFailed, EmptyPayload{}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(finalization_failure.value),
                  "finalization failure preserves an existing reason");
  if (std::holds_alternative<PreEnvelopeProposal>(finalization_failure.value)) {
    const auto applied_finalization_failure =
        Apply(reasoned_finalizing, std::get<PreEnvelopeProposal>(finalization_failure.value));
    result |= Check(
        !applied_finalization_failure.rejection &&
            applied_finalization_failure.snapshot.completion_candidate &&
            applied_finalization_failure.snapshot.latched_reason == TerminalOutcome::kCancelled,
        "finalization failure preserves candidate and reason in preserve_reason case");
  }
  Snapshot duplicate_preparing_exit = observed;
  duplicate_preparing_exit.process_presence = ProcessPresence::kAbsent;
  duplicate_preparing_exit.process_exit_confirmed = true;
  duplicate_preparing_exit.completion_mode = CompletionMode::kForced;
  const auto repeated_preparing_exit = DecideEvent(
      duplicate_preparing_exit,
      InternalEvent{
          1, job, EventType::kProcessExitConfirmed,
          ProcessExitConfirmedPayload{CompletionMode::kCooperative, StableId{"launch-op-1"}}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(repeated_preparing_exit.value),
                  "preparing process exit predicate depends only on launch identity");
  if (std::holds_alternative<PreEnvelopeProposal>(repeated_preparing_exit.value)) {
    const auto applied_exit = Apply(duplicate_preparing_exit,
                                    std::get<PreEnvelopeProposal>(repeated_preparing_exit.value));
    result |=
        Check(!applied_exit.rejection &&
                  applied_exit.snapshot.completion_mode == CompletionMode::kProcessAlreadyExited &&
                  applied_exit.snapshot.latched_reason == TerminalOutcome::kFailed &&
                  applied_exit.effects.size() == 1 &&
                  applied_exit.effects.front().id == EffectId::kDisarmPreparationTimeout,
              "preparing process exit reapplies unconditional updates and effects");
  }
  Snapshot duplicate_stopping_exit = stopping;
  duplicate_stopping_exit.process_exit_confirmed = true;
  duplicate_stopping_exit.process_presence = ProcessPresence::kAbsent;
  duplicate_stopping_exit.completion_mode = CompletionMode::kCooperative;
  const auto repeated_stopping_exit = DecideEvent(
      duplicate_stopping_exit,
      InternalEvent{1, job, EventType::kProcessExitConfirmed,
                    ProcessExitConfirmedPayload{CompletionMode::kForced, StableId{"launch-op-1"}}});
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(repeated_stopping_exit.value),
                  "stopping process exit predicate depends only on launch identity");
  if (std::holds_alternative<PreEnvelopeProposal>(repeated_stopping_exit.value)) {
    const auto applied_exit =
        Apply(duplicate_stopping_exit, std::get<PreEnvelopeProposal>(repeated_stopping_exit.value));
    result |=
        Check(!applied_exit.rejection &&
                  applied_exit.snapshot.completion_mode == CompletionMode::kCooperative &&
                  applied_exit.effects.size() == 2 &&
                  applied_exit.effects[0].id == EffectId::kDisarmCooperativeStopTimeout &&
                  applied_exit.effects[1].id == EffectId::kDisarmProcessExitConfirmationTimeout,
              "stopping process exit preserves mode and reapplies ordered disarm effects");
  }

  TimerState timer;
  timer.job_id = job;
  timer.preparation_armed = true;
  timer.preparation_generation = 9;
  const auto timer_result = IngestTimer(timer, job, TimeoutPhase::kPreparation, 9);
  result |= Check(timer_result.kind == TimerIngressKind::kEmitCandidateEvent &&
                      timer_result.candidate.has_value(),
                  "current timer emits one candidate");
  result |= Check(IngestTimer(timer, job, TimeoutPhase::kPreparation, 8).kind ==
                      TimerIngressKind::kDiscardWithoutCandidate,
                  "stale timer is discarded");
  timer.preparation_generation = std::numeric_limits<std::uint64_t>::max();
  result |=
      Check(IngestTimer(
                timer, TimerIngressInput{TimerArmRequest{job, TimeoutPhase::kPreparation,
                                                         std::numeric_limits<std::uint64_t>::max()},
                                         std::nullopt})
                    .kind == TimerIngressKind::kFailClosed,
            "new arm generation exhaustion fails closed");
  const auto exhausted_state_with_nonmax_request = IngestTimer(
      timer, TimerIngressInput{TimerArmRequest{job, TimeoutPhase::kPreparation, 1}, std::nullopt});
  result |= Check(
      exhausted_state_with_nonmax_request.kind == TimerIngressKind::kFailClosed &&
          !exhausted_state_with_nonmax_request.candidate.has_value() &&
          exhausted_state_with_nonmax_request.effects.size() == 1 &&
          exhausted_state_with_nonmax_request.effects.front().id == EffectId::kSetReadinessFalse,
      "active generation exhaustion cannot be bypassed by request generation");
  const auto invalid_arm = IngestTimer(
      timer, TimerIngressInput{TimerArmRequest{job, TimeoutPhase::kInvalid, 1}, std::nullopt});
  result |= Check(invalid_arm.kind == TimerIngressKind::kFailClosed &&
                      !invalid_arm.candidate.has_value() && invalid_arm.effects.size() == 1 &&
                      invalid_arm.effects.front().id == EffectId::kSetReadinessFalse,
                  "invalid arm phase fails closed with only readiness false");
  const auto zero_generation_arm =
      IngestTimer(timer, TimerIngressInput{TimerArmRequest{job, TimeoutPhase::kPreparation, 0},
                                           TimerNotification{job, TimeoutPhase::kPreparation, 9}});
  result |= Check(zero_generation_arm.kind == TimerIngressKind::kFailClosed &&
                      !zero_generation_arm.candidate.has_value() &&
                      zero_generation_arm.effects.size() == 1 &&
                      zero_generation_arm.effects.front().id == EffectId::kSetReadinessFalse,
                  "zero-generation arm fails closed before a valid notification");
  const Uuid other_job{"01890f3e-7b00-7abc-8abc-0123456789ac"};
  const auto mismatched_timer_job =
      IngestTimer(timer, other_job, TimeoutPhase::kPreparation, timer.preparation_generation);
  result |= Check(mismatched_timer_job.kind == TimerIngressKind::kFailClosed &&
                      !mismatched_timer_job.candidate.has_value() &&
                      mismatched_timer_job.effects.size() == 1 &&
                      mismatched_timer_job.effects.front().id == EffectId::kSetReadinessFalse,
                  "timer notification for another Job fails closed");
  const auto mismatched_arm_job = IngestTimer(
      timer,
      TimerIngressInput{TimerArmRequest{other_job, TimeoutPhase::kPreparation, 1}, std::nullopt});
  result |= Check(mismatched_arm_job.kind == TimerIngressKind::kFailClosed &&
                      !mismatched_arm_job.candidate.has_value() &&
                      mismatched_arm_job.effects.size() == 1 &&
                      mismatched_arm_job.effects.front().id == EffectId::kSetReadinessFalse,
                  "timer arm for another Job fails closed");
  const Uuid invalid_job{"not-a-job-id"};
  const auto invalid_timer_job =
      IngestTimer(timer, invalid_job, TimeoutPhase::kPreparation, timer.preparation_generation);
  result |=
      Check(invalid_timer_job.kind == TimerIngressKind::kFailClosed &&
                !invalid_timer_job.candidate.has_value() && invalid_timer_job.effects.size() == 1 &&
                invalid_timer_job.effects.front().id == EffectId::kSetReadinessFalse,
            "invalid timer notification Job ID fails closed");
  const auto invalid_arm_job = IngestTimer(
      timer,
      TimerIngressInput{TimerArmRequest{invalid_job, TimeoutPhase::kPreparation, 1}, std::nullopt});
  result |=
      Check(invalid_arm_job.kind == TimerIngressKind::kFailClosed &&
                !invalid_arm_job.candidate.has_value() && invalid_arm_job.effects.size() == 1 &&
                invalid_arm_job.effects.front().id == EffectId::kSetReadinessFalse,
            "invalid timer arm Job ID fails closed");
  result |= Check(IngestTimer(timer, TimerIngressInput{std::nullopt, std::nullopt}).kind ==
                      TimerIngressKind::kDiscardWithoutCandidate,
                  "null notification does not fail closed");
  (void)k_worker;
  return result;
}
}  // namespace

int main(int argc, char** argv) try {
  int result = Run();
  if (argc == 1) result |= RunMalformedArtifactChecks();
  if (argc > 1) {
    result |= sitometron::test::RunJobReducerVectorChecks(argv[1], argc > 2 ? argv[2] : "all");
    if (argc <= 2 || (argc > 2 && std::string_view(argv[2]) == "job_closed_state_set")) {
      result |= sitometron::test::RunJobReducerParityMutationChecks(argv[1]);
      result |= sitometron::test::RunJobReducerTrailingArtifactChecks(argv[1]);
    }
  }
  return result;
} catch (...) {
  return 1;
}
