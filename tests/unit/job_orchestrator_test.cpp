#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "job_orchestrator_harness.hpp"

namespace {
using namespace sitometron::core;
using namespace sitometron::test;

constexpr std::string_view kPrincipal = "operator@example";
constexpr std::string_view kTimestamp = "2026-08-04T00:00:00Z";

int Check(bool condition, std::string_view message) {
  if (condition) return 0;
  std::cerr << "job_orchestrator: " << message << '\n';
  return 1;
}

Command Cancel(const Uuid& job) { return {1, CommandType::kCancel, job, std::string(kPrincipal)}; }
Command Terminate(const Uuid& job) {
  return {1, CommandType::kTerminate, job, std::string(kPrincipal)};
}

bool PayloadEqual(const EventPayload& lhs, const EventPayload& rhs) {
  if (lhs.index() != rhs.index()) return false;
  return std::visit(
      [](const auto& left, const auto& right) -> bool {
        using L = std::decay_t<decltype(left)>;
        using R = std::decay_t<decltype(right)>;
        if constexpr (!std::is_same_v<L, R>) {
          return false;
        } else if constexpr (std::is_same_v<L, EmptyPayload>) {
          return true;
        } else if constexpr (std::is_same_v<L, JobCreatedPayload>) {
          return left.session_id == right.session_id;
        } else if constexpr (std::is_same_v<L, ResourcesCommittedPayload>) {
          return left.allocation_id == right.allocation_id &&
                 left.allocation_digest == right.allocation_digest &&
                 left.schema_id == right.schema_id && left.schema_version == right.schema_version &&
                 left.payload_utf8 == right.payload_utf8;
        } else if constexpr (std::is_same_v<L, WorkerLaunchIntentPayload>) {
          return left.operation_id == right.operation_id &&
                 left.application_id == right.application_id &&
                 left.application_version == right.application_version &&
                 left.bundle_sha256 == right.bundle_sha256 &&
                 left.allocation_id == right.allocation_id &&
                 left.allocation_digest == right.allocation_digest &&
                 left.worker_id == right.worker_id;
        } else if constexpr (std::is_same_v<L, WorkerLaunchObservedPayload>) {
          return left.operation_id == right.operation_id && left.started == right.started;
        } else if constexpr (std::is_same_v<L, WorkerRunningPayload>) {
          return left.worker_id == right.worker_id;
        } else if constexpr (std::is_same_v<L, PrincipalPayload>) {
          return left.principal_subject == right.principal_subject;
        } else if constexpr (std::is_same_v<L, TimeoutExpiredPayload>) {
          return left.phase == right.phase && left.timer_generation == right.timer_generation;
        } else if constexpr (std::is_same_v<L, WorkerEventPayload>) {
          return left.worker_id == right.worker_id && left.event_sequence == right.event_sequence;
        } else if constexpr (std::is_same_v<L, ProcessExitConfirmedPayload>) {
          return left.completion_mode == right.completion_mode &&
                 left.launch_operation_id == right.launch_operation_id;
        } else if constexpr (std::is_same_v<L, SessionPayload>) {
          return left.session_id == right.session_id;
        } else if constexpr (std::is_same_v<L, TerminalOutcomePayload>) {
          return left.outcome == right.outcome;
        } else if constexpr (std::is_same_v<L, ResourcesReleasedPayload>) {
          return left.allocation_id == right.allocation_id &&
                 left.allocation_digest == right.allocation_digest;
        } else if constexpr (std::is_same_v<L, CleanupStatusPayload>) {
          return left.status == right.status;
        } else if constexpr (std::is_same_v<L, LateWorkerEventPayload>) {
          return left.original_event_type == right.original_event_type &&
                 left.worker_id == right.worker_id && left.event_sequence == right.event_sequence;
        } else {
          return false;
        }
      },
      lhs, rhs);
}

bool EnvelopeEqual(const LogicalJobEvent& actual, const ExpectedEnvelope& expected) {
  return actual.schema_version == expected.schema_version && actual.sequence == expected.sequence &&
         actual.event_type == expected.event_type &&
         actual.recorded_at.rfc3339 == expected.recorded_at && actual.job_id == expected.job_id &&
         PayloadEqual(actual.payload, expected.payload);
}

bool WaitForTerminal(JobOrchestratorHarness& harness, std::uint64_t sequence,
                     Completion::Code code) {
  if (code == Completion::Code::kSuccess)
    return harness.WaitUntil(sequence, WriterPhase::kResponseReleased);
  return harness.WaitUntil(sequence, WriterPhase::kTurnFinished) ||
         harness.WaitUntil(sequence, WriterPhase::kFailureDisposed);
}

int ConsumeCompletion(JobOrchestratorHarness& harness, const IngressResult& admitted,
                      Completion::Code expected, std::optional<RejectionReason> rejection = {}) {
  int result = 0;
  result |= Check(admitted.code == IngressCode::kAdmitted && admitted.ingress_sequence != 0,
                  "admitted input has a nonzero ingress sequence");
  const auto completion_count_before = admitted.completion_count_before;
  result |= Check(WaitForTerminal(harness, admitted.ingress_sequence, expected),
                  "turn-specific terminal barrier completes");
  result |= Check(harness.completion_count() >= completion_count_before + 1,
                  "the admitted source has one terminal completion despite autonomous later turns");
  const auto completion = harness.TakeCompletion(admitted.ingress_sequence);
  result |= Check(completion.has_value() && completion->code == expected,
                  "finite completion has the expected terminal disposition");
  if (rejection) {
    result |=
        Check(completion && completion->rejection && completion->rejection->reason == *rejection,
              "reducer rejection is retained in the terminal disposition");
  }
  result |= Check(!harness.TakeCompletion(admitted.ingress_sequence).has_value(),
                  "second completion take is empty");
  return result;
}

int ConsumeShutdownCompletion(JobOrchestratorHarness& harness, const IngressResult& marker) {
  int result = 0;
  const auto before = marker.completion_count_before;
  result |= Check(harness.WaitUntil(marker.ingress_sequence, WriterPhase::kShutdownMarker),
                  "shutdown marker reaches its keyed control barrier");
  result |=
      Check(!harness.sealed(), "processed marker alone does not seal while caller controls drain");
  result |= Check(harness.completion_count() == before + 1,
                  "coalesced shutdown callers share one completion relationship");
  const auto completion = harness.TakeCompletion(marker.ingress_sequence);
  result |= Check(completion.has_value() && completion->code == Completion::Code::kSuccess,
                  "shutdown marker completion is released exactly once");
  result |= Check(!harness.TakeCompletion(marker.ingress_sequence).has_value(),
                  "shutdown marker has no second completion");
  return result;
}

int ExpectCommitted(JobOrchestratorHarness& harness, const IngressResult& admitted,
                    EventType expected_event) {
  int result = ConsumeCompletion(harness, admitted, Completion::Code::kSuccess);
  const auto journal = harness.CopyJournalAttempts();
  result |= Check(!journal.empty() && journal.back().event_type == expected_event,
                  "Journal event follows the accepted reducer proposal");
  return result;
}

int CheckTraceMetadata(const std::vector<TraceRecord>& trace) {
  int result = 0;
  for (std::size_t index = 0; index < trace.size(); ++index) {
    result |= Check(trace[index].ordinal == index + 1, "global trace ordinals are contiguous");
    result |= Check(trace[index].writer_context, "trace record belongs to the one writer context");
    result |= Check(!trace[index].ingress_mutex_held,
                    "Journal and external actions run without the ingress mutex");
  }
  return result;
}

int CheckExactTurnTrace(const std::vector<TraceRecord>& trace, std::uint64_t sequence,
                        const ExpectedTrace& expected) {
  std::vector<TraceRecord> actual;
  for (const auto& record : trace)
    if (record.journal_sequence == sequence) actual.push_back(record);
  for (std::size_t index = 0; index < actual.size(); ++index) actual[index].ordinal = index + 1;
  return Check(actual == expected.records,
               "focused turn trace matches exact commit/effect/source order");
}

int CheckGeneratedIdentities(const JobOrchestratorHarness& harness, const IdFixtures& ids) {
  const auto generated = harness.generated_identities();
  return Check(generated && generated->job_id == ids.primary_job &&
                   generated->session_id == ids.primary_job && generated->worker_id == ids.worker &&
                   generated->launch_operation_id == ids.launch_operation,
               "identity source supplies Job/Session, Worker, and launch-operation identities");
}

int DriveToRunning(JobOrchestratorHarness& harness, const IdFixtures& ids,
                   const AllocationFixture& allocation) {
  int result = 0;
  result |= Check(harness.resident_count() == 0, "lifecycle begins with no resident Job");
  result |= ExpectCommitted(harness, harness.Create(), EventType::kJobCreated);
  result |= CheckGeneratedIdentities(harness, ids);
  result |= ExpectCommitted(
      harness,
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
      EventType::kResourcesCommitted);
  result |= ExpectCommitted(harness, harness.SubmitGeneratedLaunchIntent(allocation),
                            EventType::kWorkerLaunchIntent);
  result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kPreparation),
                  "preparation timer lease is retained after its arm effect");
  result |= Check(harness.RetainResourcesReleasedLease(ids.primary_job),
                  "release lease is retained after allocation commitment");
  const auto launched = harness.TakeRunnerCandidate();
  result |= Check(launched.has_value(), "launch handoff stages one inert launch observation");
  if (launched)
    result |= ExpectCommitted(harness, harness.SubmitLaunchObserved(*launched),
                              EventType::kWorkerLaunchObserved);
  result |=
      Check(harness.VerifyFakes(), "launch candidate is consumed before the next producer phase");
  result |= ExpectCommitted(
      harness, harness.SubmitWorkerRunning(MakeWorkerRunning(ids.primary_job, ids.worker)),
      EventType::kWorkerRunning);
  result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kExecution),
                  "execution timer lease is retained after its arm effect");
  return result;
}

