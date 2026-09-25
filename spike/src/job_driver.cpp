#include "job_driver.hpp"

#include <signal.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace sitometron::spike {
namespace {

using core::internal::IngressCode;
using nlohmann::json;

// The spike has no resource model. It commits one empty allocation so that the reducer's
// resource axis is exercised. sha256("{}") is the digest the core fixtures use for "{}".
constexpr const char* k_allocation_id = "spike-empty-allocation";
constexpr int k_shutdown_grace_seconds = 3;
constexpr const char* k_allocation_digest =
    "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
// Bundle provenance is not computed by the skeleton; the value only has to be a hex digest.
constexpr const char* k_bundle_placeholder =
    "0000000000000000000000000000000000000000000000000000000000000000";

std::string Payload(const json& value) { return value.dump(); }

core::RawCandidateEvent Candidate(const std::string& job, const char* type, const json& payload) {
  return core::RawCandidateEvent{1, core::Uuid{job}, type, Payload(payload)};
}

std::string StableApplicationId(const std::string& executable) {
  // StableId: [A-Za-z0-9][A-Za-z0-9._:-]*, max 128.
  std::string out;
  for (const char c : executable) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == ':' || c == '-';
    out.push_back(ok ? c : '_');
  }
  while (!out.empty() && !std::isalnum(static_cast<unsigned char>(out.front()))) out.erase(0, 1);
  if (out.empty()) out = "application";
  if (out.size() > 128) out.resize(128);
  return out;
}

}  // namespace

JobDriver::JobDriver(DriverConfig config, FileJournal& journal)
    : config_(std::move(config)), journal_(journal) {
  core::internal::Config orchestration;
  orchestration.max_jobs = config_.max_jobs;
  orchestration.normal_capacity = 64;
  orchestration.trace_capacity = config_.trace_capacity;
  // The writer validates its bounds: completions and trace must cover the whole FIFO
  // (normal + critical reserve) and ACK slots must cover every resident.
  orchestration.completion_capacity = orchestration.total_capacity();
  orchestration.handoff_capacity = 64;
  orchestration.ack_capacity = std::max<std::size_t>(64, config_.max_jobs);
  if (orchestration.trace_capacity < orchestration.total_capacity())
    orchestration.trace_capacity = orchestration.total_capacity();
  orchestration.callback_registration_capacity = 16;
  orchestration.ports.clock = &clock_;
  orchestration.ports.journal = &journal_;
  orchestration.ports.runner = &runner_;
  orchestration.ports.session = &session_;
  orchestration.ports.identity = &identity_;
  orchestrator_ = std::make_unique<core::internal::JobOrchestrator>(orchestration);
}

JobDriver::~JobDriver() { Shutdown(); }

std::optional<std::string> JobDriver::Submit(LaunchSpec spec, std::string& error) {
  // Create() serializes identity generation internally but exposes the new id only through
  // LastCreated(), so creation is serialized here too.
  std::lock_guard create(create_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      error = "daemon is shutting down";
      return std::nullopt;
    }
  }
  const auto admitted = orchestrator_->Create();
  const auto step = Await(admitted, "job_created");
  if (!step.ok) {
    error = step.detail;
    return std::nullopt;
  }
  const auto id = orchestrator_->LastCreated();
  if (!id) {
    error = "job_created committed but no identity was published";
    return std::nullopt;
  }
  JobRecord record;
  record.job_id = id->value;
  record.spec = std::move(spec);
  record.created_at = SystemClock::Rfc3339Now();
  record.steps.push_back("job_created");
  if (const auto worker = orchestrator_->GeneratedWorker(*id)) record.worker_id = worker->value;
  if (const auto launch = orchestrator_->GeneratedLaunch(*id))
    record.launch_operation_id = launch->value;
  {
    std::lock_guard lock(mutex_);
    jobs_.emplace(record.job_id, record);
    order_.push_back(record.job_id);
    threads_.emplace_back(&JobDriver::Run, this, record.job_id);
  }
  return record.job_id;
}

JobDriver::Step JobDriver::Await(const core::internal::IngressResult& admitted, const char* what) {
  if (admitted.code != IngressCode::kAdmitted) {
    return {false, std::string(what) + ": not admitted (ingress code " +
                       std::to_string(static_cast<int>(admitted.code)) + ")"};
  }
  // WaitUntil without an armed barrier waits for the writer to go idle, which implies this
  // sequence's turn has finished. There is no production completion notification yet.
  if (!orchestrator_->WaitUntil(admitted.ingress_sequence,
                                core::internal::WriterPhase::kTurnFinished)) {
    return {false, std::string(what) + ": writer did not complete the turn"};
  }
  const auto completion = orchestrator_->TakeCompletion(admitted.ingress_sequence);
  if (!completion) return {false, std::string(what) + ": completion slot missing"};
  switch (completion->code) {
    case core::internal::Completion::Code::kSuccess:
      return {true, {}};
    case core::internal::Completion::Code::kReducerRejection:
      return {false,
              std::string(what) + ": reducer rejected (" +
                  std::string(completion->rejection ? core::ToString(completion->rejection->reason)
                                                    : "unknown") +
                  ")"};
    default:
      return {false, std::string(what) + ": service failed"};
  }
}

