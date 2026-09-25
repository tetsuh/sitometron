#ifndef SITOMETRON_SPIKE_JOB_DRIVER_HPP_
#define SITOMETRON_SPIKE_JOB_DRIVER_HPP_

#include <sys/types.h>

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

#include "file_journal.hpp"
#include "job_orchestrator.hpp"
#include "process_runner.hpp"
#include "sitometron/core/job_ports.hpp"
#include "system_ports.hpp"

namespace sitometron::spike {

struct DriverConfig {
  std::size_t max_jobs = 32;          // resident slots: also the lifetime Job count (see README)
  std::size_t trace_capacity = 4096;  // writer trace + ingress log bound (see README)
  std::string working_directory;
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
};

// The composition root's Job sequencer. One thread per Job drives the lifecycle candidates that
// the core leaves to "the supervisor": resources, launch intent, session retention, finalization,
// terminal outcome, process-exit confirmation, release, and cleanup. Every step submits one raw
// candidate, waits for the writer, and inspects the completion.
class JobDriver {
 public:
  JobDriver(DriverConfig config, FileJournal& journal);
  ~JobDriver();
  JobDriver(const JobDriver&) = delete;
  JobDriver& operator=(const JobDriver&) = delete;

  // Creates the Job (job_created committed) and starts its driver thread.
  [[nodiscard]] std::optional<std::string> Submit(LaunchSpec spec, std::string& error);
  [[nodiscard]] nlohmann::json Describe(const std::string& job_id) const;
  [[nodiscard]] nlohmann::json List() const;
  [[nodiscard]] nlohmann::json Stats() const;

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
  std::optional<JobRecord> Get(const std::string& job_id) const;

  DriverConfig config_;
  SystemClock clock_;
  LocalIdentitySource identity_;
  ProcessRunner runner_;
  NullSessionRetainer session_;
  FileJournal& journal_;
  std::unique_ptr<core::internal::JobOrchestrator> orchestrator_;

  mutable std::mutex mutex_;
  std::mutex create_mutex_;
  std::map<std::string, JobRecord> jobs_;
  std::vector<std::string> order_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_JOB_DRIVER_HPP_
