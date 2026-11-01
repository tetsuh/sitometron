#ifndef SITOMETRON_TESTS_SUPPORT_RETIREMENT_TEST_SUPPORT_HPP_
#define SITOMETRON_TESTS_SUPPORT_RETIREMENT_TEST_SUPPORT_HPP_

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core_fakes.hpp"
#include "job_orchestrator.hpp"

// Finite scripts and explicit inputs only. This fixture supplies no producer, clock, or
// retirement policy: every lifecycle state is reached through the unchanged reducer.
namespace sitometron::test::retirement {
namespace c = ::sitometron::core;
namespace i = ::sitometron::core::internal;

inline c::Uuid Job(unsigned number) {
  const auto suffix = std::to_string(number);
  return {"01890f3e-7b00-7abc-8abc-" + std::string(12 - suffix.size(), '0') + suffix};
}
inline c::Uuid Worker(unsigned number) {
  const auto suffix = std::to_string(number);
  return {"550e8400-e29b-41d4-a716-" + std::string(12 - suffix.size(), '0') + suffix};
}
inline c::StableId Launch(unsigned number) {
  return {"retirement-launch-" + std::to_string(number)};
}
inline c::RawCandidateEvent Event(unsigned number, std::string type, std::string payload = "{}") {
  return {1, Job(number), std::move(type), std::move(payload)};
}
inline c::RawCandidateEvent Create(unsigned number) {
  return Event(number, "job_created", "{\"session_id\":\"" + Job(number).value + "\"}");
}
inline c::Command Cancel(unsigned number) {
  return {1, c::CommandType::kCancel, Job(number), "retirement-test"};
}
inline c::Command Terminate(unsigned number) {
  return {1, c::CommandType::kTerminate, Job(number), "retirement-test"};
}
inline c::RawCandidateEvent Cleanup(unsigned number, bool incomplete = false) {
  return Event(number, "cleanup_status_recorded",
               incomplete ? R"({"status":"incomplete"})" : R"({"status":"completed"})");
}
inline c::RawCandidateEvent Terminal(unsigned number, std::string outcome) {
  return Event(number, "terminal_outcome_committed", "{\"outcome\":\"" + outcome + "\"}");
}
inline c::RawCandidateEvent WorkerFact(unsigned number, std::uint64_t sequence = 1,
                                       bool failed = false) {
  return Event(number, failed ? "worker_failed" : "worker_completed",
               "{\"worker_id\":\"" + Worker(number).value +
                   "\",\"event_sequence\":" + std::to_string(sequence) + "}");
}
inline c::RawCandidateEvent Exit(unsigned number) {
  return Event(number, "process_exit_confirmed",
               "{\"completion_mode\":\"cooperative\",\"launch_operation_id\":\"" +
                   Launch(number).value + "\"}");
}
inline c::RawCandidateEvent Release(unsigned number) {
  return Event(
      number, "resources_released",
      R"({"allocation_id":"allocation-1","allocation_digest":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"})");
}

using Input = std::variant<c::RawCandidateEvent, c::Command>;
inline i::IngressResult Submit(i::JobOrchestrator& writer, const Input& input) {
  if (const auto* candidate = std::get_if<c::RawCandidateEvent>(&input))
    return writer.SubmitCandidate(*candidate);
  return writer.SubmitCommand(std::get<c::Command>(input));
}
inline bool Success(i::JobOrchestrator& writer, const i::IngressResult& input) {
  if (input.code != i::IngressCode::kAdmitted) return false;
  const auto result = writer.AwaitCompletion(input.ingress_sequence);
  return result && result->code == i::Completion::Code::kSuccess;
}
inline bool Rejected(i::JobOrchestrator& writer, const i::IngressResult& input,
                     c::RejectionReason reason) {
  if (input.code != i::IngressCode::kAdmitted) return false;
  const auto result = writer.AwaitCompletion(input.ingress_sequence);
  return result && result->code == i::Completion::Code::kReducerRejection && result->rejection &&
         result->rejection->reason == reason;
}
inline bool CapacityCompletion(i::JobOrchestrator& writer, const i::IngressResult& input) {
  if (input.code != i::IngressCode::kAdmitted) return false;
  const auto result = writer.AwaitCompletion(input.ingress_sequence);
  // Detect the new completion enumerator without requiring a production edit in the RED commit.
  return result && []<typename Code>(Code code) {
    if constexpr (requires { Code::kResidentLimit; }) return code == Code::kResidentLimit;
    return false;
  }(result->code);
}

