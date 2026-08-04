#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core_fakes.hpp"
#include "sitometron/core/job_ports.hpp"

namespace {

using sitometron::core::ApplicationLaunchRequest;
using sitometron::core::ApplicationStopRequest;
using sitometron::core::ClockReading;
using sitometron::core::DiagnosticTimestamp;
using sitometron::core::EffectId;
using sitometron::core::EventType;
using sitometron::core::GeneratedJobSessionIdentity;
using sitometron::core::GeneratedLaunchOperationIdentity;
using sitometron::core::GeneratedWorkerIdentity;
using sitometron::core::IdentitySourceExhausted;
using sitometron::core::InternalEvent;
using sitometron::core::LogicalCommitResult;
using sitometron::core::LogicalJobEvent;
using sitometron::core::MonotonicInstant;
using sitometron::core::RawCandidateEvent;
using sitometron::core::SessionRetainRequest;
using sitometron::core::StableId;
using sitometron::core::TerminalOutcome;
using sitometron::core::TerminalOutcomePayload;
using sitometron::core::Uuid;
using sitometron::core::WorkerLaunchIntentPayload;
using sitometron::core::WorkerLaunchObservedPayload;
using sitometron::test::ExpectCooperativeStop;
using sitometron::test::ExpectForcedStop;
using sitometron::test::ExpectLaunch;
using sitometron::test::ExpectLaunchProcessAlreadyExited;
using sitometron::test::ExpectSessionRetain;
using sitometron::test::FakeApplicationRunner;
using sitometron::test::FakeClock;
using sitometron::test::FakeIdentitySource;
using sitometron::test::FakeJobJournal;
using sitometron::test::FakeSessionRetainer;
using sitometron::test::JournalExpectation;
using sitometron::test::PassiveEffectObserver;
using sitometron::test::RunnerCallKind;
using sitometron::test::RunnerObservation;
using sitometron::test::RunnerRequest;

constexpr std::string_view kJobId = "01890f30-7b54-7cc3-98c4-dc0c0c07398f";
constexpr std::string_view kOtherV7Id = "01890f30-7b54-7cc3-98c4-dc0c0c073990";
constexpr std::string_view kWorkerId = "550e8400-e29b-41d4-a716-446655440000";
constexpr std::string_view kLaunchOperationId = "launch-op-1";
constexpr std::string_view kEmptyObjectDigest =
    "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
constexpr std::string_view kMaxResourceDigest =
    "e1f2b8fa362bf7b7574e009b741da16f4ac6961bb1e2a594dbf19c5a7f5a2ad0";
constexpr std::string_view kOversizeResourceDigest =
    "4677f55af78bbd6788ee548feb1b932cc7a41f890dc9bd121f2f339c588a8f5d";
constexpr std::string_view kInvalidUtf8Digest =
    "4df51a863599ce9b7e9cac29d2144a68d0509897aa26626ded17edb0c761bd52";
constexpr std::string_view kInvalidJsonDigest =
    "7ccfa1fbf3940e6f0c0375d87c0f9235a50514e14cb427bdfaf5077987b26ccf";

std::string RepeatUtf8(std::string_view scalar, std::size_t count) {
  std::string result;
  result.reserve(scalar.size() * count);
  for (std::size_t index = 0; index < count; ++index) {
    result += scalar;
  }
  return result;
}

std::string MakeMaxResourceJson() {
  std::string result = "[";
  result.reserve(65536);
  for (int index = 0; index < 13105; ++index) {
    result += "null,";
  }
  result += "null";
  result.append(5, ' ');
  result += "]";
  return result;
}

int Check(bool condition, std::string_view description) {
  if (condition) {
    return 0;
  }
  std::cerr << "core_ports_fakes_test: " << description << '\n';
  return 1;
}

LogicalJobEvent MakeEvent(std::uint64_t sequence, EventType event_type) {
  LogicalJobEvent event;
  event.sequence = sequence;
  event.event_type = event_type;
  event.recorded_at = DiagnosticTimestamp{"2026-08-04T00:00:00Z"};
  event.job_id = Uuid{std::string(kJobId)};
  if (event_type == EventType::kWorkerLaunchObserved) {
    event.payload = WorkerLaunchObservedPayload{StableId{std::string(kLaunchOperationId)}, true};
  } else {
    event.payload = TerminalOutcomePayload{TerminalOutcome::kSucceeded};
  }
  return event;
}

LogicalJobEvent MakeValidEvent(EventType event_type) {
  auto event = MakeEvent(1, event_type);
  using namespace sitometron::core;
  switch (event_type) {
    case EventType::kJobCreated:
      event.payload = JobCreatedPayload{Uuid{std::string(kJobId)}};
      break;
    case EventType::kResourcesCommitted:
      event.payload = ResourcesCommittedPayload{StableId{"allocation-1"},
                                                Digest{std::string(kEmptyObjectDigest)},
                                                StableId{"allocation-schema"}, 1, "{}"};
      break;
    case EventType::kWorkerLaunchIntent:
      event.payload = WorkerLaunchIntentPayload{StableId{std::string(kLaunchOperationId)},
                                                StableId{"application-1"},
                                                RepeatUtf8("\xC3\xA9", 128),
                                                Digest{std::string(64, 'a')},
                                                StableId{"allocation-1"},
                                                Digest{std::string(64, 'b')},
                                                Uuid{std::string(kWorkerId)}};
      break;
    case EventType::kWorkerLaunchObserved:
      event.payload = WorkerLaunchObservedPayload{StableId{std::string(kLaunchOperationId)}, true};
      break;
    case EventType::kWorkerRunning:
      event.payload = WorkerRunningPayload{Uuid{std::string(kWorkerId)}};
      break;
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      event.payload = PrincipalPayload{RepeatUtf8("\xC3\xA9", 256)};
      break;
    case EventType::kTimeoutExpired:
      event.payload = TimeoutExpiredPayload{TimeoutPhase::kExecution, 1};
      break;
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed:
      event.payload = WorkerEventPayload{Uuid{std::string(kWorkerId)}, 1};
      break;
    case EventType::kProcessExitConfirmed:
      event.payload = ProcessExitConfirmedPayload{CompletionMode::kCooperative,
                                                  StableId{std::string(kLaunchOperationId)}};
      break;
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      event.payload = SessionPayload{Uuid{std::string(kJobId)}};
      break;
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      event.payload = EmptyPayload{};
      break;
    case EventType::kTerminalOutcomeCommitted:
      event.payload = TerminalOutcomePayload{TerminalOutcome::kSucceeded};
      break;
    case EventType::kResourcesReleased:
      event.payload =
          ResourcesReleasedPayload{StableId{"allocation-1"}, Digest{std::string(64, 'a')}};
      break;
    case EventType::kCleanupStatusRecorded:
      event.payload = CleanupStatusPayload{CleanupStatus::kCompleted};
      break;
    case EventType::kLateWorkerEvent:
      event.payload =
          LateWorkerEventPayload{EventType::kWorkerCompleted, Uuid{std::string(kWorkerId)}, 1};
      break;
    case EventType::kInvalid:
      break;
  }
  return event;
}

ApplicationLaunchRequest MakeLaunchRequest() {
  WorkerLaunchIntentPayload intent;
  intent.operation_id = StableId{std::string(kLaunchOperationId)};
  intent.application_id = StableId{"application-1"};
  intent.application_version = "1.0.0";
  intent.bundle_sha256.value = std::string(64, 'a');
  intent.allocation_id = StableId{"allocation-1"};
  intent.allocation_digest.value = std::string(64, 'b');
  intent.worker_id = Uuid{std::string(kWorkerId)};
  return ApplicationLaunchRequest{Uuid{std::string(kJobId)}, std::move(intent)};
}

ApplicationStopRequest MakeStopRequest() {
  return ApplicationStopRequest{Uuid{std::string(kJobId)},
                                StableId{std::string(kLaunchOperationId)},
                                Uuid{std::string(kWorkerId)}};
}

int CheckPortAndFakeContracts() {
  static_assert(std::is_abstract_v<sitometron::core::ClockPort>);
  static_assert(std::is_abstract_v<sitometron::core::JobJournalPort>);
  static_assert(std::is_abstract_v<sitometron::core::ApplicationRunnerPort>);
  static_assert(std::is_abstract_v<sitometron::core::SessionRetainerPort>);
  static_assert(std::is_abstract_v<sitometron::core::IdentitySourcePort>);
  static_assert(std::is_nothrow_move_constructible_v<ApplicationLaunchRequest>);
  static_assert(std::is_nothrow_destructible_v<ApplicationLaunchRequest>);
  static_assert(std::is_nothrow_move_constructible_v<ApplicationStopRequest>);
  static_assert(std::is_nothrow_destructible_v<ApplicationStopRequest>);
  static_assert(std::is_nothrow_move_constructible_v<SessionRetainRequest>);
  static_assert(std::is_nothrow_destructible_v<SessionRetainRequest>);
  static_assert(std::is_nothrow_move_constructible_v<RunnerRequest>);
  static_assert(std::is_nothrow_destructible_v<RunnerRequest>);
  static_assert(std::is_nothrow_move_constructible_v<RunnerObservation>);
  static_assert(std::is_nothrow_destructible_v<RunnerObservation>);
  static_assert(std::is_nothrow_move_constructible_v<RawCandidateEvent>);
  static_assert(std::is_nothrow_destructible_v<RawCandidateEvent>);
  static_assert(noexcept(std::declval<std::optional<RunnerObservation>&>() =
                             std::declval<RunnerObservation&&>()));
  static_assert(noexcept(std::declval<std::optional<SessionRetainRequest>&>() =
                             std::declval<SessionRetainRequest&&>()));
  static_assert(noexcept(std::declval<std::optional<RawCandidateEvent>&>() =
                             std::declval<RawCandidateEvent&&>()));

  int result = 0;

  FakeClock clock(ClockReading{DiagnosticTimestamp{"2026-08-04T00:00:00Z"}, MonotonicInstant{10}});
  const auto first_read = clock.Read();
  const auto second_read = clock.Read();
  result |= Check(first_read.monotonic_time.nanoseconds_since_origin == 10,
                  "clock returns configured monotonic value");
  result |= Check(second_read.monotonic_time.nanoseconds_since_origin == 10,
                  "clock read does not advance time");
  result |= Check(clock.AdvanceMonotonic(5), "clock accepts checked monotonic advance");
  clock.SetDiagnostic(DiagnosticTimestamp{"2026-08-04T00:00:01Z"});
  const auto advanced_read = clock.Read();
  result |= Check(advanced_read.monotonic_time.nanoseconds_since_origin == 15,
                  "clock advances monotonic time explicitly");
  result |= Check(advanced_read.recorded_at.rfc3339 == "2026-08-04T00:00:01Z",
                  "clock changes diagnostic time explicitly");

  FakeClock overflow_clock(
      ClockReading{DiagnosticTimestamp{"2026-08-04T00:00:00Z"},
                   MonotonicInstant{std::numeric_limits<std::uint64_t>::max()}});
  result |= Check(!overflow_clock.AdvanceMonotonic(1), "clock rejects monotonic wraparound");
  result |= Check(overflow_clock.verification_failed(), "clock overflow is sticky");
  FakeClock invalid_clock(
      ClockReading{DiagnosticTimestamp{"2026-02-30T00:00:00Z"}, MonotonicInstant{0}});
  result |= Check(invalid_clock.verification_failed(),
                  "clock rejects an invalid diagnostic timestamp during setup");

  FakeIdentitySource identities(
      {GeneratedJobSessionIdentity{Uuid{std::string(kJobId)}}, IdentitySourceExhausted{}},
      {GeneratedWorkerIdentity{Uuid{std::string(kWorkerId)}}, IdentitySourceExhausted{}},
      {GeneratedLaunchOperationIdentity{StableId{std::string(kLaunchOperationId)}},
       IdentitySourceExhausted{}});
  result |= Check(
      std::holds_alternative<GeneratedJobSessionIdentity>(identities.GenerateJobSessionIdentity()),
      "identity fake returns scripted Job/Session identity");
  result |= Check(
      std::holds_alternative<IdentitySourceExhausted>(identities.GenerateJobSessionIdentity()),
      "identity fake returns scripted Job/Session exhaustion");
  result |=
      Check(std::holds_alternative<GeneratedWorkerIdentity>(identities.GenerateWorkerIdentity()),
            "identity fake returns scripted Worker identity");
  result |=
      Check(std::holds_alternative<IdentitySourceExhausted>(identities.GenerateWorkerIdentity()),
            "identity fake returns scripted Worker exhaustion");
  result |= Check(std::holds_alternative<GeneratedLaunchOperationIdentity>(
                      identities.GenerateLaunchOperationIdentity()),
                  "identity fake returns scripted launch identity");
  result |= Check(
      std::holds_alternative<IdentitySourceExhausted>(identities.GenerateLaunchOperationIdentity()),
      "identity fake returns scripted launch exhaustion");
  result |= Check(identities.Verify(), "identity fake consumes every mandatory script entry");

  FakeIdentitySource invalid_identities({GeneratedJobSessionIdentity{Uuid{std::string(kWorkerId)}}},
                                        {}, {});
  result |= Check(invalid_identities.verification_failed(),
                  "identity fake rejects a UUIDv4 in the Job/Session script");
  FakeIdentitySource duplicate_identities({GeneratedJobSessionIdentity{Uuid{std::string(kJobId)}},
                                           GeneratedJobSessionIdentity{Uuid{std::string(kJobId)}}},
                                          {}, {});
  result |= Check(duplicate_identities.verification_failed(),
                  "identity fake rejects a duplicate generated identity");

  std::vector<sitometron::test::RunnerExpectation> runner_script;
  runner_script.push_back(ExpectLaunch(MakeLaunchRequest(), true));
  runner_script.push_back(ExpectLaunchProcessAlreadyExited(MakeLaunchRequest()));
  runner_script.push_back(ExpectCooperativeStop(MakeStopRequest()));
  runner_script.push_back(ExpectForcedStop(MakeStopRequest()));
  FakeApplicationRunner runner(std::move(runner_script), 4);
  runner.HandoffLaunch(MakeLaunchRequest());
  runner.HandoffLaunch(MakeLaunchRequest());
  runner.HandoffCooperativeStop(MakeStopRequest());
  runner.HandoffForcedStop(MakeStopRequest());

  const auto runner_observations = runner.CopyObservations();
  result |=
      Check(runner_observations.size() == 4, "runner records every matching handoff exactly once");
  result |= Check(runner_observations.at(0).kind == RunnerCallKind::kLaunch &&
                      runner_observations.at(1).kind == RunnerCallKind::kLaunch &&
                      runner_observations.at(2).kind == RunnerCallKind::kCooperativeStop &&
                      runner_observations.at(3).kind == RunnerCallKind::kForcedStop,
                  "runner preserves total call order");
  const auto launch_candidate = runner.TakeNextCandidate();
  result |= Check(
      launch_candidate.has_value() && launch_candidate->event_type == "worker_launch_observed" &&
          launch_candidate->job_id.value == kJobId &&
          launch_candidate->payload_json.find("\"outcome\":\"started\"") != std::string::npos &&
          launch_candidate->payload_json.find("\"started\":") == std::string::npos,
      "runner stages the schema-valid matching launch observation candidate");
  if (launch_candidate.has_value()) {
    const auto normalized = sitometron::core::NormalizeCandidate(
        sitometron::core::InitialSnapshot(Uuid{std::string(kJobId)}, Uuid{std::string(kJobId)}),
        *launch_candidate);
    result |= Check(std::holds_alternative<InternalEvent>(normalized.value),
                    "staged launch observation normalizes as a raw candidate");
  }
  const auto exited_candidate = runner.TakeNextCandidate();
  result |= Check(
      exited_candidate.has_value() && exited_candidate->event_type == "process_exit_confirmed" &&
          exited_candidate->payload_json.find("process_already_exited") != std::string::npos,
      "runner stages the matching launch process-exit candidate");
  result |= Check(runner.CancelNextCandidate(), "runner cancels the oldest candidate once");
  const auto forced_candidate = runner.TakeNextCandidate();
  result |= Check(forced_candidate.has_value() &&
                      forced_candidate->event_type == "process_exit_confirmed" &&
                      forced_candidate->payload_json.find("forced") != std::string::npos,
                  "runner keeps candidate FIFO order after cancellation");
  result |=
      Check(!runner.TakeNextCandidate().has_value(), "runner empty take returns no candidate");
  result |= Check(runner.Verify(), "runner verifies consumed expectations and candidates");

  FakeApplicationRunner mismatched_runner({ExpectLaunch(MakeLaunchRequest(), true)}, 1);
  auto wrong_launch = MakeLaunchRequest();
  wrong_launch.job_id.value = std::string(kOtherV7Id);
  mismatched_runner.HandoffLaunch(std::move(wrong_launch));
  result |= Check(mismatched_runner.verification_failed(), "runner mismatch is sticky");
  result |= Check(mismatched_runner.discarded_request_count() == 1,
                  "runner consumes a mismatched transferred request exactly once");
  mismatched_runner.HandoffLaunch(MakeLaunchRequest());
  result |= Check(mismatched_runner.discarded_request_count() == 2,
                  "runner consumes later transferred requests while sticky");
  result |= Check(mismatched_runner.CopyObservations().empty(),
                  "runner mismatch records no successful observation");
  result |= Check(!mismatched_runner.TakeNextCandidate().has_value(),
                  "runner mismatch stages no candidate");

  FakeApplicationRunner pending_runner({ExpectLaunch(MakeLaunchRequest(), true)}, 1);
  pending_runner.HandoffLaunch(MakeLaunchRequest());
  result |= Check(!pending_runner.Verify() && pending_runner.verification_failed(),
                  "unused mandatory candidate latches verification failure");
  result |= Check(pending_runner.CancelNextCandidate() && !pending_runner.Verify(),
                  "candidate cancellation cannot clear sticky verification failure");
  FakeApplicationRunner undersized_runner({ExpectLaunch(MakeLaunchRequest(), true)}, 0);
  result |= Check(undersized_runner.verification_failed(),
                  "runner rejects insufficient candidate capacity during setup");
  auto invalid_launch = MakeLaunchRequest();
  invalid_launch.intent.application_version.clear();
  FakeApplicationRunner invalid_launch_runner({ExpectLaunch(std::move(invalid_launch), true)}, 1);
  result |= Check(invalid_launch_runner.verification_failed(),
                  "runner rejects an invalid application version during setup");
  auto invalid_digest_launch = MakeLaunchRequest();
  invalid_digest_launch.intent.bundle_sha256.value = "not-a-digest";
  FakeApplicationRunner invalid_digest_runner(
      {ExpectLaunch(std::move(invalid_digest_launch), true)}, 1);
  result |= Check(invalid_digest_runner.verification_failed(),
                  "runner rejects an invalid digest during setup");

  const SessionRetainRequest retain_request{Uuid{std::string(kJobId)}, Uuid{std::string(kJobId)}};
  FakeSessionRetainer retainer({ExpectSessionRetain(retain_request)}, 1);
  retainer.HandoffRetainSameIdentity(SessionRetainRequest{retain_request});
  const auto retained_candidate = retainer.TakeNextCandidate();
  result |= Check(retained_candidate.has_value() &&
                      retained_candidate->event_type == "session_retained" &&
                      retained_candidate->job_id.value == kJobId,
                  "Session fake stages only the matching retained candidate");
  result |= Check(retainer.Verify(), "Session fake verifies consumed expectations");

  const SessionRetainRequest unequal_retain{Uuid{std::string(kJobId)},
                                            Uuid{std::string(kOtherV7Id)}};
  FakeSessionRetainer unequal_retainer({ExpectSessionRetain(unequal_retain)}, 1);
  result |= Check(unequal_retainer.verification_failed(),
                  "Session fake rejects unequal Job and Session identity during setup");
  FakeSessionRetainer mismatched_retainer({ExpectSessionRetain(retain_request)}, 1);
  mismatched_retainer.HandoffRetainSameIdentity(SessionRetainRequest{unequal_retain});
  result |= Check(mismatched_retainer.discarded_request_count() == 1 &&
                      mismatched_retainer.verification_failed() &&
                      mismatched_retainer.CopyObservations().empty(),
                  "Session mismatch consumes once without a successful observation");

  FakeSessionRetainer empty_retainer({}, 0);
  result |=
      Check(!empty_retainer.CancelNextCandidate(), "empty candidate cancellation reports failure");
  result |= Check(empty_retainer.verification_failed(), "empty candidate cancellation is sticky");

  return result;
}

int CheckLogicalCommitResults() {
  const auto committed_event = MakeEvent(1, EventType::kWorkerLaunchObserved);
  const auto failed_event = MakeEvent(2, EventType::kTerminalOutcomeCommitted);
  const auto unknown_event = MakeEvent(3, EventType::kTerminalOutcomeCommitted);

  FakeJobJournal journal({JournalExpectation{committed_event, LogicalCommitResult::kCommitted},
                          JournalExpectation{failed_event, LogicalCommitResult::kDefiniteFailure},
                          JournalExpectation{unknown_event, LogicalCommitResult::kOutcomeUnknown}});

  int result = 0;
  result |= Check(journal.Commit(committed_event) == LogicalCommitResult::kCommitted,
                  "Journal fake returns committed");
  result |= Check(journal.Commit(failed_event) == LogicalCommitResult::kDefiniteFailure,
                  "Journal fake returns definite failure");
  result |= Check(journal.Commit(unknown_event) == LogicalCommitResult::kOutcomeUnknown,
                  "Journal fake returns outcome unknown");
  result |= Check(journal.Verify(), "Journal fake consumes every mandatory expectation");

  const auto observations = journal.CopyObservations();
  result |= Check(observations.size() == 3, "Journal fake records ordered attempts");
  result |= Check(observations.at(0).sequence == 1 && observations.at(1).sequence == 2 &&
                      observations.at(2).sequence == 3,
                  "Journal fake preserves writer-assigned sequence values");
  result |= Check(observations.at(0).schema_version == 1 &&
                      observations.at(0).event_type == EventType::kWorkerLaunchObserved &&
                      observations.at(0).job_id.value == kJobId,
                  "Journal fake preserves schema, event type, and Job identity");
  result |= Check(observations.at(0).recorded_at.rfc3339 == "2026-08-04T00:00:00Z",
                  "Journal fake preserves writer-assigned recorded_at");
  const auto& launch_payload = std::get<WorkerLaunchObservedPayload>(observations.at(0).payload);
  result |= Check(launch_payload.operation_id.value == kLaunchOperationId && launch_payload.started,
                  "Journal fake preserves the complete payload");

  FakeJobJournal recording_failure(
      {JournalExpectation{committed_event, LogicalCommitResult::kCommitted, true}});
  result |= Check(recording_failure.Commit(committed_event) == LogicalCommitResult::kOutcomeUnknown,
                  "Journal observation failure maps to outcome unknown");
  result |= Check(recording_failure.verification_failed(), "Journal observation failure is sticky");
  result |= Check(recording_failure.CopyObservations().empty(),
                  "Journal observation failure records no false observation");
  result |= Check(recording_failure.Commit(committed_event) == LogicalCommitResult::kOutcomeUnknown,
                  "sticky Journal returns outcome unknown without retry");
  result |= Check(recording_failure.remaining_expectations() == 1,
                  "sticky Journal does not consume the failed expectation");

  FakeJobJournal mismatch({JournalExpectation{committed_event, LogicalCommitResult::kCommitted}});
  result |= Check(mismatch.Commit(failed_event) == LogicalCommitResult::kOutcomeUnknown,
                  "Journal mismatch fails closed as outcome unknown");
  result |= Check(mismatch.verification_failed(), "Journal mismatch is sticky");
  result |= Check(mismatch.CopyObservations().empty(),
                  "Journal mismatch records no successful observation");

  FakeJobJournal invalid_result(
      {JournalExpectation{committed_event, static_cast<LogicalCommitResult>(99)}});
  result |= Check(invalid_result.verification_failed(),
                  "Journal rejects an out-of-domain result during setup");
  result |= Check(invalid_result.Commit(committed_event) == LogicalCommitResult::kOutcomeUnknown,
                  "invalid Journal setup can return only outcome unknown");

  const auto check_malformed_setup = [&](LogicalJobEvent malformed_event,
                                         std::string_view description) {
    FakeJobJournal malformed(
        {JournalExpectation{malformed_event, LogicalCommitResult::kCommitted}});
    int malformed_result = 0;
    malformed_result |= Check(malformed.verification_failed(),
                              std::string(description) + " is rejected during setup");
    malformed_result |=
        Check(malformed.Commit(malformed_event) == LogicalCommitResult::kOutcomeUnknown,
              std::string(description) + " returns outcome unknown");
    malformed_result |= Check(malformed.CopyObservations().empty(),
                              std::string(description) + " records no observation");
    malformed_result |= Check(malformed.remaining_expectations() == 1,
                              std::string(description) + " consumes no expectation");
    return malformed_result;
  };

  const std::vector<EventType> all_event_types = {
      EventType::kJobCreated,           EventType::kResourcesCommitted,
      EventType::kWorkerLaunchIntent,   EventType::kWorkerLaunchObserved,
      EventType::kWorkerRunning,        EventType::kCancelAccepted,
      EventType::kTerminateAccepted,    EventType::kTimeoutExpired,
      EventType::kWorkerCompleted,      EventType::kWorkerFailed,
      EventType::kProcessExitConfirmed, EventType::kSessionRetainRequested,
      EventType::kSessionRetained,      EventType::kFinalizationCompleted,
      EventType::kFinalizationFailed,   EventType::kTerminalOutcomeCommitted,
      EventType::kResourcesReleased,    EventType::kCleanupStatusRecorded,
      EventType::kLateWorkerEvent};
  for (const auto event_type : all_event_types) {
    const auto valid_event = MakeValidEvent(event_type);
    FakeJobJournal valid_mapping(
        {JournalExpectation{valid_event, LogicalCommitResult::kCommitted}});
    result |= Check(valid_mapping.Commit(valid_event) == LogicalCommitResult::kCommitted &&
                        valid_mapping.Verify() && valid_mapping.CopyObservations().size() == 1,
                    "every EventType accepts its exact schema payload mapping");
  }
  auto boundary_event = MakeValidEvent(EventType::kResourcesCommitted);
  boundary_event.sequence = std::numeric_limits<std::uint64_t>::max();
  auto& boundary_payload =
      std::get<sitometron::core::ResourcesCommittedPayload>(boundary_event.payload);
  boundary_payload.schema_version = std::numeric_limits<std::uint32_t>::max();
  boundary_payload.payload_utf8 = MakeMaxResourceJson();
  boundary_payload.allocation_digest = sitometron::core::Digest{std::string(kMaxResourceDigest)};
  result |= Check(boundary_payload.payload_utf8.size() == 65536,
                  "resource boundary fixture is exactly 65536 bytes");
  FakeJobJournal boundary_journal(
      {JournalExpectation{boundary_event, LogicalCommitResult::kCommitted}});
  result |= Check(boundary_journal.Commit(boundary_event) == LogicalCommitResult::kCommitted &&
                      boundary_journal.Verify(),
                  "schema maximum sequence, resource version, and valid JSON bytes remain valid");

  using MalformedMutation = void (*)(LogicalJobEvent&);
  struct MalformedCase {
    std::string_view description;
    EventType event_type;
    MalformedMutation mutate;
  };
  const std::vector<MalformedCase> malformed_cases = {
      {"unknown EventType", EventType::kJobCreated,
       [](LogicalJobEvent& event) { event.event_type = static_cast<EventType>(99); }},
      {"schema version zero", EventType::kJobCreated,
       [](LogicalJobEvent& event) { event.schema_version = 0; }},
      {"sequence zero", EventType::kJobCreated, [](LogicalJobEvent& event) { event.sequence = 0; }},
      {"invalid timestamp", EventType::kJobCreated,
       [](LogicalJobEvent& event) {
         event.recorded_at = DiagnosticTimestamp{"2026-02-30T00:00:00Z"};
       }},
      {"invalid Job identity", EventType::kJobCreated,
       [](LogicalJobEvent& event) { event.job_id = Uuid{std::string(kWorkerId)}; }},
      {"EventType/payload mismatch", EventType::kWorkerLaunchObserved,
       [](LogicalJobEvent& event) { event.event_type = EventType::kTerminalOutcomeCommitted; }},
      {"JobCreated UUID", EventType::kJobCreated,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::JobCreatedPayload>(event.payload).session_id =
             Uuid{std::string(kWorkerId)};
       }},
      {"ResourcesCommitted allocation StableId", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesCommittedPayload>(event.payload).allocation_id =
             StableId{"bad id"};
       }},
      {"ResourcesCommitted allocation digest", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesCommittedPayload>(event.payload)
             .allocation_digest.value = "not-a-digest";
       }},
      {"ResourcesCommitted schema StableId", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesCommittedPayload>(event.payload).schema_id =
             StableId{"bad id"};
       }},
      {"ResourcesCommitted schema version", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesCommittedPayload>(event.payload).schema_version = 0;
       }},
      {"ResourcesCommitted payload UTF-8", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         auto& payload = std::get<sitometron::core::ResourcesCommittedPayload>(event.payload);
         payload.payload_utf8 = std::string("\xC0\x80", 2);
         payload.allocation_digest = sitometron::core::Digest{std::string(kInvalidUtf8Digest)};
       }},
      {"ResourcesCommitted payload byte bound", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         auto& payload = std::get<sitometron::core::ResourcesCommittedPayload>(event.payload);
         payload.payload_utf8 = MakeMaxResourceJson();
         payload.payload_utf8.push_back(' ');
         payload.allocation_digest = sitometron::core::Digest{std::string(kOversizeResourceDigest)};
       }},
      {"ResourcesCommitted invalid JSON", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         auto& payload = std::get<sitometron::core::ResourcesCommittedPayload>(event.payload);
         payload.payload_utf8 = "not json";
         payload.allocation_digest = sitometron::core::Digest{std::string(kInvalidJsonDigest)};
       }},
      {"ResourcesCommitted digest mismatch", EventType::kResourcesCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesCommittedPayload>(event.payload)
             .allocation_digest.value = std::string(64, '0');
       }},
      {"WorkerLaunchIntent operation StableId", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).operation_id =
             StableId{"bad id"};
       }},
      {"WorkerLaunchIntent application StableId", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).application_id =
             StableId{"bad id"};
       }},
      {"WorkerLaunchIntent application version required", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload)
             .application_version.clear();
       }},
      {"WorkerLaunchIntent application version UTF-8", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).application_version =
             std::string("\xC0\x80", 2);
       }},
      {"WorkerLaunchIntent application version bound", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).application_version =
             RepeatUtf8("\xC3\xA9", 129);
       }},
      {"WorkerLaunchIntent bundle digest", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).bundle_sha256.value =
             "not-a-digest";
       }},
      {"WorkerLaunchIntent allocation StableId", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).allocation_id =
             StableId{"bad id"};
       }},
      {"WorkerLaunchIntent allocation digest", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload)
             .allocation_digest.value = "not-a-digest";
       }},
      {"WorkerLaunchIntent Worker UUID", EventType::kWorkerLaunchIntent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchIntentPayload>(event.payload).worker_id =
             Uuid{std::string(kJobId)};
       }},
      {"WorkerLaunchObserved operation StableId", EventType::kWorkerLaunchObserved,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerLaunchObservedPayload>(event.payload).operation_id =
             StableId{"bad id"};
       }},
      {"WorkerRunning Worker UUID", EventType::kWorkerRunning,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerRunningPayload>(event.payload).worker_id =
             Uuid{std::string(kJobId)};
       }},
      {"Principal required text", EventType::kCancelAccepted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::PrincipalPayload>(event.payload).principal_subject.clear();
       }},
      {"Principal UTF-8", EventType::kCancelAccepted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::PrincipalPayload>(event.payload).principal_subject =
             std::string("\xC0\x80", 2);
       }},
      {"Principal text bound", EventType::kCancelAccepted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::PrincipalPayload>(event.payload).principal_subject =
             RepeatUtf8("\xC3\xA9", 257);
       }},
      {"Timeout phase enum", EventType::kTimeoutExpired,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::TimeoutExpiredPayload>(event.payload).phase =
             static_cast<sitometron::core::TimeoutPhase>(99);
       }},
      {"Timeout generation", EventType::kTimeoutExpired,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::TimeoutExpiredPayload>(event.payload).timer_generation = 0;
       }},
      {"Worker event Worker UUID", EventType::kWorkerCompleted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerEventPayload>(event.payload).worker_id =
             Uuid{std::string(kJobId)};
       }},
      {"Worker event sequence", EventType::kWorkerCompleted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::WorkerEventPayload>(event.payload).event_sequence = 0;
       }},
      {"Process exit completion enum", EventType::kProcessExitConfirmed,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ProcessExitConfirmedPayload>(event.payload).completion_mode =
             static_cast<sitometron::core::CompletionMode>(99);
       }},
      {"Process exit operation StableId", EventType::kProcessExitConfirmed,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ProcessExitConfirmedPayload>(event.payload)
             .launch_operation_id = StableId{"bad id"};
       }},
      {"Session UUID", EventType::kSessionRetained,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::SessionPayload>(event.payload).session_id =
             Uuid{std::string(kWorkerId)};
       }},
      {"Session identity mismatch", EventType::kSessionRetained,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::SessionPayload>(event.payload).session_id =
             Uuid{std::string(kOtherV7Id)};
       }},
      {"Terminal outcome enum", EventType::kTerminalOutcomeCommitted,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::TerminalOutcomePayload>(event.payload).outcome =
             static_cast<sitometron::core::TerminalOutcome>(99);
       }},
      {"ResourcesReleased allocation StableId", EventType::kResourcesReleased,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesReleasedPayload>(event.payload).allocation_id =
             StableId{"bad id"};
       }},
      {"ResourcesReleased allocation digest", EventType::kResourcesReleased,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::ResourcesReleasedPayload>(event.payload)
             .allocation_digest.value = "not-a-digest";
       }},
      {"Cleanup status enum", EventType::kCleanupStatusRecorded,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::CleanupStatusPayload>(event.payload).status =
             static_cast<sitometron::core::CleanupStatus>(99);
       }},
      {"Late event original type enum", EventType::kLateWorkerEvent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::LateWorkerEventPayload>(event.payload).original_event_type =
             EventType::kWorkerRunning;
       }},
      {"Late event Worker UUID", EventType::kLateWorkerEvent,
       [](LogicalJobEvent& event) {
         std::get<sitometron::core::LateWorkerEventPayload>(event.payload).worker_id =
             Uuid{std::string(kJobId)};
       }},
      {"Late event sequence", EventType::kLateWorkerEvent, [](LogicalJobEvent& event) {
         std::get<sitometron::core::LateWorkerEventPayload>(event.payload).event_sequence = 0;
       }}};
  result |= Check(malformed_cases.size() == 44, "malformed case table has exactly 44 rows");
  for (const auto& malformed_case : malformed_cases) {
    auto malformed_event = MakeValidEvent(malformed_case.event_type);
    malformed_case.mutate(malformed_event);
    result |= check_malformed_setup(std::move(malformed_event), malformed_case.description);
  }

  FakeJobJournal unused({JournalExpectation{committed_event, LogicalCommitResult::kCommitted}});
  result |= Check(!unused.Verify() && unused.verification_failed(),
                  "unused Journal expectation latches verification failure");
  result |= Check(unused.Commit(committed_event) == LogicalCommitResult::kOutcomeUnknown &&
                      unused.remaining_expectations() == 1,
                  "sticky Journal verification failure cannot consume an expectation");
  return result;
}

