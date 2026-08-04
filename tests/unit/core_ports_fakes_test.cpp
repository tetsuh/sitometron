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
