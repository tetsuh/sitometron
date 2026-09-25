#include "system_ports.hpp"

#include <ctime>

namespace sitometron::spike {

SystemClock::SystemClock() : origin_(std::chrono::steady_clock::now()) {}

std::string SystemClock::Rfc3339Now() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[40];
  const auto length = std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S", &utc);
  std::string out(buffer, length);
  char fraction[8];
  std::snprintf(fraction, sizeof fraction, ".%03lldZ", static_cast<long long>(millis));
  return out + fraction;
}

core::ClockReading SystemClock::Read() {
  const auto elapsed = std::chrono::steady_clock::now() - origin_;
  return core::ClockReading{
      core::DiagnosticTimestamp{Rfc3339Now()},
      core::MonotonicInstant{static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())}};
}

core::JobSessionIdentityResult LocalIdentitySource::GenerateJobSessionIdentity() {
  return core::GeneratedJobSessionIdentity{core::Uuid{uuids_.V7()}};
}

core::WorkerIdentityResult LocalIdentitySource::GenerateWorkerIdentity() {
  return core::GeneratedWorkerIdentity{core::Uuid{uuids_.V4()}};
}

core::LaunchOperationIdentityResult LocalIdentitySource::GenerateLaunchOperationIdentity() {
  return core::GeneratedLaunchOperationIdentity{
      core::StableId{"launch-" + std::to_string(++launch_counter_)}};
}

}  // namespace sitometron::spike
