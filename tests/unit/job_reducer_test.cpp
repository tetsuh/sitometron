#include "sitometron/core/job_reducer.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace sitometron::test {
int RunJobReducerVectorChecks(const char* path, const char* selector);
int RunJobReducerVectorTextChecks(std::string_view text, const char* selector);
int RunJobReducerParityMutationChecks(const char* path);
int RunJobReducerParityMutationTextChecks(std::string_view text);
int RunJobReducerTrailingArtifactChecks(const char* path);
int RunJobReducerClosedEnumArtifactChecks();
}  // namespace sitometron::test

namespace {
using namespace sitometron::core;
constexpr std::string_view kJob = "01890f3e-7b00-7abc-8abc-0123456789ab";
constexpr std::string_view kWorker = "123e4567-e89b-42d3-a456-426614174000";

int Check(bool condition, std::string_view message) {
  if (condition) return 0;
  std::cerr << "job_reducer: " << message << '\n';
  return 1;
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
  const Uuid job{std::string(kJob)};
  Snapshot absent = InitialSnapshot(job, job);
  int result = 0;
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
  admitted_launch.worker_id = Uuid{std::string(kWorker)};
  result |= admitted_snapshot_rejected(admitted_launch, "admitted Worker launch fails closed");
  Snapshot admitted_process = present;
  admitted_process.process_presence = ProcessPresence::kPresent;
  result |= admitted_snapshot_rejected(admitted_process, "admitted process presence fails closed");
  Snapshot admitted_exit = present;
  admitted_exit.process_exit_confirmed = true;
  result |= admitted_snapshot_rejected(admitted_exit, "admitted confirmed exit fails closed");
  const RawCandidateEvent created{1, job, "job_created",
                                  "{\"session_id\":\"" + std::string(kJob) + "\"}"};
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
                        Uuid{std::string(kWorker)}}});
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
        {"worker_id", std::string(kWorker)}};
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
                                          std::string_view label) {
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
                                               WorkerRunningPayload{Uuid{std::string(kWorker)}}});
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
  finalizing.worker_id = Uuid{std::string(kWorker)};
  finalizing.process_presence = ProcessPresence::kPresent;
  finalizing.finalization_status = FinalizationStatus::kPending;
  const auto normalized_forgery =
      Apply(finalizing, PreEnvelopeProposal{1, job, EventType::kWorkerCompleted,
                                            WorkerEventPayload{Uuid{std::string(kWorker)}, 1}});
  result |=
      Check(normalized_forgery.rejection.has_value() &&
                normalized_forgery.rejection->reason == RejectionReason::kInvariantViolation &&
                normalized_forgery.snapshot.state == JobState::kFinalizing &&
                !normalized_forgery.snapshot.pending_worker_event_ack &&
                normalized_forgery.effects.empty(),
            "Apply rejects a proposal that requires late-event normalization");

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
  (void)kWorker;
  return result;
}
}  // namespace

int main(int argc, char** argv) {
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
}
