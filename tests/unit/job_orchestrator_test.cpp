#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <exception>
#include <initializer_list>
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

bool SnapshotEqual(const Snapshot& left, const Snapshot& right) {
  return left.schema_version == right.schema_version && left.job_id == right.job_id &&
         left.session_id == right.session_id && left.entity_exists == right.entity_exists &&
         left.state == right.state && left.latched_reason == right.latched_reason &&
         left.completion_candidate == right.completion_candidate &&
         left.completion_mode == right.completion_mode &&
         left.resource_status == right.resource_status &&
         left.allocation_id == right.allocation_id &&
         left.allocation_digest == right.allocation_digest &&
         left.worker_launch_status == right.worker_launch_status &&
         left.launch_operation_id == right.launch_operation_id &&
         left.worker_id == right.worker_id && left.process_presence == right.process_presence &&
         left.process_exit_confirmed == right.process_exit_confirmed &&
         left.session_retention_status == right.session_retention_status &&
         left.finalization_status == right.finalization_status &&
         left.cleanup_status == right.cleanup_status &&
         left.pending_worker_event_ack == right.pending_worker_event_ack &&
         left.pending_worker_id == right.pending_worker_id &&
         left.pending_worker_event_sequence == right.pending_worker_event_sequence;
}

struct MutationEpoch {
  std::optional<Snapshot> snapshot;
  std::size_t journal_attempts;
  std::size_t applies;
  std::size_t effects;
  std::size_t responses;
  std::size_t acknowledgements;
  std::size_t writer_turns;
  std::size_t traces;
  std::vector<std::uint64_t> ingress_sequences;
};

MutationEpoch CaptureEpoch(const JobOrchestratorHarness& harness, const Uuid& job) {
  return {
      harness.Snapshot(job),       harness.journal_attempts(), harness.apply_count(),
      harness.effect_count(),      harness.response_count(),   harness.ack_authorization_count(),
      harness.writer_turn_count(), harness.CopyTrace().size(), harness.CopyIngressSequences()};
}

bool SealedWithNoLiveIngress(const JobOrchestratorHarness& harness) {
  // The public critical observable is fixed resident inventory, not the private live-permit
  // counter. A successful sealed transition proves the live counter reached zero.
  const auto inventory = harness.resident_count() * 9U;
  return harness.sealed() && harness.normal_occupancy() == 0 &&
         harness.live_critical_permit_count() == 0 && harness.critical_occupancy() == inventory &&
         harness.total_occupancy() == inventory;
}

bool EpochUnchanged(const JobOrchestratorHarness& harness, const Uuid& job,
                    const MutationEpoch& before) {
  const auto after = CaptureEpoch(harness, job);
  return before.snapshot.has_value() == after.snapshot.has_value() &&
         (!before.snapshot || SnapshotEqual(*before.snapshot, *after.snapshot)) &&
         before.journal_attempts == after.journal_attempts && before.applies == after.applies &&
         before.effects == after.effects && before.responses == after.responses &&
         before.acknowledgements == after.acknowledgements &&
         before.writer_turns == after.writer_turns && before.traces == after.traces &&
         before.ingress_sequences == after.ingress_sequences;
}