int DriveToFinalizing(JobOrchestratorHarness& harness, const IdFixtures& ids,
                      const AllocationFixture& allocation, std::uint64_t worker_sequence = 1) {
  int result = DriveToRunning(harness, ids, allocation);
  result |= ExpectCommitted(
      harness,
      harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, worker_sequence)),
      EventType::kWorkerCompleted);
  result |= ExpectCommitted(
      harness, harness.SubmitSessionRetainRequested(MakeSessionRetainRequested(ids.primary_job)),
      EventType::kSessionRetainRequested);
  const auto retained = harness.TakeSessionCandidate();
  result |= Check(retained.has_value(), "Session handoff stages one inert retained candidate");
  if (retained)
    result |= ExpectCommitted(harness, harness.SubmitSessionRetained(*retained),
                              EventType::kSessionRetained);
  result |=
      Check(harness.VerifyFakes(), "Session candidate is consumed before finalization continues");
  result |= ExpectCommitted(
      harness, harness.SubmitFinalizationCompleted(MakeFinalizationCompleted(ids.primary_job)),
      EventType::kFinalizationCompleted);
  return result;
}

ExpectedTrace SuccessfulTrace(const IdFixtures& ids) {
  ExpectedTrace expected;
  expected.Turn(1, EventType::kJobCreated, {});
  expected.Turn(2, EventType::kResourcesCommitted,
                {{EffectId::kArmPreparationTimeout, "timer:arm:preparation:1"}},
                {"source:timer:preparation", "source:resources_released"});
  expected.Turn(3, EventType::kWorkerLaunchIntent,
                {{EffectId::kLaunchWorkerOnce, "handoff:launch:" + ids.launch_operation.value}},
                {"source:worker", "source:process_exit"});
  expected.Turn(4, EventType::kWorkerLaunchObserved, {});
  expected.Turn(5, EventType::kWorkerRunning,
                {{EffectId::kDisarmPreparationTimeout, "timer:disarm:preparation:1"},
                 {EffectId::kArmExecutionTimeout, "timer:arm:execution:1"}});
  expected.Turn(6, EventType::kWorkerCompleted, {});
  expected.Turn(7, EventType::kSessionRetainRequested,
                {{EffectId::kRetainSessionSameIdentity, "handoff:session-retain"}});
  expected.Turn(8, EventType::kSessionRetained, {});
  expected.Turn(9, EventType::kFinalizationCompleted, {});
  expected.Turn(10, EventType::kTerminalOutcomeCommitted,
                {{EffectId::kDisarmExecutionTimeout, "timer:disarm:execution:1"},
                 {EffectId::kAckTerminalWorkerEventIfPending, "ack:terminal:1"},
                 {EffectId::kPublishTerminalResult, "publish:succeeded"},
                 {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
                  "timer:arm:process_exit_confirmation:1"}},
                {"source:timer:process_exit_confirmation", "source:cleanup"});
  expected.Turn(11, EventType::kLateWorkerEvent, {{EffectId::kAckLateWorkerEvent, "ack:late:2"}});
  expected.Turn(12, EventType::kProcessExitConfirmed,
                {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop"},
                 {EffectId::kDisarmProcessExitConfirmationTimeout,
                  "timer:disarm:process_exit_confirmation:1"}});
  expected.Turn(13, EventType::kResourcesReleased, {});
  expected.Turn(14, EventType::kCleanupStatusRecorded, {});
  return expected;
}

std::vector<ExpectedEnvelope> ExpectedSuccessfulEnvelopes(const IdFixtures& ids,
                                                          const AllocationFixture& allocation) {
  const auto event = [&](std::uint64_t sequence, EventType type, EventPayload payload) {
    return ExpectedEnvelope{
        1, sequence, type, std::string(kTimestamp), ids.primary_job, std::move(payload)};
  };
  return {
      event(1, EventType::kJobCreated, JobCreatedPayload{ids.primary_job}),
      event(2, EventType::kResourcesCommitted,
            ResourcesCommittedPayload{allocation.id, allocation.digest, allocation.schema_id,
                                      allocation.schema_version, allocation.payload_utf8}),
      event(3, EventType::kWorkerLaunchIntent,
            WorkerLaunchIntentPayload{ids.launch_operation, ids.application, "1.0.0",
                                      Digest{std::string(64, 'a')}, allocation.id,
                                      allocation.digest, ids.worker}),
      event(4, EventType::kWorkerLaunchObserved,
            WorkerLaunchObservedPayload{ids.launch_operation, true}),
      event(5, EventType::kWorkerRunning, WorkerRunningPayload{ids.worker}),
      event(6, EventType::kWorkerCompleted, WorkerEventPayload{ids.worker, 1}),
      event(7, EventType::kSessionRetainRequested, SessionPayload{ids.primary_job}),
      event(8, EventType::kSessionRetained, SessionPayload{ids.primary_job}),
      event(9, EventType::kFinalizationCompleted, EmptyPayload{}),
      event(10, EventType::kTerminalOutcomeCommitted,
            TerminalOutcomePayload{TerminalOutcome::kSucceeded}),
      event(11, EventType::kLateWorkerEvent,
            LateWorkerEventPayload{EventType::kWorkerCompleted, ids.worker, 2}),
      event(12, EventType::kProcessExitConfirmed,
            ProcessExitConfirmedPayload{CompletionMode::kCooperative, ids.launch_operation}),
      event(13, EventType::kResourcesReleased,
            ResourcesReleasedPayload{allocation.id, allocation.digest}),
      event(14, EventType::kCleanupStatusRecorded, CleanupStatusPayload{CleanupStatus::kCompleted}),
  };
}

int CheckExactEnvelopes(const std::vector<LogicalJobEvent>& actual,
                        const std::vector<ExpectedEnvelope>& expected) {
  int result =
      Check(actual.size() == expected.size(), "all expected logical envelopes are present");
  const auto count = std::min(actual.size(), expected.size());
  for (std::size_t index = 0; index < count; ++index)
    result |= Check(EnvelopeEqual(actual[index], expected[index]),
                    "all six envelope fields and payload alternatives match exactly");
  return result;
}

int JobIngressLinearizationOrder() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = 0;
  result |= Check(harness.ArmPause(WriterPhase::kBeforeDequeue) &&
                      harness.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "queue pressure window pauses before dequeue");
  const auto completion_before = harness.completion_count();
  const auto first = harness.Create();
  const auto second =
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation));
  result |= Check(first.code == IngressCode::kAdmitted && first.ingress_sequence == 1,
                  "first insertion receives ingress sequence one");
  result |= Check(second.code == IngressCode::kAdmitted && second.ingress_sequence == 2,
                  "second insertion receives ingress sequence two");
  result |= Check(harness.completion_count() == completion_before,
                  "no completion is observed before the paused writer dequeues");
  result |= Check(
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)).code ==
          IngressCode::kNormalFull,
      "normal admission cannot use the critical reserve");
  result |= Check(harness.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "queue pressure window releases deterministically");
  result |= ConsumeCompletion(harness, first, Completion::Code::kSuccess);
  result |= ConsumeCompletion(harness, second, Completion::Code::kSuccess);
  const auto sequences = harness.CopyIngressSequences();
  result |= Check(sequences.size() >= 2 && sequences[0] == 1 && sequences[1] == 2,
                  "dequeue order equals strict ingress order");

  JobOrchestratorHarness worker(PositiveConfig());
  result |= DriveToRunning(worker, ids, allocation);
  result |=
      Check(worker.ArmAdmissionPause(), "Worker registration-to-first-insertion pause is armed");
  std::optional<IngressResult> first_worker;
  std::optional<IngressResult> retry_worker;
  std::thread producer([&] {
    first_worker = worker.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  });
  result |= Check(worker.WaitForAdmissionPause(),
                  "first Worker registration reaches the insertion linearization barrier");
  std::thread retry([&] {
    retry_worker = worker.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  });
  result |= Check(worker.WaitForAdmissionAttempts(2),
                  "retry reaches admission while first insertion remains paused");
  result |= Check(worker.ReleaseAdmissionPause(),
                  "first Worker insertion is released before retry can coalesce");
  producer.join();
  retry.join();
  result |= Check(first_worker && first_worker->code == IngressCode::kAdmitted &&
                      first_worker->ingress_sequence != 0,
                  "first Worker delivery receives a nonzero sequence");
  result |= Check(retry_worker && retry_worker->code == IngressCode::kCoalescedPending &&
                      retry_worker->ingress_sequence == first_worker->ingress_sequence,
                  "two-thread retry coalesces only to the first assigned sequence");

  Config boundary = PositiveConfig();
  boundary.initial_ingress_sequence = std::numeric_limits<std::uint64_t>::max();
  JobOrchestratorHarness exhausted(boundary);
  const auto maximum = exhausted.Create();
  result |= Check(maximum.code == IngressCode::kAdmitted &&
                      maximum.ingress_sequence == std::numeric_limits<std::uint64_t>::max(),
                  "maximum ingress sequence is assigned once");
  result |=
      Check(exhausted.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation))
                    .code == IngressCode::kServiceFailed,
            "ingress sequence never wraps, reuses, or returns zero");
  return result;
}

