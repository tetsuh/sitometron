#ifndef SITOMETRON_CORE_JOB_SUPERVISOR_HPP_
#define SITOMETRON_CORE_JOB_SUPERVISOR_HPP_

#include <string>
#include <string_view>
#include <vector>

#include "sitometron/core/job_reducer.hpp"

namespace sitometron::core::internal {
// Owned creation inputs; no registry or dependency-owned type crosses this boundary.
struct JobLaunchInputs {
  StableId application_id;
  std::string application_version;
  Digest bundle_sha256;
};
inline constexpr std::string_view kEmptyAllocationId = "empty-allocation";
inline constexpr std::string_view kEmptyAllocationDigest =
    "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
struct SupervisorDecision {
  int rule = 1;
  std::string_view kind = "Done";
  std::string_view event;
  std::string_view payload_rule = "none";
  std::vector<std::string_view> sources;
};
[[nodiscard]] SupervisorDecision DecideSupervisor(const Snapshot*, bool launch_inputs_present);
}  // namespace sitometron::core::internal
#endif