bool RejectionEpochUnchanged(const JobOrchestratorHarness& harness, const Uuid& job,
                             const MutationEpoch& before, std::uint64_t ingress_sequence) {
  const auto after = CaptureEpoch(harness, job);
  return before.snapshot.has_value() == after.snapshot.has_value() &&
         (!before.snapshot || SnapshotEqual(*before.snapshot, *after.snapshot)) &&
         before.journal_attempts == after.journal_attempts && before.applies == after.applies &&
         before.effects == after.effects && before.responses == after.responses &&
         before.acknowledgements == after.acknowledgements &&
         before.writer_turns + 1 == after.writer_turns && before.traces == after.traces &&
         after.ingress_sequences.size() == before.ingress_sequences.size() + 1 &&
         std::equal(before.ingress_sequences.begin(), before.ingress_sequences.end(),
                    after.ingress_sequences.begin()) &&
         after.ingress_sequences.back() == ingress_sequence;
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

struct OrderedPair {
  IngressResult first;
  IngressResult second;
  MutationEpoch before;
  MutationEpoch paused;
  bool armed = false;
  bool arrived = false;
  bool ordered = false;
  bool released = false;
};

template <typename First, typename Second>
OrderedPair QueueOrderedPair(JobOrchestratorHarness& harness, First first, Second second) {
  OrderedPair pair;
  pair.before = CaptureEpoch(harness, Ids().primary_job);
  pair.armed = harness.ArmPause(WriterPhase::kBeforeDequeue) &&
               harness.ArmBarrier(WriterPhase::kBeforeDequeue);
  if (!pair.armed) return pair;
  pair.first = first();
  pair.arrived = pair.first.code == IngressCode::kAdmitted && pair.first.ingress_sequence != 0 &&
                 harness.WaitUntil(pair.first.ingress_sequence, WriterPhase::kBeforeDequeue);
  if (!pair.arrived) return pair;
  pair.second = second();
  pair.ordered = pair.second.code == IngressCode::kAdmitted &&
                 pair.second.ingress_sequence == pair.first.ingress_sequence + 1;
  pair.paused = CaptureEpoch(harness, Ids().primary_job);
  pair.released =
      pair.ordered && harness.Release(pair.first.ingress_sequence, WriterPhase::kBeforeDequeue);
  return pair;
}

int CheckOrderedPair(const OrderedPair& pair, std::string_view message) {
  return Check(
      pair.armed && pair.arrived && pair.ordered && pair.released &&
          pair.paused.writer_turns == pair.before.writer_turns &&
          pair.paused.journal_attempts == pair.before.journal_attempts &&
          pair.paused.applies == pair.before.applies &&
          pair.paused.effects == pair.before.effects &&
          pair.paused.acknowledgements == pair.before.acknowledgements &&
          pair.paused.traces == pair.before.traces &&
          pair.before.snapshot.has_value() == pair.paused.snapshot.has_value() &&
          (!pair.before.snapshot || SnapshotEqual(*pair.before.snapshot, *pair.paused.snapshot)) &&
          pair.paused.ingress_sequences.size() == pair.before.ingress_sequences.size() + 2 &&
          pair.paused.ingress_sequences[pair.paused.ingress_sequences.size() - 2] ==
              pair.first.ingress_sequence &&
          pair.paused.ingress_sequences.back() == pair.second.ingress_sequence,
      message);
}

struct OrderedTurnExpectation {
  EventType event = EventType::kInvalid;
  EventPayload payload{EmptyPayload{}};
  std::vector<std::string> registrations;
  std::vector<std::pair<EffectId, std::string>> effects;
  std::size_t ack_authorizations = 0;
  bool pending_ack = false;
};

std::optional<Snapshot> ExpectedSnapshotAfter(const Snapshot& before,
                                              const OrderedTurnExpectation& expected) {
  const auto applied =
      Apply(before, PreEnvelopeProposal{1, Ids().primary_job, expected.event, expected.payload});
  if (applied.rejection) return std::nullopt;
  return applied.snapshot;
}

bool AckAxesMatch(const JobOrchestratorHarness& harness, const OrderedTurnExpectation& expected) {
  const auto snapshot = harness.Snapshot(Ids().primary_job);
  if (!snapshot || harness.ack_authorization_count() != expected.ack_authorizations ||
      snapshot->pending_worker_event_ack != expected.pending_ack)
    return false;
  return expected.pending_ack
             ? snapshot->pending_worker_id == Ids().worker &&
                   snapshot->pending_worker_event_sequence == 1
             : !snapshot->pending_worker_id && !snapshot->pending_worker_event_sequence;
}

int CheckExactJournalPosition(const std::vector<LogicalJobEvent>& journal, std::size_t index,
                              const OrderedTurnExpectation& expected) {
  if (index >= journal.size())
    return Check(false, "ordered turn has its complete logical Journal envelope");
  const auto sequence = PositiveConfig().initial_journal_sequence + index;
  const auto& actual = journal[index];
  return Check(actual.schema_version == 1 && actual.sequence == sequence &&
                   actual.event_type == expected.event &&
                   actual.recorded_at.rfc3339 == kTimestamp && actual.job_id == Ids().primary_job &&
                   PayloadEqual(actual.payload, expected.payload),
               "ordered turn has its complete logical Journal envelope");
}

int CheckPairTraceDelta(const JobOrchestratorHarness& harness, const OrderedPair& pair,
                        std::initializer_list<const OrderedTurnExpectation*> turns) {
  const auto journal = harness.CopyJournalAttempts();
  if (journal.size() < pair.paused.journal_attempts + turns.size())
    return Check(false, "ordered pair has a complete exact trace delta");
  ExpectedTrace expected;
  std::size_t offset = 0;
  for (const auto* turn : turns) {
    const auto sequence =
        PositiveConfig().initial_journal_sequence + pair.paused.journal_attempts + offset;
    expected.Turn(sequence, turn->event, turn->effects, turn->registrations);
    ++offset;
  }
  for (std::size_t index = 0; index != expected.records.size(); ++index)
    expected.records[index].ordinal = static_cast<std::uint64_t>(pair.paused.traces + index + 1);
  const auto trace = harness.CopyTrace();
  std::vector<TraceRecord> actual;
  if (trace.size() >= pair.paused.traces)
    actual.assign(trace.begin() + static_cast<std::ptrdiff_t>(pair.paused.traces), trace.end());
  return Check(actual == expected.records, "ordered pair has a complete exact trace delta");
}

int ConsumeOrderedRejection(JobOrchestratorHarness& harness, const OrderedPair& pair, bool first,
                            RejectionReason reason, const OrderedTurnExpectation& accepted) {
  const auto& admitted = first ? pair.first : pair.second;
  int result = 0;
  result |= Check(
      WaitForTerminal(harness, admitted.ingress_sequence, Completion::Code::kReducerRejection),
      "queued rejection reaches terminal barrier");
  const auto completion = harness.TakeCompletion(admitted.ingress_sequence);
  result |= Check(completion && completion->code == Completion::Code::kReducerRejection &&
                      completion->rejection && completion->rejection->reason == reason,
                  "queued rejection has the expected disposition");
  result |= Check(!harness.TakeCompletion(admitted.ingress_sequence).has_value(),
                  "queued rejection publishes its completion exactly once");
  const auto after = CaptureEpoch(harness, Ids().primary_job);
  const auto expected_snapshot =
      pair.paused.snapshot ? ExpectedSnapshotAfter(*pair.paused.snapshot, accepted) : std::nullopt;
  const bool exact = after.journal_attempts == pair.paused.journal_attempts + 1 &&
                     after.applies == pair.paused.applies + 1 &&
                     after.effects == pair.paused.effects + accepted.effects.size() &&
                     after.responses == pair.paused.responses + 1 &&
                     after.acknowledgements == accepted.ack_authorizations &&
                     after.writer_turns == pair.paused.writer_turns + 2 &&
                     after.ingress_sequences == pair.paused.ingress_sequences &&
                     expected_snapshot && after.snapshot &&
                     SnapshotEqual(*expected_snapshot, *after.snapshot) &&
                     AckAxesMatch(harness, accepted);
  result |=
      Check(exact, "queued rejection adds no Journal/apply/effect/ACK beyond its FIFO predecessor");
  result |= CheckPairTraceDelta(harness, pair, {&accepted});
  return result;
}

int CheckAckAxes(const JobOrchestratorHarness& harness, const OrderedTurnExpectation& expected,
                 std::string_view message) {
  return Check(AckAxesMatch(harness, expected), message);
}

int ConsumeOrderedCommittedExact(JobOrchestratorHarness& harness, const OrderedPair& pair,
                                 bool first, const OrderedTurnExpectation& expected,
                                 std::string_view message) {
  const auto& admitted = first ? pair.first : pair.second;
  const auto index = pair.paused.journal_attempts + (first ? 0U : 1U);
  int result = ConsumeCompletion(harness, admitted, Completion::Code::kSuccess);
  const auto journal = harness.CopyJournalAttempts();
  result |= CheckExactJournalPosition(journal, index, expected);
  (void)message;
  return result;
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

int DriveToFinalizationPending(JobOrchestratorHarness& harness, const IdFixtures& ids,
                               const AllocationFixture& allocation,
                               std::uint64_t worker_sequence = 1) {
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
  return result;
}

int DriveToFinalizing(JobOrchestratorHarness& harness, const IdFixtures& ids,
                      const AllocationFixture& allocation, std::uint64_t worker_sequence = 1) {
  int result = DriveToFinalizationPending(harness, ids, allocation, worker_sequence);
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

int OrderedRaceQualification();

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
  const auto worker_attempts_before = worker.admission_attempt_count();
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
  result |= Check(worker.WaitForAdmissionAttempts(worker_attempts_before + 2),
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

  JobOrchestratorHarness coalesced_window(PositiveConfig());
  result |= DriveToRunning(coalesced_window, ids, allocation);
  result |= Check(coalesced_window.ArmPause(WriterPhase::kBeforeDequeue) &&
                      coalesced_window.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "coalesced-window regression pauses the first Worker delivery");
  const auto pending_worker =
      coalesced_window.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |= Check(pending_worker.code == IngressCode::kAdmitted,
                  "coalesced-window regression retains the first Worker delivery");
  result |=
      Check(coalesced_window.ArmAdmissionPause(), "coalesced-window admission pause is armed");
  const auto coalesced_attempts_before = coalesced_window.admission_attempt_count();
  std::optional<IngressResult> coalesced_retry;
  std::optional<TimerSubmitResult> competing_timer;
  std::thread retry_again([&] {
    coalesced_retry =
        coalesced_window.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  });
  result |= Check(coalesced_window.WaitForAdmissionPause(),
                  "exact Worker retry owns the paused insertion window");
  std::thread competing([&] {
    competing_timer = coalesced_window.SubmitTimeout(
        TimerNotification{ids.primary_job, TimeoutPhase::kExecution, 1});
  });
  result |= Check(coalesced_window.WaitForAdmissionAttempts(coalesced_attempts_before + 2),
                  "second critical producer reaches admission while retry is paused");
  result |= Check(coalesced_window.ReleaseAdmissionPause(),
                  "coalesced retry insertion window is released");
  retry_again.join();
  competing.join();
  const auto competing_timer_admitted = competing_timer && !competing_timer->discarded &&
                                        competing_timer->admitted.code == IngressCode::kAdmitted;
  result |= Check(coalesced_retry && coalesced_retry->code == IngressCode::kCoalescedPending &&
                      competing_timer_admitted,
                  "coalesced retry clears its insertion window for the next critical producer");
  result |=
      Check(coalesced_window.Release(pending_worker.ingress_sequence, WriterPhase::kBeforeDequeue),
            "coalesced-window queued Worker is released");
  result |= ConsumeCompletion(coalesced_window, pending_worker, Completion::Code::kSuccess);
  if (competing_timer_admitted)
    result |=
        ConsumeCompletion(coalesced_window, competing_timer->admitted, Completion::Code::kSuccess);
  const auto coalesced_marker = coalesced_window.SubmitShutdown();
  result |= Check(coalesced_marker.code == IngressCode::kAdmitted &&
                      coalesced_window.WaitUntil(coalesced_marker.ingress_sequence,
                                                 WriterPhase::kShutdownMarker) &&
                      coalesced_window.BeginShutdown() && coalesced_window.sealed(),
                  "coalesced-window shutdown remains bounded after both producers finish");

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
  result |= OrderedRaceQualification();
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

int MonotonicTerminalCleanupQualification() {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = DriveToFinalizing(harness, ids, allocation);
  result |= ExpectCommitted(harness,
                            harness.SubmitTerminalOutcome(
                                MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded)),
                            EventType::kTerminalOutcomeCommitted);
  result |= ExpectCommitted(harness,
                            harness.SubmitProcessExit(MakeProcessExit(
                                ids.primary_job, ids, CompletionMode::kCooperative)),
                            EventType::kProcessExitConfirmed);

  const auto matching_audit = [&](auto submit, EventType event, std::string_view message) {
    const auto before = CaptureEpoch(harness, ids.primary_job);
    const auto live_permits_before = harness.live_critical_permit_count();
    const auto admitted = submit();
    int checked = ExpectCommitted(harness, admitted, event);
    const auto after = CaptureEpoch(harness, ids.primary_job);
    checked |= Check(before.snapshot && after.snapshot &&
                         SnapshotEqual(*before.snapshot, *after.snapshot) &&
                         after.journal_attempts == before.journal_attempts + 1 &&
                         after.applies == before.applies + 1 && after.effects == before.effects &&
                         after.responses == before.responses + 1 &&
                         after.acknowledgements == before.acknowledgements &&
                         after.writer_turns == before.writer_turns + 1 &&
                         harness.live_critical_permit_count() == live_permits_before,
                     message);
    return checked;
  };
  result |= matching_audit(
      [&] {
        return harness.SubmitProcessExit(
            MakeProcessExit(ids.primary_job, ids, CompletionMode::kCooperative));
      },
      EventType::kProcessExitConfirmed,
      "matching process-exit audit reopens its retired gate without snapshot mutation");
  auto mismatching_ids = ids;
  mismatching_ids.launch_operation = ids.secondary_launch_operation;
  const auto before_bad_exit = CaptureEpoch(harness, ids.primary_job);
  const auto permits_before_bad_exit = harness.live_critical_permit_count();
  const auto bad_exit = harness.SubmitProcessExit(
      MakeProcessExit(ids.primary_job, mismatching_ids, CompletionMode::kCooperative));
  result |= ConsumeCompletion(harness, bad_exit, Completion::Code::kReducerRejection,
                              RejectionReason::kInvariantViolation);
  result |=
      Check(RejectionEpochUnchanged(harness, ids.primary_job, before_bad_exit,
                                    bad_exit.ingress_sequence) &&
                harness.live_critical_permit_count() == permits_before_bad_exit,
            "mismatching process-exit identity releases its turn without mutation or permit leak");
  result |= matching_audit(
      [&] {
        return harness.SubmitProcessExit(
            MakeProcessExit(ids.primary_job, ids, CompletionMode::kCooperative));
      },
      EventType::kProcessExitConfirmed,
      "matching process exit is admitted after the rejected gate turn and retires exactly once");

  result |= ExpectCommitted(
      harness, harness.SubmitResourcesReleased(MakeResourcesReleased(ids.primary_job, allocation)),
      EventType::kResourcesReleased);
  result |= matching_audit(
      [&] {
        return harness.SubmitResourcesReleased(MakeResourcesReleased(ids.primary_job, allocation));
      },
      EventType::kResourcesReleased,
      "matching resource-release audit reopens its retired gate without snapshot mutation");
  auto mismatching_allocation = allocation;
  mismatching_allocation.id = StableId{"allocation-2"};
  const auto before_bad_release = CaptureEpoch(harness, ids.primary_job);
  const auto permits_before_bad_release = harness.live_critical_permit_count();
  const auto bad_release = harness.SubmitResourcesReleased(
      MakeResourcesReleased(ids.primary_job, mismatching_allocation));
  result |= ConsumeCompletion(harness, bad_release, Completion::Code::kReducerRejection,
                              RejectionReason::kInvariantViolation);
  result |=
      Check(RejectionEpochUnchanged(harness, ids.primary_job, before_bad_release,
                                    bad_release.ingress_sequence) &&
                harness.live_critical_permit_count() == permits_before_bad_release,
            "mismatching resource identity releases its turn without mutation or permit leak");
  result |= matching_audit(
      [&] {
        return harness.SubmitResourcesReleased(MakeResourcesReleased(ids.primary_job, allocation));
      },
      EventType::kResourcesReleased,
      "matching resource release is admitted after the rejected gate turn and retires once");

  result |= ExpectCommitted(harness, harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)),
                            EventType::kCleanupStatusRecorded);
  result |=
      matching_audit([&] { return harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)); },
                     EventType::kCleanupStatusRecorded,
                     "matching cleanup audit reopens its retired gate without snapshot mutation");
  const auto before_regression = CaptureEpoch(harness, ids.primary_job);
  const auto permits_before_regression = harness.live_critical_permit_count();
  const auto cleanup_regression = harness.SubmitCleanup(RawCandidateEvent{
      1, ids.primary_job, "cleanup_status_recorded", "{\"status\":\"incomplete\"}"});
  result |= ConsumeCompletion(harness, cleanup_regression, Completion::Code::kReducerRejection,
                              RejectionReason::kInvariantViolation);
  result |= Check(
      RejectionEpochUnchanged(harness, ids.primary_job, before_regression,
                              cleanup_regression.ingress_sequence) &&
          harness.live_critical_permit_count() == permits_before_regression,
      "cleanup regression releases its turn without Journal, effect, mutation, or permit leak");
  result |=
      matching_audit([&] { return harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)); },
                     EventType::kCleanupStatusRecorded,
                     "matching cleanup is admitted after the rejected turn and retires once");
  return result;
}