int JobIngressSingleWriter() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  const auto first = harness.Create();
  const auto second =
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, EmptyAllocation()));
  int result = ConsumeCompletion(harness, first, Completion::Code::kSuccess);
  result |= ConsumeCompletion(harness, second, Completion::Code::kSuccess);
  result |= Check(harness.writer_turn_count() == 2 && harness.max_concurrent_writer_turns() == 1,
                  "one writer execution context owns all reducer turns");
  result |= CheckTraceMetadata(harness.CopyTrace());
  return result;
}

int JobIngressSourceClassification() {
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = 0;
  {
    JobOrchestratorHarness provisional(PositiveConfig());
    result |= Check(provisional.ArmPause(WriterPhase::kBeforeDequeue) &&
                        provisional.ArmBarrier(WriterPhase::kBeforeDequeue),
                    "provisional terminate classification pauses before dequeue");
    const auto created = provisional.SubmitJobCreated(MakeJobCreated(ids.primary_job));
    const auto terminate = provisional.SubmitTerminate(Terminate(ids.primary_job));
    result |=
        Check(created.code == IngressCode::kAdmitted && terminate.code == IngressCode::kAdmitted,
              "provisional terminate acquires a critical source gate automatically");
    result |= Check(provisional.critical_occupancy() == 1,
                    "provisional terminate consumes one critical permit");
    result |= Check(provisional.Release(created.ingress_sequence, WriterPhase::kBeforeDequeue),
                    "provisional queue is released through its keyed barrier");
    result |= ConsumeCompletion(provisional, created, Completion::Code::kSuccess);
    result |= ConsumeCompletion(provisional, terminate, Completion::Code::kSuccess);
  }
  {
    JobOrchestratorHarness unknown(PositiveConfig());
    const auto turn = unknown.SubmitTerminate(Terminate(ids.secondary_job));
    result |= Check(turn.code == IngressCode::kAdmitted, "unknown terminate uses normal admission");
    result |= ConsumeCompletion(unknown, turn, Completion::Code::kReducerRejection,
                                RejectionReason::kJobNotFound);
    result |=
        Check(unknown.journal_attempts() == 0, "unknown terminate rejection creates no Journal");
    const auto malformed_command =
        unknown.SubmitTerminate(Command{0, CommandType::kInvalid, ids.secondary_job, ""});
    result |= ConsumeCompletion(unknown, malformed_command, Completion::Code::kReducerRejection,
                                RejectionReason::kInvalidEventPayload);
    const auto malformed_candidate =
        unknown.SubmitWorker(RawCandidateEvent{1, ids.secondary_job, "worker_running", "{}"});
    result |= ConsumeCompletion(unknown, malformed_candidate, Completion::Code::kReducerRejection,
                                RejectionReason::kInvalidEventPayload);
    result |= Check(unknown.journal_attempts() == 0,
                    "malformed unknown command and candidate create no Journal");
  }
  {
    JobOrchestratorHarness staged(PositiveConfig());
    result |= ExpectCommitted(staged, staged.Create(), EventType::kJobCreated);
    result |= Check(staged.ArmPause(WriterPhase::kBeforeCommit) &&
                        staged.ArmBarrier(WriterPhase::kBeforeCommit),
                    "derived source plan pauses before its binding commit");
    const auto resources =
        staged.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation));
    result |= Check(staged.WaitUntil(resources.ingress_sequence, WriterPhase::kBeforeCommit),
                    "derived source plan reaches the precommit visibility barrier");
    result |= Check(!staged.RetainResourcesReleasedLease(ids.primary_job),
                    "source gate is not admission-visible before binding commit");
    result |= Check(staged.Release(resources.ingress_sequence, WriterPhase::kBeforeCommit),
                    "source plan barrier releases through logical commit");
    result |= ConsumeCompletion(staged, resources, Completion::Code::kSuccess);
    result |= Check(staged.RetainResourcesReleasedLease(ids.primary_job),
                    "source gate activates after logical binding commit");
  }
  {
    JobOrchestratorHarness timer(PositiveConfig());
    result |= ExpectCommitted(timer, timer.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(
        timer, timer.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
        EventType::kResourcesCommitted);
    result |= Check(timer.ArmPause(WriterPhase::kBeforeDequeue) &&
                        timer.ArmBarrier(WriterPhase::kBeforeDequeue),
                    "timer delivery pauses before its writer turn");
    const auto timeout =
        timer.SubmitTimeout(TimerNotification{ids.primary_job, TimeoutPhase::kPreparation, 1});
    result |= Check(!timeout.discarded && timeout.admitted.code == IngressCode::kAdmitted,
                    "timer effect acquires its gate before notification admission");
    result |= Check(timer.RetainTimerLease(ids.primary_job, TimeoutPhase::kPreparation),
                    "timer lease is retained only after its arm effect");
    result |= Check(timer.Release(timeout.admitted.ingress_sequence, WriterPhase::kBeforeDequeue),
                    "timer delivery barrier releases deterministically");
    result |= ExpectCommitted(timer, timeout.admitted, EventType::kTimeoutExpired);
    const auto timer_attempts = timer.journal_attempts();
    const auto stale =
        timer.SubmitTimeout(TimerNotification{ids.primary_job, TimeoutPhase::kPreparation, 1});
    const auto mismatched =
        timer.SubmitTimeout(TimerNotification{ids.primary_job, TimeoutPhase::kExecution, 1});
    const auto raw_timeout =
        timer.SubmitLaunchObserved(MakeTimeout(ids.primary_job, TimeoutPhase::kPreparation, 1));
    result |=
        Check(stale.discarded && mismatched.discarded &&
                  raw_timeout.code == IngressCode::kAdmissionClosed &&
                  raw_timeout.ingress_sequence == 0 && timer.journal_attempts() == timer_attempts,
              "stale, disarmed, and raw timeout notifications discard before ingress");
  }
  {
    JobOrchestratorHarness worker(PositiveConfig());
    result |= DriveToRunning(worker, ids, allocation);
    const auto terminate = worker.SubmitTerminate(Terminate(ids.primary_job));
    result |= Check(terminate.code == IngressCode::kAdmitted,
                    "resident terminate is automatically critical from its provisional gate");
    result |= ExpectCommitted(worker, terminate, EventType::kTerminateAccepted);
    result |= Check(worker.TakeRunnerCandidate().has_value() && worker.VerifyFakes(),
                    "resident forced-stop candidate is taken and runner fake verifies");
  }
  {
    JobOrchestratorHarness cleanup(PositiveConfig());
    result |= DriveToFinalizing(cleanup, ids, allocation);
    result |= ExpectCommitted(cleanup,
                              cleanup.SubmitTerminalOutcome(MakeTerminalOutcome(
                                  ids.primary_job, TerminalOutcome::kSucceeded)),
                              EventType::kTerminalOutcomeCommitted);
    const auto cleanup_turn = cleanup.SubmitCleanup(MakeCleanupCompleted(ids.primary_job));
    result |= Check(cleanup_turn.code == IngressCode::kAdmitted,
                    "cleanup effect acquires its gate at terminal commit");
    result |= ExpectCommitted(cleanup, cleanup_turn, EventType::kCleanupStatusRecorded);
  }
  {
    JobOrchestratorHarness concurrent(PositiveConfig());
    std::barrier gate(2);
    IngressResult first{};
    IngressResult second{};
    std::thread a([&] {
      gate.arrive_and_wait();
      first = concurrent.Create();
    });
    std::thread b([&] {
      gate.arrive_and_wait();
      second = concurrent.Create();
    });
    a.join();
    b.join();
    const bool first_admitted = first.code == IngressCode::kAdmitted;
    const bool second_admitted = second.code == IngressCode::kAdmitted;
    result |= Check(first_admitted || second_admitted,
                    "concurrent creation has a bounded admitted generation");
    if (first_admitted && second_admitted) {
      result |= ConsumeCompletion(concurrent, first, Completion::Code::kSuccess);
      result |= ConsumeCompletion(concurrent, second, Completion::Code::kSuccess);
    } else {
      const auto admitted = first_admitted ? first : second;
      const auto duplicate = first_admitted ? second : first;
      result |= Check(duplicate.code == IngressCode::kAlreadyPending,
                      "overlapping creation follows bounded already-pending behavior");
      result |= ConsumeCompletion(concurrent, admitted, Completion::Code::kSuccess);
    }
    result |= Check(concurrent.generated_identities().has_value(),
                    "admitted creation retains its generated identity bundle");
  }
  {
    // Identity bundles are resident-owned: creating B cannot redirect a later
    // launch for A to B's Worker/operation identities.
    JobOrchestratorHarness interleaved(PositiveConfig());
    result |= ExpectCommitted(interleaved, interleaved.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(interleaved, interleaved.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(
        interleaved,
        interleaved.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
        EventType::kResourcesCommitted);
    result |= ExpectCommitted(
        interleaved,
        interleaved.SubmitResourcesCommitted(MakeResourcesCommitted(ids.secondary_job, allocation)),
        EventType::kResourcesCommitted);
    result |= ExpectCommitted(interleaved,
                              interleaved.SubmitGeneratedLaunchIntent(ids.primary_job, allocation),
                              EventType::kWorkerLaunchIntent);
    result |= ExpectCommitted(
        interleaved, interleaved.SubmitGeneratedLaunchIntent(ids.secondary_job, allocation),
        EventType::kWorkerLaunchIntent);
    const auto launches = interleaved.CopyLaunchRequests();
    result |= Check(
        launches.size() == 2 && launches[0].job_id == ids.primary_job &&
            launches[0].intent.worker_id == ids.worker &&
            launches[0].intent.operation_id == ids.launch_operation &&
            launches[1].job_id == ids.secondary_job &&
            launches[1].intent.worker_id == ids.secondary_worker &&
            launches[1].intent.operation_id == ids.secondary_launch_operation,
        "interleaved resident launches retain their own generated Worker and operation identities");
  }
  {
    JobOrchestratorHarness conflict(PositiveConfig());
    result |= ExpectCommitted(conflict, conflict.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(
        conflict,
        conflict.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
        EventType::kResourcesCommitted);
    const auto before = conflict.journal_attempts();
    const auto turn = conflict.SubmitConflictingResources(ids.primary_job);
    result |= ConsumeCompletion(conflict, turn, Completion::Code::kReducerRejection,
                                RejectionReason::kInvariantViolation);
    result |=
        Check(conflict.journal_attempts() == before && !conflict.failed(),
              "well-formed binding conflict rejects in reducer without Journal or service failure");
  }
  return result;
}

int DriveToRetainedCriticalPopulation(JobOrchestratorHarness& harness, const IdFixtures& ids,
                                      const AllocationFixture& allocation,
                                      const Uuid& expected_job) {
  int result = 0;
  // Each lease is retained only after its normal reducer-driven registration point;
  // no test injects a lifecycle position or an arbitrary gate state.
  const auto created = harness.Create();
  result |= ExpectCommitted(harness, created, EventType::kJobCreated);
  const auto generated = harness.generated_identities();
  result |= Check(generated.has_value() && generated->job_id == expected_job &&
                      generated->session_id == expected_job,
                  "resident population obtains the expected same-identity source result");
  if (!generated) return result;
  IdFixtures resident = ids;
  resident.primary_job = generated->job_id;
  resident.worker = generated->worker_id;
  resident.launch_operation = generated->launch_operation_id;
  result |= ExpectCommitted(
      harness,
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(resident.primary_job, allocation)),
      EventType::kResourcesCommitted);
  result |= Check(harness.RetainTimerLease(resident.primary_job, TimeoutPhase::kPreparation),
                  "preparation timer lease is retained after preparation arm");
  result |= Check(harness.RetainResourcesReleasedLease(resident.primary_job),
                  "resource-release lease is retained after allocation commitment");
  result |= ExpectCommitted(harness, harness.SubmitGeneratedLaunchIntent(allocation),
                            EventType::kWorkerLaunchIntent);
  result |= Check(harness.RetainWorkerLease(resident.primary_job) &&
                      harness.RetainProcessExitLease(resident.primary_job),
                  "Worker and process-exit leases are retained before runner handoff");
  const auto observed = harness.TakeRunnerCandidate();
  result |= Check(observed.has_value(), "launch observation is taken from the runner fake");
  if (observed)
    result |= ExpectCommitted(harness, harness.SubmitLaunchObserved(*observed),
                              EventType::kWorkerLaunchObserved);
  result |= ExpectCommitted(
      harness,
      harness.SubmitWorkerRunning(MakeWorkerRunning(resident.primary_job, resident.worker)),
      EventType::kWorkerRunning);
  result |= Check(harness.RetainTimerLease(resident.primary_job, TimeoutPhase::kExecution),
                  "execution timer lease is retained after execution arm");
  result |= ExpectCommitted(harness, harness.SubmitCancel(Cancel(resident.primary_job)),
                            EventType::kCancelAccepted);
  result |= Check(harness.CancelRunnerCandidate(),
                  "critical-population cooperative-stop candidate is explicitly cancelled");
  result |= Check(harness.RetainTimerLease(resident.primary_job, TimeoutPhase::kCooperativeStop),
                  "cooperative-stop timer lease is retained after cancel arms it");
  result |= ExpectCommitted(
      harness, harness.SubmitWorker(MakeWorkerCompleted(resident.primary_job, resident.worker, 1)),
      EventType::kWorkerCompleted);
  result |= ExpectCommitted(
      harness,
      harness.SubmitSessionRetainRequested(MakeSessionRetainRequested(resident.primary_job)),
      EventType::kSessionRetainRequested);
  const auto retained = harness.TakeSessionCandidate();
  result |= Check(retained.has_value(), "critical-population Session handoff stages one candidate");
  if (retained)
    result |= ExpectCommitted(harness, harness.SubmitSessionRetained(*retained),
                              EventType::kSessionRetained);
  result |= ExpectCommitted(
      harness, harness.SubmitFinalizationCompleted(MakeFinalizationCompleted(resident.primary_job)),
      EventType::kFinalizationCompleted);
  result |= ExpectCommitted(harness,
                            harness.SubmitTerminalOutcome(MakeTerminalOutcome(
                                resident.primary_job, TerminalOutcome::kCancelled)),
                            EventType::kTerminalOutcomeCommitted);
  result |=
      Check(harness.RetainTimerLease(resident.primary_job, TimeoutPhase::kProcessExitConfirmation),
            "process-exit-confirmation lease is retained after terminal commit");
  result |= Check(harness.RetainCleanupLease(resident.primary_job),
                  "cleanup lease is retained only after terminal commit");
  result |= Check(harness.VerifyFakes(),
                  "runner candidate is consumed before critical permits are retained");
  return result;
}

int JobIngressCapacityAndReserve() {
  int result = 0;
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  result |=
      Check(PositiveConfig().critical_reserve() == 19 && PositiveConfig().total_capacity() == 21,
            "positive fixture derives J=2 N=2 R=19 T=21");
  // Completion ownership is reserved at FIFO insertion, independent of when
  // callers take prior dispositions. Hold T-1 results, then admit two more.
  JobOrchestratorHarness completion_reserve(PositiveConfig());
  const auto completion_create = completion_reserve.Create();
  result |= Check(
      completion_reserve.WaitUntil(completion_create.ingress_sequence, WriterPhase::kTurnFinished),
      "first completion is retained without taking its slot");
  for (std::size_t count = 1; count < PositiveConfig().total_capacity() - 1; ++count) {
    const auto duplicate = completion_reserve.SubmitJobCreated(MakeJobCreated(ids.primary_job));
    result |= Check(
        duplicate.code == IngressCode::kAdmitted &&
            completion_reserve.WaitUntil(duplicate.ingress_sequence, WriterPhase::kTurnFinished),
        "untaken completion retains its pre-reserved result slot");
  }
  const auto reserved_one = completion_reserve.SubmitJobCreated(MakeJobCreated(ids.primary_job));
  const auto reserved_two = completion_reserve.SubmitJobCreated(MakeJobCreated(ids.primary_job));
  result |= Check(
      reserved_one.code == IngressCode::kAdmitted && reserved_two.code == IngressCode::kAdmitted &&
          completion_reserve.WaitUntil(reserved_one.ingress_sequence, WriterPhase::kTurnFinished) &&
          completion_reserve.WaitUntil(reserved_two.ingress_sequence, WriterPhase::kTurnFinished),
      "T-1 untaken results leave two independently reserved admissions safe");
  JobOrchestratorHarness population(PositiveConfig());
  result |= DriveToRetainedCriticalPopulation(population, ids, allocation, ids.primary_job);
  result |= Check(population.critical_occupancy() == 9,
                  "first resident preacquires all nine bounded critical source permits");
  result |= DriveToRetainedCriticalPopulation(population, ids, allocation, ids.secondary_job);
  result |= Check(population.critical_occupancy() == 18,
                  "two residents preacquire eighteen critical source permits");
  result |= Check(population.ArmPause(WriterPhase::kBeforeDequeue) &&
                      population.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "capacity population pauses before simultaneous admission");
  const auto normal_one = population.SubmitCancel(Cancel(ids.primary_job));
  const auto normal_two = population.SubmitCancel(Cancel(ids.secondary_job));
  const auto critical_one = population.SubmitTerminate(Terminate(ids.primary_job));
  const auto critical_two = population.SubmitTerminate(Terminate(ids.secondary_job));
  const auto marker = population.SubmitShutdown();
  result |=
      Check(normal_one.code == IngressCode::kAdmitted && normal_two.code == IngressCode::kAdmitted,
            "two normal terminal inputs occupy N while paused");
  result |= Check(critical_one.code == IngressCode::kAdmitted &&
                      critical_two.code == IngressCode::kAdmitted &&
                      marker.code == IngressCode::kAdmitted,
                  "two resident terminate inputs and shutdown occupy critical reserve");
  result |= Check(population.normal_occupancy() == 2 && population.critical_occupancy() == 19 &&
                      population.total_occupancy() == 21,
                  "N=2 and R=19 are simultaneously occupied without eviction");
  Config zero_jobs = PositiveConfig();
  zero_jobs.max_jobs = 0;
  Config zero_normal = PositiveConfig();
  zero_normal.normal_capacity = 0;
  Config overflow_jobs = PositiveConfig();
  overflow_jobs.max_jobs = std::numeric_limits<std::size_t>::max();
  Config overflow_total = PositiveConfig();
  overflow_total.max_jobs = 1;
  overflow_total.normal_capacity = std::numeric_limits<std::size_t>::max();
  Config zero_trace = PositiveConfig();
  zero_trace.trace_capacity = 0;
  Config zero_completion = PositiveConfig();
  zero_completion.completion_capacity = 0;
  Config undersized_completion = PositiveConfig();
  undersized_completion.completion_capacity = undersized_completion.total_capacity() - 1;
  Config zero_handoff = PositiveConfig();
  zero_handoff.handoff_capacity = 0;
  Config insufficient_handoff = PositiveConfig();
  insufficient_handoff.handoff_capacity = 3;
  Config zero_ack = PositiveConfig();
  zero_ack.ack_capacity = 0;
  Config zero_callbacks = PositiveConfig();
  zero_callbacks.callback_registration_capacity = 0;
  Config insufficient_callbacks = PositiveConfig();
  insufficient_callbacks.callback_registration_capacity = 1;
  for (const auto& invalid :
       {zero_jobs, zero_normal, overflow_jobs, overflow_total, zero_trace, zero_completion,
        undersized_completion, zero_handoff, insufficient_handoff, zero_ack, zero_callbacks,
        insufficient_callbacks}) {
    try {
      JobOrchestratorHarness rejected(invalid);
      result |= Check(false, "invalid finite capacity configuration must fail startup");
    } catch (const std::exception&) {
      result |= Check(true, "invalid finite capacity configuration fails startup");
    }
  }
  Config precedence = PositiveConfig();
  precedence.max_jobs = 1;
  precedence.normal_capacity = 1;
  JobOrchestratorHarness resident_first(precedence);
  result |= Check(resident_first.ArmPause(WriterPhase::kBeforeDequeue) &&
                      resident_first.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "resident precedence window pauses before dequeue");
  const auto precedence_first = resident_first.Create();
  const auto precedence_second = resident_first.Create();
  result |= Check(precedence_first.code == IngressCode::kAdmitted,
                  "first provisional creation occupies resident and normal capacity");
  result |= Check(precedence_second.code == IngressCode::kResidentLimit,
                  "resident_limit takes precedence when both limits are exhausted");
  result |=
      Check(resident_first.Release(precedence_first.ingress_sequence, WriterPhase::kBeforeDequeue),
            "resident precedence window releases deterministically");
  result |= ConsumeCompletion(resident_first, precedence_first, Completion::Code::kSuccess);
  JobOrchestratorHarness claims(PositiveConfig());
  result |= Check(claims.ArmPause(WriterPhase::kBeforeDequeue) &&
                      claims.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "creation claims are tested at the FIFO linearization point");
  const auto malformed =
      claims.SubmitJobCreated(RawCandidateEvent{1, ids.primary_job, "job_created", "{}"});
  const auto valid = claims.SubmitJobCreated(MakeJobCreated(ids.primary_job));
  result |=
      Check(malformed.code == IngressCode::kAdmitted && valid.code == IngressCode::kAdmitted &&
                claims.creation_claim_count(ids.primary_job) == 2,
            "malformed-first/valid-second creation retains one shared provisional reservation");
  result |= Check(claims.Release(malformed.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "FIFO pause releases both same-ID creation turns deterministically");
  result |= ConsumeCompletion(claims, malformed, Completion::Code::kReducerRejection,
                              RejectionReason::kInvalidEventPayload);
  result |= ConsumeCompletion(claims, valid, Completion::Code::kSuccess);
  result |= Check(claims.creation_claim_count(ids.primary_job) == 0 && claims.resident_count() == 1,
                  "failed first creation does not release a claim needed by valid second creation");

  JobOrchestratorHarness valid_first(PositiveConfig());
  result |= Check(valid_first.ArmPause(WriterPhase::kBeforeDequeue) &&
                      valid_first.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "valid-first duplicate creation is paused at FIFO insertion");
  const auto valid_creation = valid_first.SubmitJobCreated(MakeJobCreated(ids.primary_job));
  const auto duplicate_creation = valid_first.SubmitJobCreated(MakeJobCreated(ids.primary_job));
  result |= Check(valid_creation.code == IngressCode::kAdmitted &&
                      duplicate_creation.code == IngressCode::kAdmitted &&
                      valid_first.creation_claim_count(ids.primary_job) == 2,
                  "valid-first duplicate retains two provisional creation claims while queued");
  result |= Check(valid_first.Release(valid_creation.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "valid-first duplicate queue is released deterministically");
  result |= ConsumeCompletion(valid_first, valid_creation, Completion::Code::kSuccess);
  result |= ConsumeCompletion(valid_first, duplicate_creation, Completion::Code::kReducerRejection,
                              RejectionReason::kJobAlreadyExists);
  result |= Check(
      valid_first.creation_claim_count(ids.primary_job) == 0 && valid_first.resident_count() == 1,
      "valid-first duplicate leaves one resident and zero provisional claims");

  JobOrchestratorHarness terminal(PositiveConfig());
  result |= DriveToFinalizing(terminal, ids, allocation);
  result |= ExpectCommitted(terminal,
                            terminal.SubmitTerminalOutcome(
                                MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded)),
                            EventType::kTerminalOutcomeCommitted);
  result |= Check(terminal.HoldCallbackLease(),
                  "terminal callback registration occurs before producer quiescence");
  const auto terminal_marker = terminal.SubmitShutdown();
  result |=
      Check(terminal.WaitUntil(terminal_marker.ingress_sequence, WriterPhase::kShutdownMarker) &&
                !terminal.BeginShutdown(),
            "terminal terminate source remains registered through producer quiescence");
  result |=
      Check(terminal.SubmitTerminate(Terminate(ids.primary_job)).code == IngressCode::kAdmitted,
            "terminal resident admits automatically critical terminate during quiescence");
  result |=
      Check(terminal.CloseCallbackLease(), "terminal shutdown lease releases deterministically");
  return result;
}

int JobIngressCoalescing() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  int result = DriveToRunning(harness, ids, EmptyAllocation());
  result |= Check(harness.ArmPause(WriterPhase::kBeforeDequeue) &&
                      harness.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "Worker retry window is paused before first insertion");
  const auto first = harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  const auto retry_queued =
      harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |= Check(retry_queued.code == IngressCode::kCoalescedPending &&
                      retry_queued.ingress_sequence == first.ingress_sequence,
                  "exact queued Worker retry coalesces to the original sequence");
  result |= Check(harness.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "Worker retry window releases through the keyed barrier");
  const auto turns = harness.writer_turn_count();
  const auto attempts = harness.journal_attempts();
  result |= ConsumeCompletion(harness, first, Completion::Code::kSuccess);
  result |= Check(harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1)).code ==
                      IngressCode::kCoalescedPending,
                  "exact post-turn retry coalesces while ACK obligation remains");
  result |= Check(harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2)).code ==
                          IngressCode::kAlreadyPending &&
                      harness.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 1)).code ==
                          IngressCode::kAlreadyPending,
                  "distinct Worker identity or payload remains with its source");
  result |=
      Check(harness.writer_turn_count() == turns + 1 && harness.journal_attempts() == attempts + 1,
            "coalescing adds no reducer turn or Journal attempt");
  const auto before_ack = harness.journal_attempts();
  result |= Check(harness.RetireWorkerAck(ids.primary_job, 1),
                  "ACK success is a private control retirement");
  result |= Check(harness.journal_attempts() == before_ack,
                  "ACK retirement consumes no ingress or Journal sequence");
  const auto late = harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2));
  result |= ExpectCommitted(harness, late, EventType::kLateWorkerEvent);

  JobOrchestratorHarness duplicate_payload(PositiveConfig());
  result |= DriveToRunning(duplicate_payload, ids, EmptyAllocation());
  result |= Check(duplicate_payload.ArmPause(WriterPhase::kBeforeDequeue) &&
                      duplicate_payload.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "duplicate-key Worker payload window pauses before first insertion");
  auto duplicate = MakeWorkerCompleted(ids.primary_job, ids.worker, 1);
  duplicate.payload_json = "{\"worker_id\":\"" + ids.secondary_worker.value +
                           "\",\"worker_id\":\"" + ids.worker.value + "\",\"event_sequence\":1}";
  const auto duplicate_first = duplicate_payload.SubmitWorker(std::move(duplicate));
  const auto duplicate_retry =
      duplicate_payload.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |= Check(duplicate_first.code == IngressCode::kAdmitted &&
                      duplicate_retry.code == IngressCode::kAlreadyPending,
                  "ambiguous duplicate-key Worker payload never coalesces with a semantic retry");
  result |= Check(
      duplicate_payload.Release(duplicate_first.ingress_sequence, WriterPhase::kBeforeDequeue) &&
          ConsumeCompletion(duplicate_payload, duplicate_first, Completion::Code::kSuccess) == 0,
      "last duplicate Worker value follows reducer semantics without coalescing");
  result |= ExpectCommitted(harness,
                            harness.SubmitProcessExit(MakeProcessExit(
                                ids.primary_job, ids, CompletionMode::kCooperative)),
                            EventType::kProcessExitConfirmed);
  return result;
}