struct Plan {
  std::vector<Input> inputs;
  std::vector<JournalExpectation> journal;
  std::vector<RunnerExpectation> runner;
  std::vector<SessionExpectation> session;
  std::vector<c::Snapshot> snapshots;
  std::vector<std::size_t> trace_counts;

  Plan& Add(Input input) {
    const auto id = std::visit([](const auto& value) { return value.job_id; }, input);
    auto found =
        std::ranges::find_if(snapshots, [&](const auto& value) { return value.job_id == id; });
    const auto before = found == snapshots.end() ? c::InitialSnapshot(id, id) : *found;
    const auto decision = std::visit(
        [&](const auto& value) -> c::Decision {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, c::Command>) {
            return c::DecideCommand(before, value);
          } else {
            const auto normalized = c::NormalizeCandidate(before, value);
            if (const auto* rejected = std::get_if<c::Rejection>(&normalized.value))
              return c::Decision{*rejected};
            return c::DecideEvent(before, std::get<c::InternalEvent>(normalized.value));
          }
        },
        input);
    const auto* proposal = std::get_if<c::PreEnvelopeProposal>(&decision.value);
    if (!proposal)
      throw std::runtime_error("retirement fixture planned an unreachable accepted input");
    const auto applied = c::Apply(before, *proposal);
    if (applied.rejection) throw std::runtime_error("retirement fixture apply rejected its plan");
    const auto type = proposal->event_type;
    const std::size_t sources =
        type == c::EventType::kResourcesCommitted || type == c::EventType::kWorkerLaunchIntent ||
                type == c::EventType::kTerminalOutcomeCommitted
            ? 2
        : type == c::EventType::kCancelAccepted || type == c::EventType::kTerminateAccepted ? 1
                                                                                            : 0;
    trace_counts.push_back(4 + sources + 2 * applied.effects.size());
    journal.push_back({{1,
                        static_cast<std::uint64_t>(journal.size() + 1),
                        proposal->event_type,
                        {"2026-11-01T00:00:00Z"},
                        id,
                        proposal->payload},
                       c::LogicalCommitResult::kCommitted,
                       false});
    for (const auto effect : applied.effects) {
      if (effect.id == c::EffectId::kRetainSessionSameIdentity)
        session.push_back({{id, applied.snapshot.session_id}, std::nullopt});
      if (effect.id == c::EffectId::kLaunchWorkerOnce)
        runner.push_back({RunnerCallKind::kLaunch,
                          c::ApplicationLaunchRequest{
                              id, std::get<c::WorkerLaunchIntentPayload>(proposal->payload)},
                          std::nullopt});
      if (effect.id == c::EffectId::kRequestCooperativeStop ||
          effect.id == c::EffectId::kRequestForcedStop)
        runner.push_back({effect.id == c::EffectId::kRequestCooperativeStop
                              ? RunnerCallKind::kCooperativeStop
                              : RunnerCallKind::kForcedStop,
                          c::ApplicationStopRequest{id, *applied.snapshot.launch_operation_id,
                                                    *applied.snapshot.worker_id},
                          std::nullopt});
    }
    if (found == snapshots.end())
      snapshots.push_back(applied.snapshot);
    else
      *found = applied.snapshot;
    inputs.push_back(std::move(input));
    return *this;
  }
  // An explicit expected retirement in the test scenario, not an alternate selection algorithm.
  // Needed only to script raw same-ID recreation, which bypasses the production identity source.
  Plan& AfterRetirementOf(unsigned number) {
    std::erase_if(snapshots, [&](const auto& snapshot) { return snapshot.job_id == Job(number); });
    return *this;
  }
  Plan& Finalize(unsigned number, std::string outcome = "cancelled", bool failed = false) {
    if (failed) return Add(Event(number, "finalization_failed")).Add(Terminal(number, outcome));
    const auto session_json = "{\"session_id\":\"" + Job(number).value + "\"}";
    return Add(Event(number, "session_retain_requested", session_json))
        .Add(Event(number, "session_retained", session_json))
        .Add(Event(number, "finalization_completed"))
        .Add(Terminal(number, outcome));
  }
  Plan& Close(unsigned number, bool failed_finalization = false, bool terminate = false) {
    Add(Create(number));
    Add(terminate ? Input(Terminate(number)) : Input(Cancel(number)));
    Finalize(number, terminate ? "terminated" : "cancelled", failed_finalization);
    // Owner reading: reducer-derived incomplete cleanup closes too, without a cleanup record.
    // https://github.com/tetsuh/sitometron/issues/120#issuecomment-6554697941
    if (!failed_finalization) Add(Cleanup(number));
    return *this;
  }
  Plan& Running(unsigned number, bool failed_launch = false) {
    Add(Create(number));
    Add(Event(
        number, "resources_committed",
        R"({"allocation_id":"allocation-1","allocation_digest":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a","resolved_allocation":{"schema_id":"allocation.v1","schema_version":1,"payload_utf8":"{}"}})"));
    Add(Event(number, "worker_launch_intent",
              "{\"operation_id\":\"" + Launch(number).value +
                  "\",\"application\":{\"application_id\":\"retirement-test\",\"version\":\"1\","
                  "\"bundle_sha256\":\"" +
                  std::string(64, 'a') +
                  "\"},\"allocation_id\":\"allocation-1\",\"allocation_digest\":"
                  "\"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a\",\"worker_"
                  "id\":\"" +
                  Worker(number).value + "\"}"));
    Add(Event(number, "worker_launch_observed",
              "{\"operation_id\":\"" + Launch(number).value + "\",\"outcome\":\"" +
                  (failed_launch ? "failed" : "started") + "\"}"));
    if (!failed_launch)
      Add(Event(number, "worker_running", "{\"worker_id\":\"" + Worker(number).value + "\"}"));
    return *this;
  }
  Plan& CloseWorker(unsigned number, bool failed_launch = false) {
    Running(number, failed_launch);
    if (!failed_launch) Add(WorkerFact(number));
    Finalize(number, failed_launch ? "failed" : "succeeded");
    if (!failed_launch) Add(Exit(number));
    return Add(Release(number)).Add(Cleanup(number));
  }
};