int JobIngressSourceClassification() {
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  int result = MonotonicTerminalCleanupQualification();
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
    JobOrchestratorHarness malformed_resident(PositiveConfig());
    result |= DriveToFinalizing(malformed_resident, ids, allocation);
    result |= ExpectCommitted(malformed_resident,
                              malformed_resident.SubmitTerminalOutcome(MakeTerminalOutcome(
                                  ids.primary_job, TerminalOutcome::kSucceeded)),
                              EventType::kTerminalOutcomeCommitted);
    const auto before = CaptureEpoch(malformed_resident, ids.primary_job);
    const auto malformed_command =
        malformed_resident.SubmitTerminate(Command{0, CommandType::kInvalid, ids.primary_job, ""});
    result |= ConsumeCompletion(malformed_resident, malformed_command,
                                Completion::Code::kReducerRejection,
                                RejectionReason::kInvalidEventPayload);
    result |= Check(RejectionEpochUnchanged(malformed_resident, ids.primary_job, before,
                                            malformed_command.ingress_sequence),
                    "malformed terminal command wins over state rejection without mutation");
  }
  {
    JobOrchestratorHarness malformed_worker(PositiveConfig());
    result |= ExpectCommitted(malformed_worker, malformed_worker.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(malformed_worker,
                              malformed_worker.SubmitResourcesCommitted(
                                  MakeResourcesCommitted(ids.primary_job, allocation)),
                              EventType::kResourcesCommitted);
    result |=
        ExpectCommitted(malformed_worker, malformed_worker.SubmitGeneratedLaunchIntent(allocation),
                        EventType::kWorkerLaunchIntent);
    const auto before = CaptureEpoch(malformed_worker, ids.primary_job);
    const auto malformed = malformed_worker.SubmitWorker(
        RawCandidateEvent{1, ids.primary_job, "worker_completed", "{}"});
    result |= ConsumeCompletion(malformed_worker, malformed, Completion::Code::kReducerRejection,
                                RejectionReason::kInvalidEventPayload);
    result |= Check(RejectionEpochUnchanged(malformed_worker, ids.primary_job, before,
                                            malformed.ingress_sequence),
                    "malformed preparing Worker wins over state rejection without mutation");
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
    const auto before = CaptureEpoch(timer, ids.primary_job);
    const auto stale =
        timer.SubmitTimeout(TimerNotification{ids.primary_job, TimeoutPhase::kPreparation, 1});
    const auto mismatched =
        timer.SubmitTimeout(TimerNotification{ids.primary_job, TimeoutPhase::kExecution, 1});
    const auto raw_timeout =
        timer.SubmitLaunchObserved(MakeTimeout(ids.primary_job, TimeoutPhase::kPreparation, 1));
    result |= Check(
        stale.discarded && mismatched.discarded &&
            raw_timeout.code == IngressCode::kAdmissionClosed &&
            raw_timeout.ingress_sequence == 0 && EpochUnchanged(timer, ids.primary_job, before),
        "stale, disarmed, and raw timeout notifications discard before ingress mutation");
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
    result |= Check(launches.size() == 2 && launches[0].job_id == ids.primary_job &&
                        launches[0].intent.worker_id == ids.worker &&
                        launches[0].intent.operation_id == ids.launch_operation &&
                        launches[1].job_id == ids.secondary_job &&
                        launches[1].intent.worker_id == ids.secondary_worker &&
                        launches[1].intent.operation_id == ids.secondary_launch_operation,
                    "interleaved resident launches retain their own generated Worker and "
                    "operation identities");
  }
  {
    JobOrchestratorHarness conflict(PositiveConfig());
    result |= ExpectCommitted(conflict, conflict.Create(), EventType::kJobCreated);
    result |= ExpectCommitted(
        conflict,
        conflict.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
        EventType::kResourcesCommitted);
    const auto before = CaptureEpoch(conflict, ids.primary_job);
    const auto turn = conflict.SubmitConflictingResources(ids.primary_job);
    result |= ConsumeCompletion(conflict, turn, Completion::Code::kReducerRejection,
                                RejectionReason::kInvariantViolation);
    result |=
        Check(RejectionEpochUnchanged(conflict, ids.primary_job, before, turn.ingress_sequence) &&
                  !conflict.failed(),
              "well-formed binding conflict changes only its admitted ingress history");
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
  // Completion ownership is reserved at FIFO insertion, independent of when callers take prior
  // dispositions. The explicitly provisioned completion backlog retains T-1 results while two
  // later turns reserve their own slots.
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
      "provisioned completion backlog keeps two later admissions independently reserved");
  JobOrchestratorHarness population(PositiveConfig());
  result |= DriveToRetainedCriticalPopulation(population, ids, allocation, ids.primary_job);
  const auto primary_generated = population.generated_identities();
  result |= Check(primary_generated.has_value() && population.critical_occupancy() == 9,
                  "first resident preacquires all nine bounded critical source permits");
  result |= DriveToRetainedCriticalPopulation(population, ids, allocation, ids.secondary_job);
  const auto secondary_generated = population.generated_identities();
  result |= Check(secondary_generated.has_value() && population.critical_occupancy() == 18,
                  "two residents preacquire eighteen critical source permits");
  if (!primary_generated || !secondary_generated) return result;
  IdFixtures primary = ids;
  primary.worker = primary_generated->worker_id;
  primary.launch_operation = primary_generated->launch_operation_id;
  IdFixtures secondary = ids;
  secondary.primary_job = ids.secondary_job;
  secondary.worker = secondary_generated->worker_id;
  secondary.launch_operation = secondary_generated->launch_operation_id;
  const bool primary_retired =
      population.RetireWorkerAck(MakeWorkerCompleted(primary.primary_job, primary.worker, 1));
  const bool secondary_retired = population.RetireWorkerAck(secondary.primary_job, 1);
  result |=
      Check(primary_retired && secondary_retired && population.live_critical_permit_count() == 16,
            "authorized terminal ACKs free both Worker permits before pressure epoch");
  result |= Check(population.ArmPause(WriterPhase::kBeforeDequeue) &&
                      population.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "capacity population pauses before simultaneous admission");
  struct ExpectedCapacityTurn {
    IngressResult ingress;
    Completion::Code completion;
    RejectionReason rejection = RejectionReason::kInvalid;
  };
  std::vector<ExpectedCapacityTurn> turns;
  const auto add_rejection = [&](IngressResult ingress, RejectionReason rejection) {
    turns.push_back({ingress, Completion::Code::kReducerRejection, rejection});
  };
  const auto add_success = [&](IngressResult ingress) {
    turns.push_back({ingress, Completion::Code::kSuccess});
  };
  const auto normal_one = population.SubmitCancel(Cancel(ids.primary_job));
  const auto normal_two = population.SubmitCancel(Cancel(ids.secondary_job));
  add_rejection(normal_one, RejectionReason::kCommandNotAllowedInState);
  add_rejection(normal_two, RejectionReason::kCommandNotAllowedInState);
  result |=
      Check(normal_one.code == IngressCode::kAdmitted && normal_two.code == IngressCode::kAdmitted,
            "two normal terminal inputs occupy N while paused");
  const auto add_resident_critical = [&](const IdFixtures& resident) {
    const auto terminate = population.SubmitTerminate(Terminate(resident.primary_job));
    add_rejection(terminate, RejectionReason::kCommandNotAllowedInState);
    for (const auto phase :
         {TimeoutPhase::kPreparation, TimeoutPhase::kExecution, TimeoutPhase::kCooperativeStop,
          TimeoutPhase::kProcessExitConfirmation}) {
      const auto submitted = population.SubmitTimeout({resident.primary_job, phase, 1});
      result |= Check(!submitted.discarded && submitted.admitted.code == IngressCode::kAdmitted,
                      "retained typed timer remains critical-admissible while N is full");
      if (phase == TimeoutPhase::kProcessExitConfirmation)
        add_success(submitted.admitted);
      else
        add_rejection(submitted.admitted, RejectionReason::kTimeoutPhaseMismatch);
    }
    add_success(
        population.SubmitWorker(MakeWorkerCompleted(resident.primary_job, resident.worker, 2)));
    add_success(population.SubmitProcessExit(
        MakeProcessExit(resident.primary_job, resident, CompletionMode::kForced)));
    add_success(population.SubmitResourcesReleased(
        MakeResourcesReleased(resident.primary_job, allocation)));
    add_rejection(population.SubmitCleanup(MakeCleanupCompleted(resident.primary_job)),
                  RejectionReason::kInvariantViolation);
  };
  add_resident_critical(primary);
  add_resident_critical(secondary);
  result |= Check(turns.size() == 20 && std::all_of(turns.begin() + 2, turns.end(),
                                                    [](const auto& turn) {
                                                      return turn.ingress.code ==
                                                                 IngressCode::kAdmitted &&
                                                             turn.ingress.ingress_sequence != 0;
                                                    }),
                  "all eighteen source-specific critical turns admit while N remains full");
  const auto marker = population.SubmitShutdown();
  result |= Check(marker.code == IngressCode::kAdmitted && population.normal_occupancy() == 2 &&
                      population.live_critical_permit_count() == 19 &&
                      population.critical_occupancy() == 19 && population.total_occupancy() == 21,
                  "all source relationships plus shutdown occupy exact N=2 R=19 T=21");
  result |= Check(population.Release(normal_one.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "full reserve population releases through the keyed FIFO barrier");
  for (const auto& turn : turns)
    result |= turn.completion == Completion::Code::kSuccess
                  ? ConsumeCompletion(population, turn.ingress, turn.completion)
                  : ConsumeCompletion(population, turn.ingress, turn.completion, turn.rejection);
  result |= Check(population.WaitUntil(marker.ingress_sequence, WriterPhase::kShutdownMarker),
                  "full reserve drains all twenty FIFO turns before the shutdown marker");
  const auto marker_completion = population.TakeCompletion(marker.ingress_sequence);
  result |= Check(marker_completion && marker_completion->code == Completion::Code::kSuccess &&
                      !population.TakeCompletion(marker.ingress_sequence),
                  "full reserve shutdown marker completes exactly once");
  result |= Check(population.BeginShutdown() && SealedWithNoLiveIngress(population),
                  "shutdown retires all live resident permits and the global marker once");
  const auto completions_after_seal = population.completion_count();
  const auto disposed_after_seal = population.disposed_count();
  result |= Check(!population.BeginShutdown() && SealedWithNoLiveIngress(population) &&
                      population.completion_count() == completions_after_seal &&
                      population.disposed_count() == disposed_after_seal,
                  "repeated finalization cannot retire permits or completions twice");
  Config zero_jobs = PositiveConfig();
  zero_jobs.max_jobs = 0;
  Config zero_normal = PositiveConfig();
  zero_normal.normal_capacity = 0;
  Config overflow_jobs = PositiveConfig();
  overflow_jobs.max_jobs = std::numeric_limits<std::size_t>::max();
  result |= Check(overflow_jobs.critical_reserve() == 0 && overflow_jobs.total_capacity() == 0,
                  "critical-reserve overflow sentinel propagates to total capacity");
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
  int result = 0;
  const auto finish_terminal = [&](JobOrchestratorHarness& target) {
    result |= ExpectCommitted(
        target, target.SubmitSessionRetainRequested(MakeSessionRetainRequested(ids.primary_job)),
        EventType::kSessionRetainRequested);
    const auto retained = target.TakeSessionCandidate();
    result |= Check(retained.has_value(), "terminal setup takes its Session candidate");
    if (retained)
      result |= ExpectCommitted(target, target.SubmitSessionRetained(*retained),
                                EventType::kSessionRetained);
    result |= ExpectCommitted(
        target, target.SubmitFinalizationCompleted(MakeFinalizationCompleted(ids.primary_job)),
        EventType::kFinalizationCompleted);
    result |= ExpectCommitted(target,
                              target.SubmitTerminalOutcome(MakeTerminalOutcome(
                                  ids.primary_job, TerminalOutcome::kSucceeded)),
                              EventType::kTerminalOutcomeCommitted);
  };
  result |= DriveToRunning(harness, ids, EmptyAllocation());
  result |= Check(harness.ArmPause(WriterPhase::kBeforeDequeue) &&
                      harness.ArmBarrier(WriterPhase::kBeforeDequeue),
                  "Worker retry window is paused before first insertion");
  const auto setup_journal_attempts = harness.journal_attempts();
  const auto first = harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  const auto retry_queued =
      harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |= Check(retry_queued.code == IngressCode::kCoalescedPending &&
                      retry_queued.ingress_sequence == first.ingress_sequence,
                  "exact queued Worker retry coalesces to the original sequence");
  result |= Check(harness.WaitUntil(first.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "first Worker reaches the keyed before-dequeue barrier");
  result |= Check(harness.journal_attempts() == setup_journal_attempts,
                  "paused Worker preserves the pre-submission Journal count");
  const auto turns_before = harness.writer_turn_count();
  const auto attempts_before = harness.journal_attempts();
  const auto ingress_before = harness.CopyIngressSequences().size();
  const auto acks_before = harness.ack_authorization_count();
  result |= Check(harness.Release(first.ingress_sequence, WriterPhase::kBeforeDequeue),
                  "Worker retry window releases through the keyed barrier");
  result |= ConsumeCompletion(harness, first, Completion::Code::kSuccess);
  const auto turns_after = harness.writer_turn_count();
  const auto attempts_after = harness.journal_attempts();
  const auto ingress_after = harness.CopyIngressSequences().size();
  const auto acks_after = harness.ack_authorization_count();
  result |= Check(turns_after == turns_before + 1, "first Worker adds exactly one writer turn");
  result |=
      Check(attempts_after == attempts_before + 1, "first Worker adds exactly one Journal attempt");
  result |= Check(ingress_after == ingress_before,
                  "first Worker admission retains its assigned ingress sequence");
  result |= Check(acks_after == acks_before,
                  "first Worker completion does not authorize an ACK before terminal commit");
  result |= Check(!harness.RetireWorkerAck(ids.primary_job, 1),
                  "Worker gate cannot retire before its ACK effect is authorized");
  const auto retry_after_turn =
      harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  const auto distinct_sequence =
      harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2));
  const auto distinct_payload =
      harness.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 1));
  result |= Check(retry_after_turn.code == IngressCode::kCoalescedPending,
                  "exact post-turn retry coalesces while ACK obligation remains");
  result |= Check(distinct_sequence.code == IngressCode::kAlreadyPending &&
                      distinct_payload.code == IngressCode::kAlreadyPending,
                  "distinct Worker identity or payload remains with its source");
  result |= Check(harness.writer_turn_count() == turns_after &&
                      harness.journal_attempts() == attempts_after &&
                      harness.CopyIngressSequences().size() == ingress_after &&
                      harness.ack_authorization_count() == acks_after,
                  "coalesced and already-pending retries add no turn, Journal, sequence, or ACK");
  finish_terminal(harness);
  const auto terminal_snapshot = harness.Snapshot(ids.primary_job);
  result |= Check(harness.ack_authorization_count() == acks_after + 1 && terminal_snapshot &&
                      terminal_snapshot->pending_worker_id == ids.worker &&
                      terminal_snapshot->pending_worker_event_sequence == 1,
                  "terminal commit authorizes ACK for the retained Worker identity");
  const auto before_ack = CaptureEpoch(harness, ids.primary_job);
  result |= Check(
      !harness.RetireWorkerAck(MakeWorkerCompleted(ids.secondary_job, ids.worker, 1)) &&
          !harness.RetireWorkerAck(MakeWorkerCompleted(ids.primary_job, ids.secondary_worker, 1)) &&
          !harness.RetireWorkerAck(MakeWorkerFailed(ids.primary_job, ids.worker, 1)) &&
          !harness.RetireWorkerAck(MakeWorkerCompleted(ids.primary_job, ids.worker, 2)),
      "ACK retirement rejects wrong Job, Worker, event kind, and sequence identities");
  const auto acknowledged_worker = MakeWorkerCompleted(ids.primary_job, ids.worker, 1);
  result |= Check(
      harness.RetireWorkerAck(acknowledged_worker) && !harness.RetireWorkerAck(acknowledged_worker),
      "authorized ACK success retires its exact identity once");
  result |= Check(EpochUnchanged(harness, ids.primary_job, before_ack),
                  "ACK retirement consumes no ingress, Journal, effect, or snapshot mutation");
  const auto late = harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2));
  result |=
      Check(late.code == IngressCode::kAdmitted && late.ingress_sequence > first.ingress_sequence,
            "ACK retirement admits the formerly already-pending delivery exactly once");
  result |= ExpectCommitted(harness, late, EventType::kLateWorkerEvent);
  result |= Check(harness.ack_authorization_count() == acks_after + 2,
                  "ACK-retired late delivery authorizes exactly one late ACK");

  JobOrchestratorHarness failed_ack(PositiveConfig());
  result |= DriveToRunning(failed_ack, ids, EmptyAllocation());
  const auto failed_worker = MakeWorkerFailed(ids.primary_job, ids.worker, 1);
  result |=
      ExpectCommitted(failed_ack, failed_ack.SubmitWorker(failed_worker), EventType::kWorkerFailed);
  result |= Check(!failed_ack.RetireWorkerAck(failed_worker),
                  "failed Worker identity cannot retire before terminal ACK authorization");
  result |= ExpectCommitted(
      failed_ack,
      failed_ack.SubmitSessionRetainRequested(MakeSessionRetainRequested(ids.primary_job)),
      EventType::kSessionRetainRequested);
  const auto failed_session = failed_ack.TakeSessionCandidate();
  result |= Check(failed_session.has_value(), "failed Worker setup takes its Session candidate");
  if (failed_session)
    result |= ExpectCommitted(failed_ack, failed_ack.SubmitSessionRetained(*failed_session),
                              EventType::kSessionRetained);
  result |= ExpectCommitted(
      failed_ack,
      failed_ack.SubmitFinalizationCompleted(MakeFinalizationCompleted(ids.primary_job)),
      EventType::kFinalizationCompleted);
  result |= ExpectCommitted(failed_ack,
                            failed_ack.SubmitTerminalOutcome(
                                MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kFailed)),
                            EventType::kTerminalOutcomeCommitted);
  result |= Check(
      !failed_ack.RetireWorkerAck(MakeWorkerCompleted(ids.primary_job, ids.worker, 1)) &&
          failed_ack.RetireWorkerAck(failed_worker) && !failed_ack.RetireWorkerAck(failed_worker),
      "Worker-failed ACK retires only its exact event discriminator once");
  result |= ExpectCommitted(
      failed_ack, failed_ack.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 2)),
      EventType::kLateWorkerEvent);
  result |= Check(failed_ack.ack_authorization_count() == 2,
                  "retired Worker-failed gate admits one later distinct ACK obligation");

  JobOrchestratorHarness exit_retirement(PositiveConfig());
  result |= DriveToRunning(exit_retirement, ids, EmptyAllocation());
  const auto exit_first =
      exit_retirement.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  result |= ExpectCommitted(exit_retirement, exit_first, EventType::kWorkerCompleted);
  const auto exit_before = CaptureEpoch(exit_retirement, ids.primary_job);
  const auto exit_distinct =
      exit_retirement.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2));
  result |= Check(exit_distinct.code == IngressCode::kAlreadyPending &&
                      exit_distinct.ingress_sequence == 0 &&
                      EpochUnchanged(exit_retirement, ids.primary_job, exit_before),
                  "process-exit path retains the same distinct Worker obligation without burn");
  finish_terminal(exit_retirement);
  result |= Check(exit_retirement.ack_authorization_count() == 1,
                  "process-exit path first authorizes the terminal ACK");
  result |= ExpectCommitted(exit_retirement,
                            exit_retirement.SubmitProcessExit(MakeProcessExit(
                                ids.primary_job, ids, CompletionMode::kCooperative)),
                            EventType::kProcessExitConfirmed);
  const auto exit_retry =
      exit_retirement.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2));
  result |= Check(exit_retry.code == IngressCode::kAdmitted &&
                      exit_retry.ingress_sequence > exit_first.ingress_sequence,
                  "confirmed process exit admits the formerly already-pending delivery");
  result |= ExpectCommitted(exit_retirement, exit_retry, EventType::kLateWorkerEvent);
  result |= Check(exit_retirement.ack_authorization_count() == 2,
                  "process-exit-retired delivery authorizes exactly one late ACK");

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
  return result;
}