int JobIngressFailClosed() {
  int result = 0;
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  JobOrchestratorHarness precommit(PositiveConfig());
  precommit.InjectPrecommitMaterializationFailure();
  const auto materialization = precommit.Create();
  result |= ConsumeCompletion(precommit, materialization, Completion::Code::kServiceFailed);
  result |= Check(precommit.journal_attempts() == 0 && precommit.NoForbiddenPostcommitActions(),
                  "precommit failure has no Journal, activation, action, or response");
  JobOrchestratorHarness clock_failure(PositiveConfig());
  clock_failure.InjectClockReadFailure();
  const auto clock_turn = clock_failure.Create();
  result |= ConsumeCompletion(clock_failure, clock_turn, Completion::Code::kServiceFailed);
  result |= Check(
      clock_failure.failed() && clock_failure.sealed() && clock_failure.total_occupancy() == 0 &&
          clock_failure.journal_attempts() == 0 && clock_failure.effect_count() == 0,
      "throwing Clock disposes its reserved completion and seals without Journal/effects");
  JobOrchestratorHarness invalid_timer(PositiveConfig());
  const auto invalid_timer_create = invalid_timer.Create();
  result |= ConsumeCompletion(invalid_timer, invalid_timer_create, Completion::Code::kSuccess);
  const auto invalid_timer_result = invalid_timer.SubmitTimeout(
      TimerNotification{Uuid{"not-a-uuid"}, TimeoutPhase::kPreparation, 1});
  result |= Check(!invalid_timer_result.discarded &&
                      invalid_timer_result.admitted.code == IngressCode::kServiceFailed &&
                      invalid_timer.failed() && invalid_timer.sealed(),
                  "invalid timer ingress latches fail-closed outside the ingress lock");
  JobOrchestratorHarness corruption(PositiveConfig());
  corruption.InjectAccountingCorruption();
  result |= Check(corruption.Create().code == IngressCode::kServiceFailed && corruption.failed(),
                  "internal gate/permit corruption latches sticky service_failed");
  JobOrchestratorHarness capacity(PositiveConfig());
  capacity.SetDestinationCapacity(0);
  result |= Check(capacity.Create().code == IngressCode::kServiceFailed &&
                      capacity.journal_attempts() == 0 &&
                      capacity.DestinationCapacityCheckedBeforeCommit(),
                  "destination capacity insufficiency fails before logical commit");
  JobOrchestratorHarness queued(PositiveConfig());
  result |= Check(queued.ArmPause(WriterPhase::kBeforeDequeue) &&
                      queued.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "failure disposal queues multiple owned entries before latching");
  const auto first = queued.Create();
  const auto second = queued.SubmitJobCreated(MakeJobCreated(ids.secondary_job));
  const auto before_disposed = queued.disposed_count();
  const auto before_writer = queued.writer_turn_count();
  const auto before_apply = queued.apply_count();
  const auto before_journal = queued.journal_attempts();
  result |= Check(queued.LatchReadinessFailure() &&
                      queued.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "readiness releases queued failure disposal deterministically");
  result |= ConsumeCompletion(queued, first, Completion::Code::kServiceFailed);
  result |= ConsumeCompletion(queued, second, Completion::Code::kServiceFailed);
  result |=
      Check(queued.disposed_count() == before_disposed + 2 && queued.total_occupancy() == 0 &&
                queued.writer_turn_count() == before_writer &&
                queued.apply_count() == before_apply && queued.journal_attempts() == before_journal,
            "each queued payload is disposed once without writer/apply/Journal work");
  JobOrchestratorHarness padded_identity(PositiveConfig());
  result |= DriveToRunning(padded_identity, ids, allocation);
  result |= Check(padded_identity.ArmPause(WriterPhase::kBeforeDequeue) &&
                      padded_identity.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "padded Worker identity test pauses before first delivery dequeue");
  auto padded_worker = MakeWorkerCompleted(ids.primary_job, ids.worker, 1);
  padded_worker.payload_json.append(300, ' ');
  const auto padded_first = padded_identity.SubmitWorker(std::move(padded_worker));
  const auto padded_retry =
      padded_identity.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |=
      Check(padded_first.code == IngressCode::kAdmitted &&
                padded_retry.code == IngressCode::kCoalescedPending && !padded_identity.failed(),
            "valid Worker identity with arbitrary JSON whitespace coalesces without a raw bound");
  result |=
      Check(padded_identity.Release(padded_first.ingress_sequence, WriterPhase::kBeforeDequeue) &&
                ConsumeCompletion(padded_identity, padded_first, Completion::Code::kSuccess) == 0,
            "padded Worker delivery remains normally admitted and completed");
  return result;
}