inline std::vector<c::JobSessionIdentityResult> Jobs() {
  std::vector<c::JobSessionIdentityResult> values;
  for (unsigned n = 1; n <= 128; ++n) values.emplace_back(c::GeneratedJobSessionIdentity{Job(n)});
  return values;
}
inline std::vector<c::WorkerIdentityResult> Workers() {
  std::vector<c::WorkerIdentityResult> values;
  for (unsigned n = 1; n <= 128; ++n) values.emplace_back(c::GeneratedWorkerIdentity{Worker(n)});
  return values;
}
inline std::vector<c::LaunchOperationIdentityResult> Launches() {
  std::vector<c::LaunchOperationIdentityResult> values;
  for (unsigned n = 1; n <= 128; ++n)
    values.emplace_back(c::GeneratedLaunchOperationIdentity{Launch(n)});
  return values;
}

class Fixture {
 public:
  explicit Fixture(const Plan& plan, std::size_t jobs = 2, std::size_t normal = 8,
                   std::size_t trace = 1024, c::JobJournalPort* external_journal = nullptr)
      : journal(plan.journal),
        runner(plan.runner, 1),
        session(plan.session, 1),
        identity(Jobs(), Workers(), Launches()) {
    config.max_jobs = jobs;
    config.normal_capacity = normal;
    config.trace_capacity = trace;
    config.completion_capacity = std::max(config.total_capacity(), plan.inputs.size() + 16);
    config.handoff_capacity = 4;
    config.ack_capacity = std::max<std::size_t>(jobs, 4);
    config.callback_registration_capacity = 16;
    config.ports = {&clock, external_journal ? external_journal : &journal, &runner, &session,
                    &identity};
    writer = std::make_unique<i::JobOrchestrator>(config);
  }
  bool Run(const Plan& plan, std::size_t from, std::size_t until) {
    for (auto n = from; n < until; ++n)
      if (!Success(*writer, Submit(*writer, plan.inputs.at(n)))) return false;
    return true;
  }
  bool Run(const Plan& plan) { return Run(plan, 0, plan.inputs.size()); }
  bool Verify() {
    return !clock.verification_failed() && journal.Verify() && runner.Verify() &&
           session.Verify() && !identity.verification_failed();
  }
  FakeClock clock{{{"2026-11-01T00:00:00Z"}, {0}}};
  FakeJobJournal journal;
  FakeApplicationRunner runner;
  FakeSessionRetainer session;
  FakeIdentitySource identity;
  i::Config config;
  std::unique_ptr<i::JobOrchestrator> writer;
};