int JobIngressFailClosed() {
  int result = 0;
  const auto ids = Ids();
  const auto allocation = EmptyAllocation();
  JobOrchestratorHarness precommit(PositiveConfig());
  const auto before_precommit = CaptureEpoch(precommit, ids.primary_job);
  const auto disposed_before_precommit = precommit.disposed_count();
  precommit.InjectPrecommitMaterializationFailure();
  const auto materialization = precommit.Create();
  result |= ConsumeCompletion(precommit, materialization, Completion::Code::kServiceFailed);
  const auto after_precommit = CaptureEpoch(precommit, ids.primary_job);
  result |=
      Check(!after_precommit.snapshot &&
                after_precommit.journal_attempts == before_precommit.journal_attempts &&
                after_precommit.applies == before_precommit.applies &&
                after_precommit.effects == before_precommit.effects &&
                after_precommit.responses == before_precommit.responses &&
                after_precommit.acknowledgements == before_precommit.acknowledgements &&
                after_precommit.traces == before_precommit.traces &&
                after_precommit.writer_turns == before_precommit.writer_turns + 1 &&
                after_precommit.ingress_sequences.size() == 1 &&
                after_precommit.ingress_sequences.front() == materialization.ingress_sequence &&
                precommit.disposed_count() == disposed_before_precommit + 1 &&
                precommit.NoForbiddenPostcommitActions(),
            "precommit failure disposes exactly once with no Journal or postcommit axis");
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
  // Owner decision D13-1/A: R = 9 * J + 1 assigns one permit to every valid per-resident
  // critical relationship plus the marker. Once those permits are occupied, every corresponding
  // source gate is occupied too, so a free-gate reserve-acquisition failure is unreachable from a
  // valid state. Do not add a synthetic production seam to violate that invariant. The following
  // impossible-accounting case qualifies sticky fail-closed behavior without claiming to execute
  // the unreachable acquisition branch.
  JobOrchestratorHarness corruption(PositiveConfig());
  result |= DriveToRunning(corruption, ids, allocation);
  const auto critical_obligation = MakeWorkerCompleted(ids.primary_job, ids.worker, 1);
  const auto before_corruption = CaptureEpoch(corruption, ids.primary_job);
  const auto completions_before_corruption = corruption.completion_count();
  corruption.InjectAccountingCorruption();
  const auto corrupted_delivery = corruption.SubmitWorker(critical_obligation);
  const auto failure_sealed = corruption.BeginShutdown();
  const auto corrupted_retry = corruption.SubmitWorker(critical_obligation);
  result |= Check(corrupted_delivery.code == IngressCode::kServiceFailed &&
                      corrupted_delivery.ingress_sequence == 0 && failure_sealed &&
                      corrupted_retry.code == IngressCode::kServiceFailed &&
                      corrupted_retry.ingress_sequence == 0 && corruption.failed() &&
                      SealedWithNoLiveIngress(corruption) &&
                      corruption.completion_count() == completions_before_corruption &&
                      EpochUnchanged(corruption, ids.primary_job, before_corruption),
                  "impossible accounting corruption burns no sequence, Journal, ACK, completion, "
                  "or retry identity");
  JobOrchestratorHarness failure_waiter(PositiveConfig());
  result |= DriveToRunning(failure_waiter, ids, allocation);
  const auto prior_failure_sequences = failure_waiter.CopyIngressSequences();
  const auto failure_sequence = prior_failure_sequences.back() + 1;
  failure_waiter.InjectPrecommitMaterializationFailure();
  result |= Check(
      failure_waiter.ArmAdmissionPause() && failure_waiter.ArmBarrier(WriterPhase::kTurnFinished),
      "failure waiter arms admission and turn-finished barriers");
  std::optional<IngressResult> failed_turn;
  std::thread failing_producer([&] {
    failed_turn = failure_waiter.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
  });
  result |= Check(failure_waiter.WaitForAdmissionPause(),
                  "failure waiter holds the admitted input before writer processing");
  std::barrier waiter_ready(2);
  bool waiter_result = true;
  std::thread terminal_waiter([&] {
    waiter_ready.arrive_and_wait();
    waiter_result = failure_waiter.WaitUntil(failure_sequence, WriterPhase::kTurnFinished);
  });
  waiter_ready.arrive_and_wait();
  result |= Check(failure_waiter.WaitForWaitUntilAttempts(1),
                  "turn-finished waiter blocks before the injected failure is released");
  result |= Check(failure_waiter.ReleaseAdmissionPause(),
                  "failure waiter releases the input into injected failure");
  failing_producer.join();
  terminal_waiter.join();
  result |= Check(failed_turn && failed_turn->code == IngressCode::kAdmitted && !waiter_result,
                  "failure disposal wakes an armed turn-finished waiter for fallback handling");
  if (failed_turn)
    result |= ConsumeCompletion(failure_waiter, *failed_turn, Completion::Code::kServiceFailed);
  JobOrchestratorHarness residual_permit(PositiveConfig());
  residual_permit.InjectResidualCriticalPermit();
  const auto residual_marker = residual_permit.SubmitShutdown();
  result |= Check(
      residual_marker.code == IngressCode::kAdmitted &&
          residual_permit.WaitUntil(residual_marker.ingress_sequence, WriterPhase::kShutdownMarker),
      "residual permit shutdown marker reaches its control barrier");
  result |= Check(!residual_permit.BeginShutdown() && residual_permit.failed() &&
                      residual_permit.sealed() && residual_permit.live_critical_permit_count() == 1,
                  "residual critical permit accounting is detected before sticky failure seal");
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
  result |= Check(harness.CloseCallbackLease() && SealedWithNoLiveIngress(harness),
                  "lease release seals only after FIFO, live permits, and marker reach zero");
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
    result |= Check(
        second_endpoint.ReleaseLease() && clean.BeginShutdown() && SealedWithNoLiveIngress(clean),
        "sealing waits for every callback lease and retires all live occupancy");
    result |= Check(first_endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code ==
                            IngressCode::kAdmissionClosed &&
                        second_endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code ==
                            IngressCode::kAdmissionClosed,
                    "both sealed callback endpoints cannot reach writer state");
  }
  {
    JobOrchestratorHarness failed(PositiveConfig());
    auto endpoint = failed.RetainedCallback();
    result |= Check(
        failed.ConcurrentCallbackInvocation() && failed.failed() && SealedWithNoLiveIngress(failed),
        "concurrent invocation seals only after its final reference and occupancy");
    const auto marker = failed.SubmitShutdown();
    result |= Check(marker.code == IngressCode::kServiceFailed && failed.sealed(),
                    "failed service remains deterministically sealed without a shutdown marker");
    result |= Check(
        endpoint.Invoke(MakeJobCreated(Ids().primary_job)).code == IngressCode::kServiceFailed,
        "late endpoint returns sticky service_failed after sealing");
  }
  {
    JobOrchestratorHarness live_invocation(PositiveConfig());
    const auto ids = Ids();
    result |= DriveToRunning(live_invocation, ids, EmptyAllocation());
    auto endpoint = live_invocation.RetainedCallback();
    result |= Check(live_invocation.ArmAdmissionPause(),
                    "callback invocation pause is armed before producer entry");
    IngressResult invocation_result{};
    std::thread invocation([&] {
      invocation_result = endpoint.Invoke(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
    });
    result |= Check(live_invocation.WaitForAdmissionPause(),
                    "callback invocation retains its reference inside admission");
    result |= Check(live_invocation.LatchReadinessFailure() && live_invocation.failed() &&
                        !live_invocation.sealed(),
                    "failure cannot seal while one callback invocation reference is live");
    result |= Check(live_invocation.ReleaseAdmissionPause(),
                    "live callback invocation is explicitly released after failure latch");
    invocation.join();
    result |= Check(invocation_result.code == IngressCode::kServiceFailed &&
                        SealedWithNoLiveIngress(live_invocation),
                    "final callback reference release permits exact failed sealing");
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
    result |=
        Check(callback_race.BeginShutdown() && SealedWithNoLiveIngress(callback_race) &&
                  endpoint.Invoke(MakeJobCreated(Ids().secondary_job)).code ==
                      IngressCode::kAdmissionClosed,
              "final seal closes callback access only after ingress and live gates reach zero");
  }
  return result;
}

