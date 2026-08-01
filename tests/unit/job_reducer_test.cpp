#include "sitometron/core/job_reducer.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace sitometron::test {
int RunJobReducerVectorChecks(const char* path, const char* selector);
}

namespace {
using namespace sitometron::core;
constexpr std::string_view kJob = "01890f3e-7b00-7abc-8abc-0123456789ab";
constexpr std::string_view kWorker = "123e4567-e89b-42d3-a456-426614174000";

int Check(bool condition, std::string_view message) {
  if (condition) return 0;
  std::cerr << "job_reducer: " << message << '\n';
  return 1;
}

int Run() {
  const Uuid job{std::string(kJob)};
  Snapshot absent = InitialSnapshot(job, job);
  int result = 0;
  result |= Check(!absent.entity_exists, "initial entity position is absent");
  const RawCandidateEvent created{1, job, "job_created", "{\"session_id\":\"" + std::string(kJob) + "\"}"};
  const auto normalized_created = NormalizeCandidate(absent, created);
  const Decision created_decision = std::holds_alternative<InternalEvent>(normalized_created.value)
      ? DecideEvent(absent, std::get<InternalEvent>(normalized_created.value))
      : Decision{std::get<Rejection>(normalized_created.value)};
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(created_decision.value), "job_created validates");
  if (std::holds_alternative<PreEnvelopeProposal>(created_decision.value)) {
    const auto applied = Apply(absent, std::get<PreEnvelopeProposal>(created_decision.value));
    result |= Check(applied.snapshot.state == JobState::kAdmitted, "job_created applies admitted");
    absent = applied.snapshot;
  }
  const RawCandidateEvent allocation{1, job, "resources_committed",
      "{\"allocation_id\":\"allocation-1\",\"allocation_digest\":\"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a\",\"resolved_allocation\":{\"schema_id\":\"allocation.v1\",\"schema_version\":1,\"payload_utf8\":\"{}\"}}"};
  const auto normalized_allocation = NormalizeCandidate(absent, allocation);
  const auto allocation_decision = std::holds_alternative<InternalEvent>(normalized_allocation.value)
      ? DecideEvent(absent, std::get<InternalEvent>(normalized_allocation.value))
      : Decision{std::get<Rejection>(normalized_allocation.value)};
  result |= Check(std::holds_alternative<PreEnvelopeProposal>(allocation_decision.value), "allocation validates byte-exactly");
  if (std::holds_alternative<PreEnvelopeProposal>(allocation_decision.value)) {
    absent = Apply(absent, std::get<PreEnvelopeProposal>(allocation_decision.value)).snapshot;
    result |= Check(absent.state == JobState::kPreparing, "allocation enters preparing");
  }
  const Command cancel{1, CommandType::kCancel, job, "operator@example"};
  result |= Check(std::holds_alternative<Rejection>(DecideCommand(absent, cancel).value), "cancel rejected before worker");
  const RawCandidateEvent invalid{1, job, "job_created", "{\"unexpected\":true}"};
  const auto invalid_result = NormalizeCandidate(absent, invalid);
  result |= Check(std::holds_alternative<Rejection>(invalid_result.value) &&
                      std::get<Rejection>(invalid_result.value).reason == RejectionReason::kInvalidEventPayload,
                  "unknown payload is rejected before matrix selection");

  TimerState timer;
  timer.preparation_armed = true;
  timer.preparation_generation = 9;
  const auto timer_result = IngestTimer(timer, job, TimeoutPhase::kPreparation, 9);
  result |= Check(timer_result.kind == TimerIngressKind::kEmitCandidateEvent && timer_result.candidate.has_value(),
                  "current timer emits one candidate");
  result |= Check(IngestTimer(timer, job, TimeoutPhase::kPreparation, 8).kind == TimerIngressKind::kDiscardWithoutCandidate,
                  "stale timer is discarded");
  timer.preparation_generation = std::numeric_limits<std::uint64_t>::max();
  result |= Check(IngestTimer(timer, job, TimeoutPhase::kPreparation, 9).kind == TimerIngressKind::kFailClosed,
                  "generation exhaustion fails closed");
  (void)kWorker;
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  int result = Run();
  if (argc > 1) result |= sitometron::test::RunJobReducerVectorChecks(argv[1], argc > 2 ? argv[2] : "all");
  return result;
}