int JobIngressShutdownQuiescence() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  int result = 0;
  result |= Check(harness.ArmPause(WriterPhase::kBeforeDequeue) &&
                      harness.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "shutdown window pauses before the pre-marker FIFO entry");
  const auto pre_marker = harness.Create();
  result |= Check(harness.HoldCallbackLease(),
                  "running service admits a finite callback registration before shutdown");
  const auto marker = harness.SubmitShutdown();
  const auto coalesced = harness.SubmitShutdown();
  result |= Check(marker.code == IngressCode::kAdmitted &&
                      coalesced.code == IngressCode::kCoalescedPending &&
                      coalesced.ingress_sequence == marker.ingress_sequence,
                  "shutdown retry attaches to one marker relationship");
  result |= Check(harness.Release(pre_marker.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "pre-marker FIFO is released before shutdown marker processing");
  result |= Check(ConsumeCompletion(harness, pre_marker, Completion::Code::kSuccess) == 0,
                  "normal pre-marker work completes first");
  const auto before_marker = harness.journal_attempts();
  result |= Check(harness.WaitUntil(marker.ingress_sequence, WriterPhase::kShutdownMarker),
                  "writer reaches the non-Journal shutdown marker after normal work");
  result |= Check(harness.journal_attempts() == before_marker,
                  "shutdown marker adds no reducer turn, envelope, or Journal sequence");
  auto post_marker = harness.RetainedCallback();
  result |= Check(!post_marker.HoldLease(),
                  "draining rejects new callback registration after the shutdown marker");
  result |= Check(ConsumeShutdownCompletion(harness, marker) == 0,
                  "shutdown marker control completion is consumed once including coalescing");
  result |= Check(!harness.sealed(), "held finite callback lease prevents sealing");
  result |= Check(harness.CloseCallbackLease() && harness.sealed(),
                  "lease release permits sealing after the processed marker");
  result |= Check(harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2)).code ==
                      IngressCode::kAdmissionClosed,
                  "sealed ingress rejects registered critical delivery with admission_closed");
  result |= CheckTraceMetadata(harness.CopyTrace());
  return result;
}