int CheckReadinessCase(WriterPhase phase) {
  JobOrchestratorHarness harness(PositiveConfig());
  const auto ids = Ids();
  int result = 0;
  const auto failure_is_quiescent = [&] {
    return Check(
        harness.WaitForWriterIdle() && harness.failed() && SealedWithNoLiveIngress(harness),
        "readiness failure automatically seals after writer-owned work reaches idle");
  };
  if (phase == WriterPhase::kAfterDecision || phase == WriterPhase::kShutdownMarker ||
      phase == WriterPhase::kBeforeDequeue || phase == WriterPhase::kAfterDequeueAuthorized)
    result |= Check(harness.ArmPause(phase) && harness.ArmBarrier(phase),
                    "readiness race is armed before the source is submitted");
  if (phase == WriterPhase::kAfterDecision) {
    const auto rejected = harness.SubmitTerminate(Terminate(ids.secondary_job));
    result |= Check(harness.WaitUntil(rejected.ingress_sequence, phase),
                    "rejection reaches decision barrier");
    result |= Check(harness.LatchReadinessFailure() && !harness.sealed(),
                    "readiness failure cannot seal while reducer rejection is in flight");
    result |= Check(harness.Release(rejected.ingress_sequence, phase),
                    "readiness-latched reducer rejection is released");
    result |= ConsumeCompletion(harness, rejected, Completion::Code::kReducerRejection,
                                RejectionReason::kJobNotFound);
    return result | failure_is_quiescent();
  }
  if (phase == WriterPhase::kShutdownMarker) {
    const auto completion_before = harness.completion_count();
    const auto marker = harness.SubmitShutdown();
    result |= Check(harness.WaitUntil(marker.ingress_sequence, phase),
                    "shutdown control reaches barrier");
    result |= Check(harness.LatchReadinessFailure() && SealedWithNoLiveIngress(harness),
                    "processed empty shutdown marker permits immediate readiness-failure seal");
    result |= Check(harness.Release(marker.ingress_sequence, phase),
                    "readiness-sealed shutdown marker barrier is released");
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
    result |= Check(harness.failed(), "shutdown control completes before failure handling");
    return result | failure_is_quiescent();
  }
  if (phase == WriterPhase::kAfterDequeueAuthorized) {
    const auto accepted = harness.Create();
    result |= Check(harness.WaitUntil(accepted.ingress_sequence, phase),
                    "entry is authorized before the readiness race");
    result |= Check(harness.LatchReadinessFailure() && !harness.sealed(),
                    "readiness failure cannot seal while an authorized turn is in flight");
    result |= Check(harness.Release(accepted.ingress_sequence, phase),
                    "readiness-latched authorized turn is released without revocation");
    result |= ConsumeCompletion(harness, accepted, Completion::Code::kSuccess);
    result |= Check(harness.journal_attempts() == 1,
                    "authorized turn completes its reducer and Journal disposition");
    return result | failure_is_quiescent();
  }
  if (phase == WriterPhase::kBeforeDequeue) {
    const auto writer_before = harness.writer_turn_count();
    const auto apply_before = harness.apply_count();
    const auto journal_before = harness.journal_attempts();
    const auto queued = harness.Create();
    result |= Check(harness.WaitUntil(queued.ingress_sequence, phase),
                    "queued turn reaches the before-dequeue barrier");
    result |= Check(harness.LatchReadinessFailure() && !harness.sealed(),
                    "readiness failure cannot seal while queued ingress remains owned");
    result |= Check(harness.Release(queued.ingress_sequence, phase),
                    "readiness-latched queued ingress is released for failure disposal");
    result |= ConsumeCompletion(harness, queued, Completion::Code::kServiceFailed);
    result |= Check(harness.writer_turn_count() == writer_before &&
                        harness.apply_count() == apply_before &&
                        harness.journal_attempts() == journal_before,
                    "before-dequeue readiness failure starts no writer/apply/Journal turn");
    return result | failure_is_quiescent();
  }
  // Use an effect-bearing resources_committed turn.  Thus every accepted readiness position
  // proves the required committed activation, timer effect/mapping, and response before failure
  // handling.
  const auto created = harness.Create();
  result |= ConsumeCompletion(harness, created, Completion::Code::kSuccess);
  const auto effects_before = harness.effect_count();
  const auto responses_before = harness.response_count();
  const auto trace_before = harness.CopyTrace().size();
  result |= Check(harness.ArmPause(phase) && harness.ArmBarrier(phase),
                  "effect-bearing readiness race is armed after setup completes");
  const auto accepted =
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, EmptyAllocation()));
  result |= Check(harness.WaitUntil(accepted.ingress_sequence, phase),
                  "effect-bearing accepted turn reaches requested barrier");
  result |= Check(harness.LatchReadinessFailure() && !harness.sealed(),
                  "readiness failure cannot seal while committed work is in flight");
  result |= Check(harness.Release(accepted.ingress_sequence, phase),
                  "readiness-latched committed work is released");
  result |= ConsumeCompletion(harness, accepted, Completion::Code::kSuccess);
  const auto snapshot = harness.Snapshot(ids.primary_job);
  result |= Check(snapshot.has_value() && snapshot->resource_status == ResourceStatus::kCommitted,
                  "every accepted readiness race installs the committed resources snapshot");
  ExpectedTrace expected_trace;
  expected_trace.Turn(2, EventType::kResourcesCommitted,
                      {{EffectId::kArmPreparationTimeout, "timer:arm:preparation:1"}},
                      {"source:timer:preparation", "source:resources_released"});
  for (std::size_t index = 0; index != expected_trace.records.size(); ++index)
    expected_trace.records[index].ordinal = static_cast<std::uint64_t>(trace_before + index + 1);
  const auto trace = harness.CopyTrace();
  const std::vector<TraceRecord> trace_delta(
      trace.begin() + static_cast<std::ptrdiff_t>(trace_before), trace.end());
  result |= Check(harness.effect_count() == effects_before + 1 &&
                      harness.response_count() == responses_before + 1 &&
                      trace_delta == expected_trace.records,
                  "successful committed turn completes one exact effect and response trace");
  return result | failure_is_quiescent();
}