JobDriver::Step JobDriver::SubmitCandidate(const core::RawCandidateEvent& event, const char* what) {
  return Await(orchestrator_->SubmitCandidate(event), what);
}

void JobDriver::Record(const std::string& job_id, const std::string& step) {
  std::lock_guard lock(mutex_);
  if (const auto found = jobs_.find(job_id); found != jobs_.end())
    found->second.steps.push_back(step);
}

void JobDriver::Fail(const std::string& job_id, const std::string& error) {
  std::lock_guard lock(mutex_);
  if (const auto found = jobs_.find(job_id); found != jobs_.end() && found->second.error.empty())
    found->second.error = error;
}

std::optional<JobRecord> JobDriver::Get(const std::string& job_id) const {
  std::lock_guard lock(mutex_);
  if (const auto found = jobs_.find(job_id); found != jobs_.end()) return found->second;
  return std::nullopt;
}

void JobDriver::Run(std::string job_id) {
  const auto record = Get(job_id);
  if (!record || !record->worker_id || !record->launch_operation_id) {
    Fail(job_id, "generated identities are missing");
    return;
  }
  const auto& worker = *record->worker_id;
  const auto& operation = *record->launch_operation_id;
  auto step = [&](const char* type, const json& payload) {
    const auto result = SubmitCandidate(Candidate(job_id, type, payload), type);
    if (result.ok)
      Record(job_id, type);
    else
      Fail(job_id, result.detail);
    return result.ok;
  };

  // 1. admitted -> preparing
  if (!step("resources_committed",
            {{"allocation_id", k_allocation_id},
             {"allocation_digest", k_allocation_digest},
             {"resolved_allocation",
              {{"schema_id", "allocation.v1"}, {"schema_version", 1}, {"payload_utf8", "{}"}}}}))
    return;

  // 2. launch intent -> writer hands the launch to the runner port
  if (!step("worker_launch_intent",
            {{"operation_id", operation},
             {"application",
              {{"application_id", StableApplicationId(record->spec.executable)},
               {"version", "0.0.0-spike"},
               {"bundle_sha256", k_bundle_placeholder}}},
             {"allocation_id", k_allocation_id},
             {"allocation_digest", k_allocation_digest},
             {"worker_id", worker}}))
    return;

  const auto launch = runner_.AwaitLaunch(core::Uuid{job_id});
  if (!launch) {
    Fail(job_id, "launch handoff never arrived (shutdown?)");
    return;
  }

  // 3. spawn the real process and report what the runner observed
  LaunchSpec spec = record->spec;
  if (spec.working_directory.empty()) spec.working_directory = config_.working_directory;
  const auto spawned = ProcessRunner::Spawn(spec);
  const bool started = spawned.pid > 0;
  {
    std::lock_guard lock(mutex_);
    jobs_[job_id].pid = spawned.pid;
  }
  if (!step("worker_launch_observed",
            {{"operation_id", operation}, {"outcome", started ? "started" : "failed"}}))
    return;
  bool worker_succeeded = false;
  core::RawCandidateEvent worker_event;
  if (started) {
    // The skeleton has no Worker protocol: process start is taken as "running".
    if (!step("worker_running", {{"worker_id", worker}})) return;
    const auto exit = ProcessRunner::Wait(spawned.pid);
    {
      std::lock_guard lock(mutex_);
      jobs_[job_id].exit = exit;
      exited_.notify_all();
    }
    worker_succeeded = exit.exited_normally && exit.exit_code == 0;
    worker_event = Candidate(job_id, worker_succeeded ? "worker_completed" : "worker_failed",
                             {{"worker_id", worker}, {"event_sequence", 1}});
    const auto result = Await(orchestrator_->SubmitWorker(worker_event), "worker_terminal");
    if (!result.ok) {
      Fail(job_id, result.detail);
      return;
    }
    Record(job_id, worker_event.event_type);
  } else {
    Fail(job_id, "spawn failed: " + spawned.error);
    // launch_observed{failed} already moved the Job to finalizing with a latched failure.
  }

  // 4. finalizing: session retention, finalization, terminal outcome
  if (!step("session_retain_requested", {{"session_id", job_id}})) return;
  if (!session_.AwaitRetain(core::Uuid{job_id})) {
    Fail(job_id, "session handoff never arrived (shutdown?)");
    return;
  }
  if (!step("session_retained", {{"session_id", job_id}})) return;
  if (!step("finalization_completed", json::object())) return;
  if (!step("terminal_outcome_committed", {{"outcome", worker_succeeded ? "succeeded" : "failed"}}))
    return;
  if (started && !orchestrator_->RetireWorkerAck(worker_event))
    Record(job_id, "worker_ack_not_retired");

  // 5. terminal axes: process exit, resource release, cleanup
  if (!step("process_exit_confirmed",
            {{"completion_mode", "process_already_exited"}, {"launch_operation_id", operation}}))
    return;
  if (!step("resources_released",
            {{"allocation_id", k_allocation_id}, {"allocation_digest", k_allocation_digest}}))
    return;
  step("cleanup_status_recorded", {{"status", "completed"}});
}