int CheckEffectObservation() {
  PassiveEffectObserver observer(2);
  int result = 0;
  result |= Check(observer.Observe(EffectId::kLaunchWorkerOnce),
                  "observer records first explicit effect");
  result |= Check(observer.Observe(EffectId::kRetainSessionSameIdentity),
                  "observer records second explicit effect");
  const auto observations = observer.CopyObservations();
  result |= Check(observations.size() == 2, "observer keeps finite ordered observations");
  result |= Check(observations.at(0).ordinal == 0 &&
                      observations.at(0).effect_id == EffectId::kLaunchWorkerOnce &&
                      observations.at(1).ordinal == 1 &&
                      observations.at(1).effect_id == EffectId::kRetainSessionSameIdentity,
                  "observer assigns local ordinals without dispatching effects");
  result |= Check(!observer.Observe(EffectId::kPublishTerminalResult),
                  "observer rejects capacity overflow");
  result |= Check(observer.verification_failed(), "observer capacity overflow is sticky");
  result |= Check(observer.CopyObservations().size() == 2,
                  "observer overflow records no false observation");
  PassiveEffectObserver invalid_observer(1);
  result |= Check(!invalid_observer.Observe(EffectId::kInvalid) &&
                      invalid_observer.verification_failed() &&
                      invalid_observer.CopyObservations().empty(),
                  "observer rejects an invalid effect without a false observation");
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: core_ports_fakes_test <check-name>\n";
    return 2;
  }
  const std::string_view check = argv[1];
  if (check == "core_port_fake_contracts") {
    return CheckPortAndFakeContracts();
  }
  if (check == "job_fake_logical_commit_results") {
    return CheckLogicalCommitResults();
  }
  if (check == "job_fake_effect_observation") {
    return CheckEffectObservation();
  }
  std::cerr << "unknown check: " << check << '\n';
  return 2;
}