int JobIngressReadinessFailure() {
  int result = 0;
  for (const auto phase :
       {WriterPhase::kBeforeDequeue, WriterPhase::kAfterDequeueAuthorized,
        WriterPhase::kAfterDecision, WriterPhase::kBeforeCommit, WriterPhase::kAfterCommit,
        WriterPhase::kAfterApply, WriterPhase::kAfterEffects, WriterPhase::kResponseReleased,
        WriterPhase::kShutdownMarker})
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

int DriveToPreparing(JobOrchestratorHarness& harness, const IdFixtures& ids,
                     const AllocationFixture& allocation) {
  int result = 0;
  result |= ExpectCommitted(harness, harness.Create(), EventType::kJobCreated);
  result |= ExpectCommitted(
      harness,
      harness.SubmitResourcesCommitted(MakeResourcesCommitted(ids.primary_job, allocation)),
      EventType::kResourcesCommitted);
  result |= ExpectCommitted(harness, harness.SubmitGeneratedLaunchIntent(allocation),
                            EventType::kWorkerLaunchIntent);
  const auto launch = harness.TakeRunnerCandidate();
  result |= Check(launch.has_value(), "preparing setup consumes launch candidate");
  if (launch)
    result |= ExpectCommitted(harness, harness.SubmitLaunchObserved(*launch),
                              EventType::kWorkerLaunchObserved);
  return result;
}

bool Axes(const JobOrchestratorHarness& h, const Uuid& job, JobState state,
          std::optional<TerminalOutcome> reason, bool candidate, CompletionMode mode, bool ack) {
  const auto s = h.Snapshot(job);
  return s && s->state == state && s->latched_reason == reason &&
         s->completion_candidate == candidate && s->completion_mode == mode &&
         s->pending_worker_event_ack == ack;
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

int OrderedRaceQualification() {
  const auto ids = Ids();
  int result = 0;
  const auto turn = [](EventType event, std::vector<std::string> registrations,
                       std::vector<std::pair<EffectId, std::string>> effects,
                       std::size_t acknowledgements, bool pending_ack, EventPayload payload) {
    return OrderedTurnExpectation{event,
                                  std::move(payload),
                                  std::move(registrations),
                                  std::move(effects),
                                  acknowledgements,
                                  pending_ack};
  };
  const auto cancel_turn =
      turn(EventType::kCancelAccepted, {"source:timer:cooperative_stop"},
           {{EffectId::kRequestCooperativeStop, "handoff:cooperative-stop"},
            {EffectId::kArmCooperativeStopTimeout, "timer:arm:cooperative_stop:1"}},
           0, false, PrincipalPayload{std::string(kPrincipal)});
  const auto terminate_without_cooperative_timer = [&](EventType event, bool pending_ack = false) {
    return turn(event, {"source:timer:process_exit_confirmation"},
                {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
                 {EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop:0"},
                 {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
                  "timer:arm:process_exit_confirmation:1"}},
                0, pending_ack, PrincipalPayload{std::string(kPrincipal)});
  };
  const auto terminate_after_cooperative_stop = [&](EventType event) {
    return turn(event, {"source:timer:process_exit_confirmation"},
                {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
                 {EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop:1"},
                 {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
                  "timer:arm:process_exit_confirmation:1"}},
                0, false, PrincipalPayload{std::string(kPrincipal)});
  };
  const auto preparation_timeout =
      turn(EventType::kTimeoutExpired, {},
           {{EffectId::kDisarmPreparationTimeout, "timer:disarm:preparation:1"},
            {EffectId::kRequestCooperativeStop, "handoff:cooperative-stop"},
            {EffectId::kArmCooperativeStopTimeout, "timer:arm:cooperative_stop:1"}},
           0, false, TimeoutExpiredPayload{TimeoutPhase::kPreparation, 1});
  const auto worker_running =
      turn(EventType::kWorkerRunning, {},
           {{EffectId::kDisarmPreparationTimeout, "timer:disarm:preparation:1"},
            {EffectId::kArmExecutionTimeout, "timer:arm:execution:1"}},
           0, false, WorkerRunningPayload{ids.worker});
  const auto worker_no_effect = [&](EventType event) {
    return turn(event, {}, {}, 0, true, WorkerEventPayload{ids.worker, 1});
  };
  const auto worker_stopping = [&](EventType event, std::string action) {
    return turn(event, {}, {{EffectId::kDisarmCooperativeStopTimeout, std::move(action)}}, 0, true,
                WorkerEventPayload{ids.worker, 1});
  };
  const auto execution_timeout_running =
      turn(EventType::kTimeoutExpired, {},
           {{EffectId::kRequestCooperativeStop, "handoff:cooperative-stop"},
            {EffectId::kArmCooperativeStopTimeout, "timer:arm:cooperative_stop:1"}},
           0, false, TimeoutExpiredPayload{TimeoutPhase::kExecution, 1});
  const auto execution_timeout_finalizing =
      turn(EventType::kTimeoutExpired, {},
           {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
            {EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop:0"},
            {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
             "timer:arm:process_exit_confirmation:1"}},
           0, true, TimeoutExpiredPayload{TimeoutPhase::kExecution, 1});
  const auto cooperative_timeout =
      turn(EventType::kTimeoutExpired, {},
           {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop:1"},
            {EffectId::kRequestForcedStop, "handoff:forced-stop"},
            {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
             "timer:arm:process_exit_confirmation:1"}},
           0, false, TimeoutExpiredPayload{TimeoutPhase::kCooperativeStop, 1});
  const auto cooperative_timeout_worker = [&](EventType event) {
    return worker_stopping(event, "timer:disarm:cooperative_stop:1");
  };
  const auto cooperative_timeout_exit =
      turn(EventType::kProcessExitConfirmed, {},
           {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop"},
            {EffectId::kDisarmProcessExitConfirmationTimeout,
             "timer:disarm:process_exit_confirmation:1"}},
           0, false, ProcessExitConfirmedPayload{CompletionMode::kForced, ids.launch_operation});
  const auto cooperative_exit_before_timeout =
      turn(EventType::kProcessExitConfirmed, {},
           {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop"},
            {EffectId::kDisarmProcessExitConfirmationTimeout,
             "timer:disarm:process_exit_confirmation:0"}},
           0, false, ProcessExitConfirmedPayload{CompletionMode::kForced, ids.launch_operation});
  const auto finalization_completed =
      turn(EventType::kFinalizationCompleted, {}, {}, 0, true, EmptyPayload{});
  const auto finalization_failed =
      turn(EventType::kFinalizationFailed, {}, {}, 0, true, EmptyPayload{});
  const auto terminal_success =
      turn(EventType::kTerminalOutcomeCommitted,
           {"source:timer:process_exit_confirmation", "source:cleanup"},
           {{EffectId::kDisarmExecutionTimeout, "timer:disarm:execution:1"},
            {EffectId::kAckTerminalWorkerEventIfPending, "ack:terminal:1"},
            {EffectId::kPublishTerminalResult, "publish:succeeded"},
            {EffectId::kArmProcessExitConfirmationTimeoutIfNeeded,
             "timer:arm:process_exit_confirmation:1"}},
           1, true, TerminalOutcomePayload{TerminalOutcome::kSucceeded});
  const auto process_exit_timeout =
      turn(EventType::kTimeoutExpired, {},
           {{EffectId::kRequestForcedStop, "handoff:forced-stop"},
            {EffectId::kQuarantineResources, "safety:quarantine"},
            {EffectId::kSetReadinessFalse, "safety:set_readiness_false"}},
           1, true, TimeoutExpiredPayload{TimeoutPhase::kProcessExitConfirmation, 1});
  const auto process_exit_after_timeout =
      turn(EventType::kProcessExitConfirmed, {},
           {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop"},
            {EffectId::kDisarmProcessExitConfirmationTimeout,
             "timer:disarm:process_exit_confirmation:1"}},
           1, false, ProcessExitConfirmedPayload{CompletionMode::kForced, ids.launch_operation});
  const auto process_exit_before_timeout =
      turn(EventType::kProcessExitConfirmed, {},
           {{EffectId::kDisarmCooperativeStopTimeout, "timer:disarm:cooperative_stop"},
            {EffectId::kDisarmProcessExitConfirmationTimeout,
             "timer:disarm:process_exit_confirmation:1"}},
           1, false, ProcessExitConfirmedPayload{CompletionMode::kForced, ids.launch_operation});

  auto timer = [&](JobOrchestratorHarness& harness, TimeoutPhase phase) {
    const auto submitted = harness.SubmitTimeout({ids.primary_job, phase, 1});
    return submitted.discarded ? IngressResult{} : submitted.admitted;
  };
  auto drain_stops = [&](JobOrchestratorHarness& harness, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
      result |= Check(harness.TakeRunnerCandidate().has_value(),
                      "ordered stop handoff candidate is exact");
  };
  auto committed_pair = [&](JobOrchestratorHarness& harness, const OrderedPair& pair,
                            const OrderedTurnExpectation& first,
                            const OrderedTurnExpectation& second, std::string_view message) {
    result |= CheckOrderedPair(pair, message);
    result |= ConsumeOrderedCommittedExact(harness, pair, true, first,
                                           "first accepted turn has exact trace");
    result |= ConsumeOrderedCommittedExact(harness, pair, false, second,
                                           "second accepted turn has exact trace");
    const auto after = CaptureEpoch(harness, ids.primary_job);
    const auto after_first =
        pair.paused.snapshot ? ExpectedSnapshotAfter(*pair.paused.snapshot, first) : std::nullopt;
    const auto expected_snapshot =
        after_first ? ExpectedSnapshotAfter(*after_first, second) : std::nullopt;
    result |= Check(
        after.journal_attempts == pair.paused.journal_attempts + 2 &&
            after.applies == pair.paused.applies + 2 &&
            after.effects == pair.paused.effects + first.effects.size() + second.effects.size() &&
            after.responses == pair.paused.responses + 2 &&
            after.acknowledgements == second.ack_authorizations &&
            after.writer_turns == pair.paused.writer_turns + 2 &&
            after.ingress_sequences == pair.paused.ingress_sequences && expected_snapshot &&
            after.snapshot && SnapshotEqual(*expected_snapshot, *after.snapshot),
        "accepted pair adds exactly two ordered turns and the exact reducer snapshot");
    result |= CheckAckAxes(harness, second, "accepted pair has exact final ACK axes");
    result |= CheckPairTraceDelta(harness, pair, {&first, &second});
  };

  for (const bool cancel_first : {true, false}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToRunning(harness, ids, EmptyAllocation());
    const auto pair =
        cancel_first
            ? QueueOrderedPair(
                  harness, [&] { return harness.SubmitCancel(Cancel(ids.primary_job)); },
                  [&] { return harness.SubmitTerminate(Terminate(ids.primary_job)); })
            : QueueOrderedPair(
                  harness, [&] { return harness.SubmitTerminate(Terminate(ids.primary_job)); },
                  [&] { return harness.SubmitCancel(Cancel(ids.primary_job)); });
    if (cancel_first) {
      committed_pair(harness, pair, cancel_turn,
                     terminate_after_cooperative_stop(EventType::kTerminateAccepted),
                     "cancel then terminate is FIFO-linearized at keyed dequeue");
      drain_stops(harness, 2);
      result |= Check(Axes(harness, ids.primary_job, JobState::kStopping,
                           TerminalOutcome::kCancelled, false, CompletionMode::kForced, false),
                      "cancel then terminate preserves first cause and escalates only mode");
    } else {
      result |= CheckOrderedPair(pair, "terminate then cancel is FIFO-linearized at keyed dequeue");
      result |= ConsumeOrderedCommittedExact(
          harness, pair, true, terminate_without_cooperative_timer(EventType::kTerminateAccepted),
          "terminate-first turn has exact trace");
      result |= ConsumeOrderedRejection(
          harness, pair, false, RejectionReason::kStopCauseAlreadyLatched,
          terminate_without_cooperative_timer(EventType::kTerminateAccepted));
      result |= Check(Axes(harness, ids.primary_job, JobState::kStopping,
                           TerminalOutcome::kTerminated, false, CompletionMode::kForced, false),
                      "terminate first retains its first cause and cancel has no mutation");
      drain_stops(harness, 1);
    }
    result |=
        Check(harness.VerifyFakes(), "ordered command pair consumes every exact fake candidate");
  }

  for (const bool timeout_first : {true, false}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToPreparing(harness, ids, EmptyAllocation());
    result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kPreparation),
                    "preparation generation one is retained before ordered notification");
    const auto pair = timeout_first
                          ? QueueOrderedPair(
                                harness, [&] { return timer(harness, TimeoutPhase::kPreparation); },
                                [&] {
                                  return harness.SubmitWorkerRunning(
                                      MakeWorkerRunning(ids.primary_job, ids.worker));
                                })
                          : QueueOrderedPair(
                                harness,
                                [&] {
                                  return harness.SubmitWorkerRunning(
                                      MakeWorkerRunning(ids.primary_job, ids.worker));
                                },
                                [&] { return timer(harness, TimeoutPhase::kPreparation); });
    result |=
        CheckOrderedPair(pair, "preparation timeout and Worker running share a keyed FIFO epoch");
    if (timeout_first) {
      result |= ConsumeOrderedCommittedExact(harness, pair, true, preparation_timeout,
                                             "preparation timeout has exact trace");
      result |= ConsumeOrderedRejection(
          harness, pair, false, RejectionReason::kEventNotAllowedInState, preparation_timeout);
      result |= Check(Axes(harness, ids.primary_job, JobState::kStopping,
                           TerminalOutcome::kTimedOut, false, CompletionMode::kCooperative, false),
                      "timeout-first preparation order latches timeout and rejects Worker running");
      drain_stops(harness, 1);
    } else {
      result |= ConsumeOrderedCommittedExact(harness, pair, true, worker_running,
                                             "Worker running has exact trace");
      result |= ConsumeOrderedRejection(harness, pair, false,
                                        RejectionReason::kTimeoutPhaseMismatch, worker_running);
      result |= Check(Axes(harness, ids.primary_job, JobState::kRunning, std::nullopt, false,
                           CompletionMode::kNone, false),
                      "running-first preparation order rejects the stale timer without mutation");
    }
    result |=
        Check(harness.VerifyFakes(), "preparation ordered pair has no unconsumed fake candidate");
  }

  for (const bool worker_failed : {false, true}) {
    for (const int cause : {0, 1, 2}) {
      for (const bool control_first : {true, false}) {
        JobOrchestratorHarness harness(PositiveConfig());
        result |= DriveToRunning(harness, ids, EmptyAllocation());
        const auto control = [&] {
          if (cause == 0) return harness.SubmitCancel(Cancel(ids.primary_job));
          if (cause == 1) return harness.SubmitTerminate(Terminate(ids.primary_job));
          return timer(harness, TimeoutPhase::kExecution);
        };
        const auto worker = [&] {
          return worker_failed
                     ? harness.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 1))
                     : harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
        };
        const auto pair = control_first ? QueueOrderedPair(harness, control, worker)
                                        : QueueOrderedPair(harness, worker, control);
        const auto control_expected =
            cause == 0 ? cancel_turn
            : cause == 1
                ? terminate_without_cooperative_timer(EventType::kTerminateAccepted, !control_first)
            : control_first ? execution_timeout_running
                            : execution_timeout_finalizing;
        const auto worker_expected =
            control_first
                ? worker_stopping(
                      worker_failed ? EventType::kWorkerFailed : EventType::kWorkerCompleted,
                      (cause == 0 || cause == 2) ? "timer:disarm:cooperative_stop:1"
                                                 : "timer:disarm:cooperative_stop:0")
                : worker_no_effect(worker_failed ? EventType::kWorkerFailed
                                                 : EventType::kWorkerCompleted);
        result |=
            CheckOrderedPair(pair, "control and terminal Worker fact use explicit FIFO order");
        if (control_first) {
          committed_pair(harness, pair, control_expected, worker_expected,
                         "control-first Worker pair has exact reducer trace");
          drain_stops(harness, 1);
          const auto reason = cause == 0   ? TerminalOutcome::kCancelled
                              : cause == 1 ? TerminalOutcome::kTerminated
                                           : TerminalOutcome::kTimedOut;
          result |=
              Check(Axes(harness, ids.primary_job, JobState::kFinalizing, reason, false,
                         cause == 1 ? CompletionMode::kForced : CompletionMode::kCooperative, true),
                    "control-first Worker preserves first cause and pending ACK");
        } else {
          if (cause == 0) {
            result |= ConsumeOrderedCommittedExact(harness, pair, true, worker_expected,
                                                   "Worker-first turn has exact trace");
            result |= ConsumeOrderedRejection(
                harness, pair, false, RejectionReason::kCommandNotAllowedInState, worker_expected);
          } else {
            committed_pair(harness, pair, worker_expected, control_expected,
                           "Worker-first control pair has exact reducer trace");
            drain_stops(harness, 1);
          }
          const auto reason = worker_failed ? std::optional{TerminalOutcome::kFailed}
                              : cause == 2  ? std::optional{TerminalOutcome::kTimedOut}
                                            : std::nullopt;
          result |= Check(Axes(harness, ids.primary_job, JobState::kFinalizing, reason,
                               !worker_failed && cause != 2,
                               cause == 0 ? CompletionMode::kNone : CompletionMode::kForced, true),
                          "Worker-first order preserves candidate replacement and pending ACK");
        }
        result |= Check(harness.VerifyFakes(), "control/Worker order drains exact fake candidates");
      }
    }
  }

  for (const int fact_kind : {0, 1, 2}) {
    const bool worker_failed = fact_kind == 1;
    const bool process_exit = fact_kind == 2;
    for (const bool timeout_first : {true, false}) {
      JobOrchestratorHarness harness(PositiveConfig());
      result |= DriveToRunning(harness, ids, EmptyAllocation());
      result |= ExpectCommitted(harness, harness.SubmitCancel(Cancel(ids.primary_job)),
                                EventType::kCancelAccepted);
      drain_stops(harness, 1);
      result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kCooperativeStop),
                      "cooperative-stop generation one is retained before ordered pair");
      const auto worker_event =
          worker_failed ? EventType::kWorkerFailed : EventType::kWorkerCompleted;
      const auto fact = [&] {
        if (process_exit)
          return harness.SubmitProcessExit(
              MakeProcessExit(ids.primary_job, ids, CompletionMode::kForced));
        return worker_failed
                   ? harness.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 1))
                   : harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 1));
      };
      const auto fact_expected =
          process_exit
              ? (timeout_first ? cooperative_timeout_exit : cooperative_exit_before_timeout)
          : timeout_first ? cooperative_timeout_worker(worker_event)
                          : worker_stopping(worker_event, "timer:disarm:cooperative_stop:1");
      const auto pair =
          timeout_first
              ? QueueOrderedPair(
                    harness, [&] { return timer(harness, TimeoutPhase::kCooperativeStop); }, fact)
              : QueueOrderedPair(harness, fact,
                                 [&] { return timer(harness, TimeoutPhase::kCooperativeStop); });
      result |=
          CheckOrderedPair(pair, "cooperative timeout and terminal fact use explicit FIFO order");
      if (timeout_first) {
        committed_pair(harness, pair, cooperative_timeout, fact_expected,
                       "cooperative timeout-first pair has exact trace");
        drain_stops(harness, 1);
      } else {
        result |= ConsumeOrderedCommittedExact(harness, pair, true, fact_expected,
                                               "terminal fact-first turn has exact trace");
        result |= ConsumeOrderedRejection(harness, pair, false,
                                          RejectionReason::kTimeoutPhaseMismatch, fact_expected);
      }
      const auto mode = timeout_first ? CompletionMode::kForced : CompletionMode::kCooperative;
      result |=
          Check(Axes(harness, ids.primary_job, JobState::kFinalizing, TerminalOutcome::kCancelled,
                     false, mode, !process_exit),
                "cooperative ordered pair preserves cause and exact gate/ACK axes kind=" +
                    std::to_string(fact_kind) + " timeout_first=" + std::to_string(timeout_first));
      result |= Check(harness.VerifyFakes(), "cooperative ordered pair drains fake candidates");
    }
  }

  {
    JobOrchestratorHarness timeout_only(PositiveConfig());
    result |= DriveToRunning(timeout_only, ids, EmptyAllocation());
    result |= ExpectCommitted(timeout_only, timeout_only.SubmitCancel(Cancel(ids.primary_job)),
                              EventType::kCancelAccepted);
    drain_stops(timeout_only, 1);
    result |= Check(timeout_only.RetainTimerLease(ids.primary_job, TimeoutPhase::kCooperativeStop),
                    "standalone timeout retains cooperative generation one");
    result |= ExpectCommitted(timeout_only, timer(timeout_only, TimeoutPhase::kCooperativeStop),
                              EventType::kTimeoutExpired);
    const auto snapshot = timeout_only.Snapshot(ids.primary_job);
    result |= Check(snapshot && !snapshot->pending_worker_event_ack &&
                        !snapshot->pending_worker_id && !snapshot->pending_worker_event_sequence &&
                        timeout_only.ack_authorization_count() == 0,
                    "cooperative timeout alone creates no Worker ACK obligation");
    drain_stops(timeout_only, 1);
  }
  {
    JobOrchestratorHarness worker_only(PositiveConfig());
    result |= DriveToRunning(worker_only, ids, EmptyAllocation());
    result |= ExpectCommitted(worker_only, worker_only.SubmitCancel(Cancel(ids.primary_job)),
                              EventType::kCancelAccepted);
    drain_stops(worker_only, 1);
    result |= ExpectCommitted(
        worker_only, worker_only.SubmitWorker(MakeWorkerFailed(ids.primary_job, ids.worker, 1)),
        EventType::kWorkerFailed);
    const auto snapshot = worker_only.Snapshot(ids.primary_job);
    result |= Check(snapshot && snapshot->pending_worker_event_ack &&
                        snapshot->pending_worker_id == ids.worker &&
                        snapshot->pending_worker_event_sequence == 1 &&
                        worker_only.ack_authorization_count() == 0,
                    "Worker failure alone creates its exact pending ACK before authorization");
  }

  for (const bool failure_first : {true, false}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToFinalizationPending(harness, ids, EmptyAllocation());
    const auto completed = [&] {
      return harness.SubmitFinalizationCompleted(MakeFinalizationCompleted(ids.primary_job));
    };
    const auto failed = [&] {
      return harness.SubmitFinalizationFailed(MakeFinalizationFailed(ids.primary_job));
    };
    const auto pair = failure_first ? QueueOrderedPair(harness, failed, completed)
                                    : QueueOrderedPair(harness, completed, failed);
    result |= CheckOrderedPair(pair, "finalization completion and failure use explicit FIFO order");
    const auto& accepted = failure_first ? finalization_failed : finalization_completed;
    result |= ConsumeOrderedCommittedExact(harness, pair, true, accepted,
                                           "first finalization fact has exact trace");
    result |= ConsumeOrderedRejection(harness, pair, false, RejectionReason::kInvariantViolation,
                                      accepted);
    const auto snapshot = harness.Snapshot(ids.primary_job);
    result |= Check(
        snapshot && snapshot->state == JobState::kFinalizing &&
            snapshot->latched_reason ==
                (failure_first ? std::optional{TerminalOutcome::kFailed} : std::nullopt) &&
            snapshot->completion_candidate == !failure_first &&
            snapshot->finalization_status ==
                (failure_first ? FinalizationStatus::kFailed : FinalizationStatus::kCompleted) &&
            snapshot->cleanup_status ==
                (failure_first ? CleanupStatus::kIncomplete : CleanupStatus::kPending) &&
            AckAxesMatch(harness, accepted),
        "first finalization fact alone selects success or failure outcome");
  }

  for (const bool timeout_first : {true, false}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToFinalizing(harness, ids, EmptyAllocation());
    const auto terminal = [&] {
      return harness.SubmitTerminalOutcome(
          MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded));
    };
    const auto pair =
        timeout_first
            ? QueueOrderedPair(
                  harness, [&] { return timer(harness, TimeoutPhase::kExecution); }, terminal)
            : QueueOrderedPair(harness, terminal,
                               [&] { return timer(harness, TimeoutPhase::kExecution); });
    result |= CheckOrderedPair(
        pair, "finalizing execution timeout and terminal success use explicit FIFO order");
    if (timeout_first) {
      result |= ConsumeOrderedCommittedExact(harness, pair, true, execution_timeout_finalizing,
                                             "finalizing execution timeout has exact trace");
      drain_stops(harness, 1);
      result |=
          ConsumeOrderedRejection(harness, pair, false, RejectionReason::kTerminalOutcomeMismatch,
                                  execution_timeout_finalizing);
      result |= Check(Axes(harness, ids.primary_job, JobState::kFinalizing,
                           TerminalOutcome::kTimedOut, false, CompletionMode::kForced, true),
                      "execution timeout replaces success before terminal commit");
    } else {
      result |= ConsumeOrderedCommittedExact(harness, pair, true, terminal_success,
                                             "terminal success has exact trace");
      result |= ConsumeOrderedRejection(harness, pair, false,
                                        RejectionReason::kTimeoutPhaseMismatch, terminal_success);
      result |= Check(Axes(harness, ids.primary_job, JobState::kSucceeded, std::nullopt, true,
                           CompletionMode::kNone, true),
                      "terminal success first publishes the candidate and retains ACK obligation");
    }
    result |= Check(harness.VerifyFakes(), "finalizing ordered pair drains exact fake candidates");
  }

  for (const bool timeout_first : {true, false}) {
    JobOrchestratorHarness harness(PositiveConfig());
    result |= DriveToFinalizing(harness, ids, EmptyAllocation());
    result |= ExpectCommitted(harness,
                              harness.SubmitTerminalOutcome(MakeTerminalOutcome(
                                  ids.primary_job, TerminalOutcome::kSucceeded)),
                              EventType::kTerminalOutcomeCommitted);
    result |=
        Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kProcessExitConfirmation),
              "process-exit-confirmation generation one is retained before ordered pair");
    const auto exit = [&] {
      return harness.SubmitProcessExit(
          MakeProcessExit(ids.primary_job, ids, CompletionMode::kForced));
    };
    const auto pair =
        timeout_first
            ? QueueOrderedPair(
                  harness, [&] { return timer(harness, TimeoutPhase::kProcessExitConfirmation); },
                  exit)
            : QueueOrderedPair(harness, exit, [&] {
                return timer(harness, TimeoutPhase::kProcessExitConfirmation);
              });
    result |= CheckOrderedPair(
        pair, "exit-confirmation timeout and confirmed exit use explicit FIFO order");
    if (timeout_first) {
      committed_pair(harness, pair, process_exit_timeout, process_exit_after_timeout,
                     "exit-confirmation timeout-first pair has exact trace");
      drain_stops(harness, 1);
    } else {
      result |= ConsumeOrderedCommittedExact(harness, pair, true, process_exit_before_timeout,
                                             "confirmed exit has exact trace");
      result |= ConsumeOrderedRejection(harness, pair, false, RejectionReason::kInvariantViolation,
                                        process_exit_before_timeout);
    }
    result |= Check(Axes(harness, ids.primary_job, JobState::kSucceeded, std::nullopt, true,
                         CompletionMode::kForced, false) &&
                        harness.ack_authorization_count() == 1,
                    "exit-confirmation order retains terminal success and one ACK authorization");
    result |= Check(harness.VerifyFakes(), "exit-confirmation order drains exact fake candidates");
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
    result |= Check(harness.ArmPause(WriterPhase::kBeforeCommit) &&
                        harness.ArmBarrier(WriterPhase::kBeforeCommit),
                    "noncommitted logical result pauses at the keyed precommit barrier");
    harness.SetNextCommitResult(outcome);
    const auto terminal = harness.SubmitTerminalOutcome(
        MakeTerminalOutcome(ids.primary_job, TerminalOutcome::kSucceeded));
    result |= Check(harness.WaitUntil(terminal.ingress_sequence, WriterPhase::kBeforeCommit),
                    "terminal success reaches precommit before logical result");
    const auto before = CaptureEpoch(harness, ids.primary_job);
    const auto trace_size = harness.CopyTrace().size();
    const auto completions = harness.completion_count();
    const auto disposed = harness.disposed_count();
    const auto launches = harness.CopyLaunchRequests().size();
    const auto sessions = harness.CopySessionRequests().size();
    result |= Check(harness.Release(terminal.ingress_sequence, WriterPhase::kBeforeCommit),
                    "precommit barrier releases exactly the noncommitted logical turn");
    result |= ConsumeCompletion(harness, terminal, Completion::Code::kServiceFailed);
    const auto trace = harness.CopyTrace();
    const auto journal = harness.CopyJournalAttempts();
    const auto after = CaptureEpoch(harness, ids.primary_job);
    const auto expected_sequence =
        PositiveConfig().initial_journal_sequence + before.journal_attempts;
    result |= Check(after.journal_attempts == before.journal_attempts + 1 &&
                        after.ingress_sequences == before.ingress_sequences &&
                        terminal.ingress_sequence != 0 && !journal.empty() &&
                        journal.back().schema_version == 1 &&
                        journal.back().sequence == expected_sequence &&
                        journal.back().recorded_at.rfc3339 == kTimestamp &&
                        journal.back().job_id == ids.primary_job &&
                        journal.back().event_type == EventType::kTerminalOutcomeCommitted &&
                        PayloadEqual(journal.back().payload,
                                     TerminalOutcomePayload{TerminalOutcome::kSucceeded}),
                    "noncommitted result records one exact attempted and burned Journal envelope");
    result |= Check(before.snapshot && after.snapshot &&
                        SnapshotEqual(*before.snapshot, *after.snapshot) &&
                        after.applies == before.applies && after.effects == before.effects &&
                        after.responses == before.responses &&
                        after.acknowledgements == before.acknowledgements &&
                        harness.CopyLaunchRequests().size() == launches &&
                        harness.CopySessionRequests().size() == sessions &&
                        harness.NoForbiddenPostcommitActions() &&
                        harness.NoPostcommitAllocationOrCopy() && harness.VerifyFakes(),
                    "noncommitted result leaves snapshot and every postcommit axis unchanged");
    result |=
        Check(harness.disposed_count() == disposed + 1 &&
                  harness.completion_count() == completions + 1 && trace.size() == trace_size + 1 &&
                  trace.back() == TraceRecord{static_cast<std::uint64_t>(trace_size + 1),
                                              TraceKind::kJournalAttempt,
                                              expected_sequence,
                                              EventType::kTerminalOutcomeCommitted,
                                              EffectId::kInvalid,
                                              {},
                                              false,
                                              true} &&
                  harness.failed(),
              "noncommitted result has one exact attempt trace, disposal, and sticky completion");
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
  const auto terminal_snapshot = harness.Snapshot(ids.primary_job);
  result |= Check(terminal_snapshot.has_value(), "terminal commit exposes its monotonic baseline");
  result |= Check(harness.RetireWorkerAck(ids.primary_job, 1),
                  "ACK success retires Worker identity without an ingress sequence");
  result |= Check(harness.RetainTimerLease(ids.primary_job, TimeoutPhase::kProcessExitConfirmation),
                  "process-exit-confirmation timer lease is retained before process exit");
  result |= ExpectCommitted(
      harness, harness.SubmitWorker(MakeWorkerCompleted(ids.primary_job, ids.worker, 2)),
      EventType::kLateWorkerEvent);
  const auto after_late = harness.Snapshot(ids.primary_job);
  result |= Check(terminal_snapshot && after_late && SnapshotEqual(*terminal_snapshot, *after_late),
                  "late Worker ACK leaves every terminal snapshot axis unchanged");
  result |= ExpectCommitted(harness,
                            harness.SubmitProcessExit(MakeProcessExit(
                                ids.primary_job, ids, CompletionMode::kCooperative)),
                            EventType::kProcessExitConfirmed);
  const auto after_exit = harness.Snapshot(ids.primary_job);
  if (terminal_snapshot) {
    auto expected = *terminal_snapshot;
    expected.process_exit_confirmed = true;
    expected.process_presence = ProcessPresence::kAbsent;
    expected.completion_mode = CompletionMode::kCooperative;
    expected.pending_worker_event_ack = false;
    expected.pending_worker_id.reset();
    expected.pending_worker_event_sequence.reset();
    result |= Check(after_exit && SnapshotEqual(expected, *after_exit),
                    "process exit advances only process and pending-ACK terminal axes");
  }
  result |= ExpectCommitted(
      harness, harness.SubmitResourcesReleased(MakeResourcesReleased(ids.primary_job, allocation)),
      EventType::kResourcesReleased);
  const auto after_release = harness.Snapshot(ids.primary_job);
  if (after_exit) {
    auto expected = *after_exit;
    expected.resource_status = ResourceStatus::kReleased;
    result |= Check(after_release && SnapshotEqual(expected, *after_release),
                    "resource release advances only the terminal resource axis");
  }
  result |= ExpectCommitted(harness, harness.SubmitCleanup(MakeCleanupCompleted(ids.primary_job)),
                            EventType::kCleanupStatusRecorded);
  const auto after_cleanup = harness.Snapshot(ids.primary_job);
  if (after_release) {
    auto expected = *after_release;
    expected.cleanup_status = CleanupStatus::kCompleted;
    result |= Check(after_cleanup && SnapshotEqual(expected, *after_cleanup),
                    "cleanup advances only the monotonic terminal cleanup axis");
  }
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
