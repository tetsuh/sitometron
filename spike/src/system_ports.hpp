#ifndef SITOMETRON_SPIKE_SYSTEM_PORTS_HPP_
#define SITOMETRON_SPIKE_SYSTEM_PORTS_HPP_

#include <chrono>
#include <string>

#include "sitometron/core/job_ports.hpp"
#include "uuid.hpp"

namespace sitometron::spike {

// Wall clock for the diagnostic timestamp, steady clock for the monotonic instant.
class SystemClock final : public core::ClockPort {
 public:
  SystemClock();
  [[nodiscard]] core::ClockReading Read() override;

  static std::string Rfc3339Now();

 private:
  std::chrono::steady_clock::time_point origin_;
};

// Controller-issued identities: UUIDv7 for Job/Session, UUIDv4 for Worker, and a readable
// stable id for the launch operation.
class LocalIdentitySource final : public core::IdentitySourcePort {
 public:
  [[nodiscard]] core::JobSessionIdentityResult GenerateJobSessionIdentity() override;
  [[nodiscard]] core::WorkerIdentityResult GenerateWorkerIdentity() override;
  [[nodiscard]] core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override;

 private:
  UuidGenerator uuids_;
  std::uint64_t launch_counter_ = 0;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_SYSTEM_PORTS_HPP_
