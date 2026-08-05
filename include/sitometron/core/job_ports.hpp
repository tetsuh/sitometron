#ifndef SITOMETRON_CORE_JOB_PORTS_HPP_
#define SITOMETRON_CORE_JOB_PORTS_HPP_

#include <cstdint>
#include <string>
#include <variant>

#include "sitometron/core/job_reducer.hpp"

namespace sitometron::core {

struct DiagnosticTimestamp {
  std::string rfc3339;
  friend bool operator==(const DiagnosticTimestamp&, const DiagnosticTimestamp&) = default;
};

struct MonotonicInstant {
  std::uint64_t nanoseconds_since_origin = 0;
  friend bool operator==(const MonotonicInstant&, const MonotonicInstant&) = default;
};

struct ClockReading {
  DiagnosticTimestamp recorded_at;
  MonotonicInstant monotonic_time;
  friend bool operator==(const ClockReading&, const ClockReading&) = default;
};

struct LogicalJobEvent {
  std::uint32_t schema_version = 1;
  std::uint64_t sequence = 0;
  EventType event_type = EventType::kInvalid;
  DiagnosticTimestamp recorded_at;
  Uuid job_id;
  EventPayload payload;
};

enum class LogicalCommitResult { kCommitted, kDefiniteFailure, kOutcomeUnknown };

struct ApplicationLaunchRequest {
  Uuid job_id;
  WorkerLaunchIntentPayload intent;
};

struct ApplicationStopRequest {
  Uuid job_id;
  StableId launch_operation_id;
  Uuid worker_id;
  friend bool operator==(const ApplicationStopRequest&, const ApplicationStopRequest&) = default;
};

struct SessionRetainRequest {
  Uuid job_id;
  Uuid session_id;
  friend bool operator==(const SessionRetainRequest&, const SessionRetainRequest&) = default;
};

struct IdentitySourceExhausted {
  friend bool operator==(IdentitySourceExhausted, IdentitySourceExhausted) = default;
};
struct GeneratedJobSessionIdentity {
  Uuid value;
  friend bool operator==(const GeneratedJobSessionIdentity&,
                         const GeneratedJobSessionIdentity&) = default;
};
struct GeneratedWorkerIdentity {
  Uuid value;
  friend bool operator==(const GeneratedWorkerIdentity&, const GeneratedWorkerIdentity&) = default;
};
struct GeneratedLaunchOperationIdentity {
  StableId value;
  friend bool operator==(const GeneratedLaunchOperationIdentity&,
                         const GeneratedLaunchOperationIdentity&) = default;
};

using JobSessionIdentityResult = std::variant<GeneratedJobSessionIdentity, IdentitySourceExhausted>;
using WorkerIdentityResult = std::variant<GeneratedWorkerIdentity, IdentitySourceExhausted>;
using LaunchOperationIdentityResult =
    std::variant<GeneratedLaunchOperationIdentity, IdentitySourceExhausted>;

class ClockPort {
 public:
  virtual ~ClockPort() = default;
  [[nodiscard]] virtual ClockReading Read() = 0;
};

class JobJournalPort {
 public:
  virtual ~JobJournalPort() = default;
  [[nodiscard]] virtual LogicalCommitResult Commit(const LogicalJobEvent& event) noexcept = 0;
};

class ApplicationRunnerPort {
 public:
  virtual ~ApplicationRunnerPort() = default;
  virtual void HandoffLaunch(ApplicationLaunchRequest&& request) noexcept = 0;
  virtual void HandoffCooperativeStop(ApplicationStopRequest&& request) noexcept = 0;
  virtual void HandoffForcedStop(ApplicationStopRequest&& request) noexcept = 0;
};

class SessionRetainerPort {
 public:
  virtual ~SessionRetainerPort() = default;
  virtual void HandoffRetainSameIdentity(SessionRetainRequest&& request) noexcept = 0;
};

class IdentitySourcePort {
 public:
  virtual ~IdentitySourcePort() = default;
  [[nodiscard]] virtual JobSessionIdentityResult GenerateJobSessionIdentity() = 0;
  [[nodiscard]] virtual WorkerIdentityResult GenerateWorkerIdentity() = 0;
  [[nodiscard]] virtual LaunchOperationIdentityResult GenerateLaunchOperationIdentity() = 0;
};

}  // namespace sitometron::core

#endif  // SITOMETRON_CORE_JOB_PORTS_HPP_