int JobIngressCallbackLifetime() {
  int result = 0;
  {
    JobOrchestratorHarness clean(PositiveConfig());
    auto first_endpoint = clean.RetainedCallback();
    auto second_endpoint = clean.RetainedCallback();
    result |= Check(first_endpoint.HoldLease() && second_endpoint.HoldLease(),
                    "two independently retained callback leases are finite");
    const auto marker = clean.SubmitShutdown();
    result |= Check(clean.WaitUntil(marker.ingress_sequence, WriterPhase::kShutdownMarker) &&
                        !clean.BeginShutdown() && !clean.sealed(),
                    "processed shutdown remains unsealed while callback leases are held");
    result |= Check(first_endpoint.ReleaseLease() && !clean.sealed(),
                    "releasing only one callback lease cannot seal the writer");
    result |= Check(second_endpoint.ReleaseLease() && clean.BeginShutdown() && clean.sealed(),
                    "sealing waits for every callback lease before writer destruction");
    result |= Check(first_endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code ==
                            IngressCode::kAdmissionClosed &&
                        second_endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code ==
                            IngressCode::kAdmissionClosed,
                    "both sealed callback endpoints cannot reach writer state");
  }
  {
    JobOrchestratorHarness failed(PositiveConfig());
    auto endpoint = failed.RetainedCallback();
    result |= Check(failed.ConcurrentCallbackInvocation() && failed.failed() && failed.sealed(),
                    "concurrent invocation latches and seals after its final invocation reference");
    const auto marker = failed.SubmitShutdown();
    result |= Check(marker.code == IngressCode::kServiceFailed && failed.sealed(),
                    "failed service remains deterministically sealed without a shutdown marker");
    result |= Check(
        endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code == IngressCode::kServiceFailed,
        "late endpoint returns sticky service_failed after sealing");
  }
  {
    JobOrchestratorHarness callback_race(PositiveConfig());
    auto endpoint = callback_race.RetainedCallback();
    result |= Check(callback_race.ArmPause(WriterPhase::kBeforeDequeue) &&
                        callback_race.ArmBarrier(WriterPhase::kBeforeDequeue),
                    "callback admission/final-seal race pauses the admitted callback turn");
    IngressResult callback_admission{};
    std::thread producer(
        [&] { callback_admission = endpoint.Invoke(MakeJobCreated(Ids().primary_job)); });
    producer.join();
    const auto marker = callback_race.SubmitShutdown();
    result |= Check(
        callback_admission.code == IngressCode::kAdmitted && marker.code == IngressCode::kAdmitted,
        "callback admission is published before the shutdown marker");
    result |= Check(
        callback_race.Release(callback_admission.ingress_sequence, WriterPhase::kBeforeDequeue) &&
            ConsumeCompletion(callback_race, callback_admission, Completion::Code::kSuccess) == 0 &&
            callback_race.WaitUntil(marker.ingress_sequence, WriterPhase::kShutdownMarker),
        "final-seal protocol drains callback-admitted work before marker completion");
    result |= Check(callback_race.BeginShutdown() && callback_race.sealed() &&
                        endpoint.Invoke(MakeJobCreated(Ids().secondary_job)).code ==
                            IngressCode::kAdmissionClosed,
                    "final seal closes callback access only after ingress and gates are stable");
  }
  return result;
}

