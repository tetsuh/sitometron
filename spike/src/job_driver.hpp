#ifndef SITOMETRON_SPIKE_JOB_DRIVER_HPP_
#define SITOMETRON_SPIKE_JOB_DRIVER_HPP_

#include <sys/types.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "job_orchestrator.hpp"
#include "process_runner.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/segment_journal.hpp"
#include "system_ports.hpp"

namespace sitometron::spike {

struct DriverConfig {
  std::size_t max_jobs = 32;          // resident slots: also the lifetime Job count (see README)
  std::size_t trace_capacity = 4096;  // writer trace + ingress log bound (see README)
  std::string working_directory;
  // The principal recorded in cancel_accepted (ADR-0008 Section 3): one configured name.
  std::string cancel_principal = "local-operator";
  // Replayed at startup (ADR-0006 Section 6): snapshots in creation order, and the unresolved Jobs
  // that keep admission closed for this run.
  std::vector<core::Snapshot> replayed_jobs;
  std::vector<core::Uuid> unresolved_jobs;
};

struct JobRecord {
  std::string job_id;
  LaunchSpec spec;
  std::string created_at;
  std::optional<std::string> worker_id;
  std::optional<std::string> launch_operation_id;
  pid_t pid = -1;
  std::optional<ExitStatus> exit;
  std::string error;               // first driver-side failure, empty on success
  std::vector<std::string> steps;  // committed event types in order, for the demo
  bool recovered = false;          // replayed from the Journal of a previous run
};

// Why a creation was refused, in the order ADR-0003 and ADR-0008 Section 5 give the causes.
enum class CreateRefusal { kNone, kServiceFailed, kNotReady, kCapacityExhausted, kBusy };

struct CreateOutcome {
  CreateRefusal refusal = CreateRefusal::kServiceFailed;
  std::string job_id;  // set when refusal is kNone
  std::string detail;  // diagnostic text for the daemon log and the unversioned route only
};

// The result of a cancel command, in the order ADR-0008 Sections 3 and 5 give the causes.
enum class CancelOutcome {
  kAccepted,
  kServiceFailed,
  kNotReady,
  kBusy,
  kJobNotFound,
  kStopCauseAlreadyLatched,
  kCommandNotAllowedInState
};

// The composition root's Job sequencer. One thread per Job drives the lifecycle candidates that
// the core leaves to "the supervisor": resources, launch intent, session retention, finalization,
// terminal outcome, process-exit confirmation, release, and cleanup. Every step submits one raw
// candidate, waits for the writer, and inspects the completion.
class JobDriver {
 public:
  JobDriver(DriverConfig config, journal::SegmentJournal& journal);
  ~JobDriver();
  JobDriver(const JobDriver&) = delete;
  JobDriver& operator=(const JobDriver&) = delete;

  // Creates the Job (job_created committed) and starts its driver thread.
  [[nodiscard]] std::optional<std::string> Submit(LaunchSpec spec, std::string& error);
  // The same creation, reporting why it was refused instead of a text.
  [[nodiscard]] CreateOutcome Create(LaunchSpec spec);
  // Submits the ADR-0002 cancel command and waits until it is applied or refused.
  [[nodiscard]] CancelOutcome Cancel(const std::string& job_id);
  // The External REST v1 Job resource (ADR-0008 Section 3) from the committed snapshot; null for a
  // Job that is not resident.
  [[nodiscard]] nlohmann::json Resource(const std::string& job_id) const;
  // Every resident Job's resource, in creation order.
  [[nodiscard]] nlohmann::json Resources() const;
  // The readiness reasons of ADR-0008 Section 6; empty when the daemon accepts new Jobs.
  [[nodiscard]] nlohmann::json ReadinessReasons() const;
  [[nodiscard]] nlohmann::json Describe(const std::string& job_id) const;
  [[nodiscard]] nlohmann::json List() const;
  [[nodiscard]] nlohmann::json Stats() const;
  // False while a replayed Job is unresolved (admission closed) or after a writer failure.
  [[nodiscard]] bool Ready() const;
  [[nodiscard]] std::vector<std::string> Unresolved() const;

  // Signals live children, drains driver threads, and seals the writer.
  void Shutdown();

 private:
  struct Step {
    bool ok = false;
    std::string detail;
  };
  void Run(std::string job_id);
  Step Await(const core::internal::IngressResult& admitted, const char* what);
  Step SubmitCandidate(const core::RawCandidateEvent& event, const char* what);
  void Record(const std::string& job_id, const std::string& step);
  void Fail(const std::string& job_id, const std::string& error);
  // True once a committed cancel is the Job's latched reason.
  bool Cancelled(const std::string& job_id) const;
  std::optional<JobRecord> Get(const std::string& job_id) const;

  DriverConfig config_;
  SystemClock clock_;
  LocalIdentitySource identity_;
  ProcessRunner runner_;
  NullSessionRetainer session_;
  journal::SegmentJournal& journal_;
  std::unique_ptr<core::internal::JobOrchestrator> orchestrator_;

  mutable std::mutex mutex_;
  std::condition_variable exited_;  // signalled whenever a child's exit status is recorded
  std::mutex create_mutex_;
  std::map<std::string, JobRecord> jobs_;
  std::vector<std::string> order_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_JOB_DRIVER_HPP_