json JobDriver::Describe(const std::string& job_id) const {
  const auto record = Get(job_id);
  if (!record) return nullptr;
  json out{{"job_id", record->job_id},
           {"created_at", record->created_at},
           {"executable", record->spec.executable},
           {"arguments", record->spec.arguments},
           {"worker_id", record->worker_id ? json(*record->worker_id) : json(nullptr)},
           {"launch_operation_id",
            record->launch_operation_id ? json(*record->launch_operation_id) : json(nullptr)},
           {"pid", record->pid > 0 ? json(record->pid) : json(nullptr)},
           {"steps", record->steps},
           {"error", record->error.empty() ? json(nullptr) : json(record->error)}};
  if (record->exit) {
    out["exit"] = {{"exited_normally", record->exit->exited_normally},
                   {"exit_code", record->exit->exit_code},
                   {"signal", record->exit->signal}};
  } else {
    out["exit"] = nullptr;
  }
  if (const auto snapshot = orchestrator_->SnapshotFor(core::Uuid{job_id})) {
    out["state"] = std::string(core::ToString(snapshot->state));
    out["terminal"] = core::IsTerminalState(snapshot->state);
    out["process_exit_confirmed"] = snapshot->process_exit_confirmed;
  } else {
    out["state"] = nullptr;
    out["terminal"] = false;
  }
  return out;
}

json JobDriver::List() const {
  std::vector<std::string> ids;
  {
    std::lock_guard lock(mutex_);
    ids = order_;
  }
  json out = json::array();
  for (const auto& id : ids) {
    const auto entry = Describe(id);
    if (!entry.is_null())
      out.push_back({{"job_id", id}, {"state", entry["state"]}, {"terminal", entry["terminal"]}});
  }
  return out;
}

json JobDriver::Stats() const {
  std::size_t count = 0;
  {
    std::lock_guard lock(mutex_);
    count = order_.size();
  }
  return {{"jobs", count},
          {"max_jobs", config_.max_jobs},
          {"journal_committed", journal_.committed_count()},
          {"journal_lines_on_open", journal_.lines_on_open()},
          {"writer_failed", orchestrator_->failed()},
          {"trace_records", orchestrator_->CopyTrace().size()},
          {"trace_capacity", config_.trace_capacity}};
}

void JobDriver::Shutdown() {
  std::vector<std::thread> threads;
  {
    std::unique_lock lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    auto live = [this] {
      for (const auto& [id, record] : jobs_)
        if (record.pid > 0 && !record.exit) return true;
      return false;
    };
    for (const auto& [id, record] : jobs_)
      if (record.pid > 0 && !record.exit) ProcessRunner::Signal(record.pid, SIGTERM);
    // Cooperative grace period, then force: a child ignoring SIGTERM must not block shutdown.
    if (!exited_.wait_for(lock, std::chrono::seconds(k_shutdown_grace_seconds),
                          [&] { return !live(); })) {
      for (const auto& [id, record] : jobs_)
        if (record.pid > 0 && !record.exit) ProcessRunner::Signal(record.pid, SIGKILL);
    }
    threads.swap(threads_);
  }
  for (auto& thread : threads)
    if (thread.joinable()) thread.join();
  runner_.Abandon();
  session_.Abandon();
  const auto marker = orchestrator_->SubmitShutdown();
  if (marker.code == IngressCode::kAdmitted) {
    (void)orchestrator_->WaitUntil(marker.ingress_sequence,
                                   core::internal::WriterPhase::kShutdownMarker);
    (void)orchestrator_->TakeCompletion(marker.ingress_sequence);
    (void)orchestrator_->BeginShutdown();
  }
}

}  // namespace sitometron::spike