int CheckReadinessCase(WriterPhase phase) {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  int result = 0;
  if (phase == WriterPhase::kAfterDecision || phase == WriterPhase::kShutdownMarker ||
      phase == WriterPhase::kBeforeDequeue || phase == WriterPhase::kAfterDequeueAuthorized)
    result |= Check(harness.ArmPause(phase) && harness.ArmBarrier(phase),
                    "readiness race is armed before the source is submitted");
  if (phase == WriterPhase::kAfterDecision) {
    const auto rejected = harness.SubmitTerminate(Terminate(ids.secondary_job));
    result |= Check(harness.WaitUntil(rejected.ingress_sequence, phase),
                    "rejection reaches decision barrier");
    result |=
        Check(harness.LatchReadinessFailure() && harness.Release(rejected.ingress_sequence, phase),
              "readiness latches during reducer rejection");
    return result | ConsumeCompletion(harness, rejected, Completion::Code::kReducerRejection,
                                      RejectionReason::kJobNotFound);
  }
  if (phase == WriterPhase::kShutdownMarker) {
    const auto completion_before = harness.completion_count();
    const auto marker = harness.SubmitShutdown();
    result |= Check(harness.WaitUntil(marker.ingress_sequence, phase),
                    "shutdown control reaches barrier");
    result |=
        Check(harness.LatchReadinessFailure() && harness.Release(marker.ingress_sequence, phase),
              "readiness latches while shutdown marker is in flight");
    result |= Check(harness.WaitUntil(marker.ingress_sequence, WriterPhase::kTurnFinished),
                    "shutdown control reaches its exact terminal disposition");
    result |= Check(harness.completion_count() == completion_before + 1,
                    "shutdown marker control completion is published before failure disposal");
    const auto completion = harness.TakeCompletion(marker.ingress_sequence);
    result |= Check(completion.has_value() && completion->code == Completion::Code::kSuccess,
                    "in-flight shutdown marker completes its control transition exactly once");
    result |= Check(!harness.TakeCompletion(marker.ingress_sequence).has_value(),
                    "shutdown marker has no duplicate control completion");
    result |= Check(harness.journal_attempts() == 0,
                    "readiness-failed shutdown marker never creates a Journal event");
    return result | Check(harness.failed(), "shutdown control completes before failure handling");
  }
  if (phase == WriterPhase::kAfterDequeueAuthorized) {
    const auto accepted = harness.Create();
    result |= Check(harness.WaitUntil(accepted.ingress_sequence, phase),
                    "entry is authorized before the readiness race");
    result |=
        Check(harness.LatchReadinessFailure() && harness.Release(accepted.ingress_sequence, phase),
              "readiness after authorization is latched without revoking the turn");
    result |= ConsumeCompletion(harness, accepted, Completion::Code::kSuccess);
    return result | Check(harness.journal_attempts() == 1,
                          "authorized turn completes its reducer and Journal disposition");
  }
  if (phase == WriterPhase::kBeforeDequeue) {
    const auto writer_before = harness.writer_turn_count();
    const auto apply_before = harness.apply_count();
    const auto journal_before = harness.journal_attempts();
    const auto queued = harness.Create();
    result |= Check(harness.WaitUntil(queued.ingress_sequence, phase),
                    "queued turn reaches the before-dequeue barrier");
    result |=
        Check(harness.LatchReadinessFailure() && harness.Release(queued.ingress_sequence, phase),
              "readiness closes admission before dequeue");
    result |= ConsumeCompletion(harness, queued, Completion::Code::kServiceFailed);
    result |= Check(harness.writer_turn_count() == writer_before &&
                        harness.apply_count() == apply_before &&
                        harness.journal_attempts() == journal_before,
                    "before-dequeue readiness failure starts no writer/apply/Journal turn");
    return result;
  }
  // Use an effect-bearing resources_committed turn.  Thus every accepted readiness position proves
  // the required committed activation, timer effect/mapping, and response before failure handling.
  const auto created = harness.Create();
  result |= ConsumeCompletion(harness, created, Completion::Code::kSuccess);
  const auto effects_before = harness.effect_count();
  const auto responses_before = harness.response_count();
  result |= Check(harness.ArmPause(phase) && harness.ArmBarrier(phase),
                  "effect-bearing readiness race is armed after setup completes");
  const auto accepted =
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, EmptyAllocation()));
  result |= Check(harness.WaitUntil(accepted.ingress_sequence, phase),
                  "effect-bearing accepted turn reaches requested barrier");
  result |=
      Check(harness.LatchReadinessFailure() && harness.Release(accepted.ingress_sequence, phase),
            "readiness latch is monotonic and explicit");
  result |= ConsumeCompletion(harness, accepted, Completion::Code::kSuccess);
  const auto snapshot = harness.Snapshot(ids.primary_job);
  result |= Check(snapshot.has_value() && snapshot->resource_status == ResourceStatus::kCommitted,
                  "every accepted readiness race installs the committed resources snapshot");
  return result | Check(harness.effect_count() > effects_before &&
                            harness.response_count() > responses_before,
                        "successful committed turn completes effect mapping and response before "
                        "failure handling");
}

int JobIngressReadinessFailure() {
  int result = 0;
  for (const auto phase :
       {WriterPhase::kBeforeDequeue, WriterPhase::kAfterDequeueAuthorized,
        WriterPhase::kAfterDecision, WriterPhase::kBeforeCommit, WriterPhase::kAfterCommit,
        WriterPhase::kAfterApply, WriterPhase::kAfterEffects, WriterPhase::kShutdownMarker})
    result |= CheckReadinessCase(phase);
  return result;
}

int JobJournalEnvelopeVectors() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = DriveToFinalizing(harness, ids, allocation);
  result |= ExpectCommitted(harness,
                            harness.SubmitTerminalOutcome(
                                MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded)),
                            EventType::kTerminalOutcomeCommitted);
  const auto all_expected = ExpectedSuccessfulEnvelopes(ids, allocation);
  result |= CheckExactEnvelopes(
      harness.CopyJournalAttempts(),
      std::vector<ExpectedEnvelope>(all_expected.begin(), all_expected.begin() + 10));
  const auto before = harness.journal_attempts();
  const auto rejected = harness.SubmitCancel(Cancel(ids.primary_job));
  result |= ConsumeCompletion(harness, rejected, Completion::Code::kReducerRejection,
                              RejectionReason::kCommandNotAllowedInState);
  result |=
      Check(harness.journal_attempts() == before, "rejected input creates no envelope attempt");
  return result;
}

int JobLogicalSequenceExhaustionFailClosed() {
  Config config = PositiveConfig();
  config.initial_journal_sequence = std::numeric_limits<std::uint64_t>::max();
  JobOrchestratorHarness harness(config);
  const auto ids = Ids();
  int result = ConsumeCompletion(harness, harness.Create(), Completion::Code::kSuccess);
  const auto exhausted =
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, EmptyAllocation()));
  result |= ConsumeCompletion(harness, exhausted, Completion::Code::kServiceFailed);
  result |=
      Check(harness.journal_attempts() == 1 && harness.failed(),
            "maximum sequence is committed once and next input fails before envelope construction");
  result |= Check(
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, EmptyAllocation()))
              .code == IngressCode::kServiceFailed,
      "burned durable sequence is never retried or reused");
  return result;
}

