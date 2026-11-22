#ifndef SITOMETRON_PROCESS_RUNNER_HPP_
#define SITOMETRON_PROCESS_RUNNER_HPP_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/core/job_reducer.hpp"

namespace sitometron::process {
namespace internal {
struct PortState;
struct ProducerState;
struct TestAccess;
}  // namespace internal

struct LaunchSpecification {
  std::string application_id;
  std::string executable;
  std::vector<std::string> arguments;
  std::string bundle_sha256;
};
enum class AdmissionCode {
  kAdmitted,
  kCoalescedPending,
  kAlreadyPending,
  kNormalFull,
  kResidentLimit,
  kAdmissionClosed,
  kServiceFailed
};
enum class CompletionCode { kSuccess, kRejected, kServiceFailed };
struct CompletionResult {
  CompletionCode code = CompletionCode::kServiceFailed;
  std::optional<core::RejectionReason> rejection;
};
struct SubmissionResult {
  AdmissionCode admission = AdmissionCode::kServiceFailed;
  std::optional<CompletionResult> completion;
};
// One registration per submitting thread; Submit includes taking that exact completion.
class IngressHandle {
 public:
  virtual ~IngressHandle() = default;
  virtual SubmissionResult Submit(core::RawCandidateEvent event) noexcept = 0;
};
class CapacityWaiter {
 public:
  virtual ~CapacityWaiter() = default;
  virtual void Clear() noexcept = 0;
  virtual void Set() noexcept = 0;
  virtual bool Wait() noexcept = 0;
  virtual bool Closed() const noexcept = 0;
};
class FailureLatch {
 public:
  virtual ~FailureLatch() = default;
  virtual void Latch(std::string_view diagnostic) noexcept = 0;
};
class RunnerPort final : public core::ApplicationRunnerPort {
 public:
  explicit RunnerPort(std::size_t max_jobs);
  ~RunnerPort() override;
  RunnerPort(const RunnerPort&) = delete;
  RunnerPort& operator=(const RunnerPort&) = delete;
  void HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept override;
  void HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept override;
  void HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept override;

 private:
  std::unique_ptr<internal::PortState> state_;
  friend class RunnerProducer;
  friend struct internal::TestAccess;
};
class RunnerProducer final {
 public:
  // Handles: slots, service, relay. Waiters: slots, relay. All resources precede the listener.
  RunnerProducer(RunnerPort&, std::vector<LaunchSpecification>,
                 std::vector<std::unique_ptr<IngressHandle>>,
                 std::vector<std::shared_ptr<CapacityWaiter>>, FailureLatch&);
  ~RunnerProducer();
  RunnerProducer(const RunnerProducer&) = delete;
  RunnerProducer& operator=(const RunnerProducer&) = delete;
  void RequestStop() noexcept;
  void Join() noexcept;

 private:
  std::unique_ptr<internal::ProducerState> state_;
};
}  // namespace sitometron::process
#endif  // SITOMETRON_PROCESS_RUNNER_HPP_
