#ifndef SITOMETRON_TEST_PROCESS_RUNNER_TEST_SUPPORT_HPP_
#define SITOMETRON_TEST_PROCESS_RUNNER_TEST_SUPPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::test::process {

// Test-owned facade. The native driver binds the real adapter when its target exists;
// absence is a failing contract precondition, never an emulated successful runner.
enum class Admission {
  kAdmitted,
  kCoalescedPending,
  kAlreadyPending,
  kNormalFull,
  kResidentLimit,
  kAdmissionClosed,
  kServiceFailed
};
enum class Completion { kSuccess, kRejected, kServiceFailed, kMissing };
struct Answer {
  Admission admission = Admission::kAdmitted;
  Completion completion = Completion::kSuccess;
  core::RejectionReason rejection = core::RejectionReason::kEventNotAllowedInState;
};
struct Specification {
  std::string application_id;
  std::string executable;
  std::vector<std::string> arguments;
  std::string bundle_sha256;
};
struct Attempt {
  core::RawCandidateEvent event;
  Answer answer;
  std::thread::id thread;
};
struct Diagnostics {
  std::size_t occupied = 0;
  std::size_t closing = 0;
  std::size_t threads_created = 0;
  std::size_t threads_joined = 0;
  std::size_t processes_created = 0;
  std::size_t processes_reaped = 0;
  std::size_t cooperative_deliveries = 0;
  std::size_t forced_deliveries = 0;
  std::size_t handoff_allocations = 0;
  std::size_t handoff_locks = 0;
  std::size_t handoff_ingress = 0;
  std::size_t max_completions_per_thread = 0;
  std::size_t invariant_failures = 0;
  std::vector<std::string> delivery_order;
};
struct Options {
  std::size_t max_jobs = 1;
  std::vector<Specification> specifications;
  std::vector<Answer> observations;
  std::vector<Answer> exits;
  std::string hold_at;
};
class Driver {
 public:
  virtual ~Driver() = default;
  virtual void Launch(core::ApplicationLaunchRequest) = 0;
  virtual void Cooperative(core::ApplicationStopRequest) = 0;
  virtual void Forced(core::ApplicationStopRequest) = 0;
  virtual void WaitHeld(std::size_t count = 1) = 0;
  virtual void Release() noexcept = 0;
  virtual void WaitPoint(std::string_view point, std::size_t count = 1) = 0;
  virtual void Capacity(std::size_t waiter) = 0;
  virtual void CloseAdmission() = 0;
  virtual void RequestStop() noexcept = 0;
  virtual void Join() noexcept = 0;
  virtual void WaitFacts(std::size_t observations, std::size_t exits) = 0;
  virtual std::vector<Attempt> Attempts() = 0;
  virtual Diagnostics Inspect() = 0;
};

std::unique_ptr<Driver> MakeDriver(const Options&);
std::string QuoteWindowsArgument(std::string_view);

}  // namespace sitometron::test::process
#endif