// RED bridges specify private observation/injection seams without changing production sources.
// Missing probes never count as successful coverage. All retirement checks first require real
// pressure admission, so the baseline failure is behavior, not an unavailable test interface.
struct SlotState {
  std::size_t index = 0;
  std::array<std::uint64_t, 4> timer_generation{};
  bool state_banks_empty = false;
  bool prepared_banks_empty = false;
  bool gates_empty = false;
  bool identities_empty = false;
  bool generated_bindings_empty = false;
};
template <typename Writer, typename Key>
std::optional<SlotState> InspectSlot(Writer& writer, const Key& key) {
  if constexpr (requires { writer.InspectSlotForTest(key); }) {
    const auto value = writer.InspectSlotForTest(key);
    if (!value) return std::nullopt;
    return SlotState{value->index,
                     value->timer_generation,
                     value->state_banks_empty,
                     value->prepared_banks_empty,
                     value->gates_empty,
                     value->identities_empty,
                     value->generated_bindings_empty};
  }
  return std::nullopt;
}
template <typename Writer>
bool InjectGeneration(Writer& writer, const c::Uuid& id, c::TimeoutPhase phase,
                      std::uint64_t value) {
  if constexpr (requires { writer.SetTimerGenerationForTest(id, phase, value); })
    return writer.SetTimerGenerationForTest(id, phase, value);
  return false;
}
template <typename Writer>
bool InjectOrdinal(Writer& writer, std::uint64_t value) {
  if constexpr (requires { writer.SetNextTraceOrdinalForTest(value); })
    return writer.SetNextTraceOrdinalForTest(value);
  return false;
}
template <typename Writer>
bool AcquireAndReturn(Writer& writer, const c::Uuid& id, c::EventType event,
                      c::TimeoutPhase phase = c::TimeoutPhase::kInvalid) {
  if constexpr (requires { writer.AcquireSourceLease(id, event, phase); }) {
    auto lease = writer.AcquireSourceLease(id, event, phase);
    if (!lease) return false;
    return writer.ReturnSourceLease(std::move(*lease));
  }
  return false;
}

template <typename Phase>
std::optional<Phase> ClaimedPhase() {
  if constexpr (requires { Phase::kVictimClaimed; }) return Phase::kVictimClaimed;
  return std::nullopt;
}
template <typename Writer, typename Action>
bool WhileLeased(Writer& writer, const c::Uuid& id, c::EventType event, Action action) {
  if constexpr (requires { writer.AcquireSourceLease(id, event, c::TimeoutPhase::kInvalid); }) {
    auto lease = writer.AcquireSourceLease(id, event, c::TimeoutPhase::kInvalid);
    if (!lease) return false;
    const bool observed = action();
    const bool returned = writer.ReturnSourceLease(std::move(*lease));
    return observed && returned;
  }
  return false;
}

// Always release a keyed barrier during unwinding, including the expected RED admission failure.
class Hold {
 public:
  Hold(i::JobOrchestrator& value, i::WriterPhase phase) : writer_(value), phase_(phase) {
    writer_.ArmBarrier(phase_);
  }
  Hold(const Hold&) = delete;
  Hold& operator=(const Hold&) = delete;
  ~Hold() { writer_.Release(sequence_, phase_); }
  bool Wait(const i::IngressResult& input) {
    if (input.code != i::IngressCode::kAdmitted) return false;
    sequence_ = input.ingress_sequence;
    return writer_.WaitUntil(sequence_, phase_);
  }
  void Release() { writer_.Release(sequence_, phase_); }

 private:
  i::JobOrchestrator& writer_;
  i::WriterPhase phase_;
  std::uint64_t sequence_ = 0;
};

}  // namespace sitometron::test::retirement
#endif  // SITOMETRON_TESTS_SUPPORT_RETIREMENT_TEST_SUPPORT_HPP_
