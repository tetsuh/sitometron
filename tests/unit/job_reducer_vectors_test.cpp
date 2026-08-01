#include "sitometron/core/job_reducer.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace sitometron::test {
namespace {
using Json = nlohmann::json;
using namespace sitometron::core;

int Check(bool condition, std::string_view message) {
  if (condition) return 0;
  std::cerr << "job_reducer_vectors: " << message << '\n';
  return 1;
}

}  // namespace

int RunJobReducerVectorChecks(const char* path) {
  std::ifstream input(path);
  if (!input) return Check(false, "normative vector artifact is readable");
  Json vectors;
  try { input >> vectors; } catch (const Json::exception&) { return Check(false, "normative vectors parse"); }
  int result = 0;
  result |= Check(vectors.at("contract_version") == 1, "contract version is consumed explicitly");
  const auto& cases = vectors.at("case_vectors");
  const auto& invalid = vectors.at("invalid_payload_vectors");
  const auto& sequences = vectors.at("sequence_vectors");
  const auto& timers = vectors.at("timer_ingress_vectors");
  result |= Check(cases.size() == 324, "all case vectors are present");
  result |= Check(invalid.size() == 39, "all invalid-payload vectors are present");
  result |= Check(sequences.size() == 14, "all sequence vectors are present");
  result |= Check(timers.size() == 4, "all timer-ingress vectors are present");
  std::size_t command_count = 0;
  std::size_t event_count = 0;
  for (const auto& vector : cases) {
    if (vector.at("matrix") == "command") ++command_count;
    if (vector.at("matrix") == "event") ++event_count;
    result |= Check(vector.contains("expected") && vector.at("expected").contains("disposition"),
                    "case vector has an expected disposition");
  }
  result |= Check(command_count == 24, "command selector covers every command case");
  result |= Check(event_count == 300, "event selector covers every event case");
  for (const auto& vector : invalid) {
    const auto& input = vector.at("input");
    const Uuid job{input.at("job_id").get<std::string>()};
    const RawCandidateEvent candidate{input.value("schema_version", 1), job,
                                      input.at("event_type").get<std::string>(), input.at("payload").dump()};
    const Snapshot snapshot = InitialSnapshot(job, job);
    const auto decision = NormalizeCandidate(snapshot, candidate);
    result |= Check(std::holds_alternative<Rejection>(decision.value), "invalid payload has no proposal");
    if (std::holds_alternative<Rejection>(decision.value)) {
      result |= Check(std::get<Rejection>(decision.value).reason == RejectionReason::kInvalidEventPayload,
                      "invalid payload uses the stable rejection reason");
    }
  }
  return result;
}
}  // namespace sitometron::test