int JobLogicalCommitOrder() {
  const auto ids = Ids();
  int result = 0;
  {
    JobOrchestratorHarness cooperative(PositiveConfig());
    result |= DriveToRunning(cooperative, ids, EmptyAllocation());
    const auto cancel = cooperative.SubmitCancel(Cancel(ids.primary_job));
    result |= ExpectCommitted(cooperative, cancel, EventType::kCancelAccepted);
    result |= Check(cooperative.RetainTimerLease(ids.primary_job, TimeoutPhase::kCooperativeStop),
                    "cooperative-stop timer lease is retained after cancel arm");
    const auto stop_candidate = cooperative.TakeRunnerCandidate();
    result |= Check(stop_candidate.has_value(), "cooperative stop candidate is explicitly taken");
    result |= CheckExactTurnTrace(cooperative.CopyTrace(), 6, [] {
      ExpectedTrace expected;
      expected.Turn(6, EventType::kCancelAccepted,
                    {{EffectId::kRequestCooperativeStop, "handoff:cooperative-stop"},
                     {EffectId::kArmCooperativeStopTimeout, "timer:arm:cooperative_stop:1"}},
                    {"source:timer:cooperative_stop"});
      return expected;
    }());
    result |= Check(cooperative.VerifyFakes(),
                    "cooperative runner candidate is consumed and fake verifies");
  }
  {
    JobOrchestratorHarness forced(PositiveConfig());
    result |= DriveToRunning(forced, ids, EmptyAllocation());
    const auto cancel = forced.SubmitCancel(Cancel(ids.primary_job));
    result |= ExpectCommitted(forced, cancel, EventType::kCancelAccepted);
    result |= Check(forced.CancelRunnerCandidate(),
                    "cooperative candidate is explicitly cancelled before escalation");
    const auto terminate = forced.SubmitTerminate(Terminate(ids.primary_job));
    result |= ExpectCommitted(forced, terminate, EventType::kTerminateAccepted);
    result |= Check(forced.TakeRunnerCandidate().has_value(),
                    "forced stop candidate is explicitly taken after escalation");
    result |= CheckExactTurnTrace(forced.CopyTrace(), 7, [] {
      ExpectedTrace expected;
      expected.Turn(7, EventType::kTerminateAccepted,
                    {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
                     {EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop:1"},
                     {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
                      "timer:arm:process_exit_confirmation:1"}},
                    {"source:timer:process_exit_confirmation"});
      return expected;
    }());
    result |= Check(forced.VerifyFakes(), "forced runner candidate cancellation is verified");
  }
  {
    JobOrchestratorHarness timeout(PositiveConfig());
    result |= DriveToFinalizing(timeout, ids, EmptyAllocation());
    result |= ExpectCommitted(timeout,
                              timeout.SubmitTerminalOutcome(MakeTerminalOutcome(
                                  ids.primary_job, TerminalOutcome::kSucceeded)),
                              EventType::kTerminalOutcomeCommitted);
    result |=
        Check(timeout.RetainTimerLease(ids.primary_job, TimeoutPhase::kProcessExitConfirmation),
              "process-exit-confirmation timer permit is retained before source delivery");
    const auto notification = timeout.SubmitTimeout(
        TimerNotification{ids.primary_job, TimeoutPhase::kProcessExitConfirmation, 1});
    result |= Check(!notification.discarded, "armed timer notification emits a typed candidate");
    result |= ExpectCommitted(timeout, notification.admitted, EventType::kTimeoutExpired);
    result |= Check(timeout.TakeRunnerCandidate().has_value(),
                    "process-exit timeout forced-stop candidate is explicitly taken");
    result |= CheckExactTurnTrace(timeout.CopyTrace(), 11, [] {
      ExpectedTrace expected;
      expected.Turn(11, EventType::kTimeoutExpired,
                    {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
                     {EffectId::kQuarantineResources, "safety:quarantine"},
                     {EffectId::kSetReadinessFalse, "safety:set_readiness_false"}});
      return expected;
    }());
    result |= Check(!timeout.failed(),
                    "set_readiness_false remains a safety observation before explicit latch");
    result |= Check(timeout.LatchReadinessFailure() && timeout.failed(),
                    "service failure follows only the later explicit readiness latch");
    result |= Check(timeout.VerifyFakes(), "process-exit timeout fake observations verify");
  }
  return result;
}

int JobLogicalCommitFailureFailClosed() {
  int result = 0;
  const auto ids = Ids();
  for (const auto outcome :
       {LogicalCommitResult::kDefiniteFailure, LogicalCommitResult::kOutcomeUnknown}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToFinalizing(harness, ids, EmptyAllocation());
    const auto attempts = harness.journal_attempts();
    const auto trace_size = harness.CopyTrace().size();
    harness.SetNextCommitResult(outcome);
    const auto terminal = harness.SubmitTerminalOutcome(
        MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded));
    result |= ConsumeCompletion(harness, terminal, Completion::Code::kServiceFailed);
    const auto trace = harness.CopyTrace();
    result |= Check(harness.journal_attempts() == attempts + 1 && trace.size() == trace_size + 1 &&
                        trace.back().kind == TraceKind::kJournalAttempt && harness.failed(),
                    "definite and unknown result burn one sequence with no activation or action");
    result |= Check(harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)).code ==
                        IngressCode::kServiceFailed,
                    "failed commit is not retried and later admission remains sticky failure");
  }
  return result;
}

int SuccessfulLifecycleSlice() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = DriveToFinalizing(harness, ids, allocation);
  result |= ExpectCommitted(harness,
                            harness.SubmitTerminalOutcome(
                                MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded)),
                            EventType::kTerminalOutcomeCommitted);
  result |= Check(harness.RetireWorkerAck(ids.primary_job, 1),
                  "ACK success retires Worker identity without an ingress sequence");
  result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kProcessExitConfirmation),
                  "process-exit-confirmation timer lease is retained before process exit");
  result |= ExpectCommitted(
      harness, harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2)),
      EventType::kLateWorkerEvent);
  result |= ExpectCommitted(harness,
                            harness.SubmitProcessExit(MakeProcessExit(
                                ids.primary_job, ids, CompletionMode::kCooperative)),
                            EventType::kProcessExitConfirmed);
  result |= ExpectCommitted(
      harness, harness.SubmitResourcesReleased(MakeResourcesReleased(ids.primary_job, allocation)),
      EventType::kResourcesReleased);
  result |= ExpectCommitted(harness, harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)),
                            EventType::kCleanupStatusRecorded);
  result |= CheckExactEnvelopes(harness.CopyJournalAttempts(),
                                ExpectedSuccessfulEnvelopes(ids, allocation));
  result |= CheckTraceMetadata(harness.CopyTrace());
  result |= Check(
      SuccessfulTrace(ids) == harness.CopyTrace(),
      "successful lifecycle has the complete exact global Journal/effect/mapping/response trace");
  result |= Check(harness.NoPostcommitAllocationOrCopy(),
                  "successful lifecycle postcommit performs no allocation, copy, or destruction");
  const auto snapshot = harness.Snapshot(ids.primary_job);
  result |= Check(snapshot && snapshot->state == JobState::kSucceeded &&
                      snapshot->session_retention_status == RetentionStatus::kRetained &&
                      snapshot->finalization_status == FinalizationStatus::kCompleted &&
                      snapshot->cleanup_status == CleanupStatus::kCompleted &&
                      snapshot->completion_mode == CompletionMode::kCooperative &&
                      snapshot->process_exit_confirmed && !snapshot->pending_worker_event_ack &&
                      snapshot->resource_status == ResourceStatus::kReleased,
                  "successful lifecycle asserts all final snapshot axes");
  const auto launches = harness.CopyLaunchRequests();
  const auto sessions = harness.CopySessionRequests();
  result |=
      Check(launches.size() == 1 && launches[0].job_id == ids.primary_job &&
                launches[0].intent.operation_id == ids.launch_operation &&
                launches[0].intent.worker_id == ids.worker && sessions.size() == 1 &&
                sessions[0].job_id == ids.primary_job && sessions[0].session_id == ids.primary_job,
            "generated identities feed exact owned runner and Session requests");
  result |= Check(
      !harness.TakeRunnerCandidate() && !harness.TakeSessionCandidate() && harness.VerifyFakes(),
      "all inert fake candidates are consumed and no pending candidate remains");
  return result;
}

using Selector = int (*)();
struct SelectorEntry {
  std::string_view name;
  Selector function;
};
constexpr std::array<SelectorEntry, 14> kSelectors{{
    {"job_ingress_linearization_order", JobIngressLinearizationOrder},
    {"job_ingress_single_writer", JobIngressSingleWriter},
    {"job_ingress_source_classification", JobIngressSourceClassification},
    {"job_ingress_capacity_and_reserve", JobIngressCapacityAndReserve},
    {"job_ingress_coalescing", JobIngressCoalescing},
    {"job_ingress_fail_closed", JobIngressFailClosed},
    {"job_ingress_shutdown_quiescence", JobIngressShutdownQuiescence},
    {"job_ingress_callback_lifetime", JobIngressCallbackLifetime},
    {"job_ingress_readiness_failure", JobIngressReadinessFailure},
    {"job_journal_envelope_vectors", JobJournalEnvelopeVectors},
    {"job_logical_sequence_exhaustion_fail_closed", JobLogicalSequenceExhaustionFailClosed},
    {"job_logical_commit_order", JobLogicalCommitOrder},
    {"job_logical_commit_failure_fail_closed", JobLogicalCommitFailureFailClosed},
    {"job_successful_lifecycle_slice", SuccessfulLifecycleSlice},
}};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sitometron_job_orchestration_tests <selector>\n";
    return 2;
  }
  for (const auto& selector : kSelectors) {
    if (selector.name == argv[1]) return selector.function();
  }
  std::cerr << "unknown selector: " << argv[1] << '\n';
  return 2;
}
