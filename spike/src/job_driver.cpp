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

JobDriver::JobDriver(DriverConfig config, journal::SegmentJournal& journal)
    : config_(std::move(config)), journal_(journal) {
  core::internal::Config orchestration;
  orchestration.max_jobs = config_.max_jobs;
  // Continue after the last durable record, with every replayed Job resident (OPS-003, OPS-004).
  orchestration.initial_journal_sequence = journal_.NextSequence();
  orchestration.replayed_jobs = config_.replayed_jobs;
  orchestration.unresolved_jobs = config_.unresolved_jobs;
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
  for (const auto& snapshot : config_.replayed_jobs) {
    JobRecord record;
    record.job_id = snapshot.job_id.value;
    record.recovered = true;
    if (snapshot.worker_id) record.worker_id = snapshot.worker_id->value;
    if (snapshot.launch_operation_id)
      record.launch_operation_id = snapshot.launch_operation_id->value;
    jobs_.emplace(record.job_id, record);
    order_.push_back(record.job_id);
  }
}

JobDriver::~JobDriver() { Shutdown(); }

std::optional<std::string> JobDriver::Submit(LaunchSpec spec, std::string& error) {
  auto outcome = Create(std::move(spec));
  if (outcome.refusal == CreateRefusal::kNone) return outcome.job_id;
  error = std::move(outcome.detail);
  return std::nullopt;
}

CreateOutcome JobDriver::Create(LaunchSpec spec) {
  // A second creation during identity generation is refused with already_pending, so creation is
  // serialized here.
  std::lock_guard create(create_mutex_);
  if (orchestrator_->failed()) return {CreateRefusal::kServiceFailed, {}, "the writer has failed"};
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return {CreateRefusal::kNotReady, {}, "daemon is shutting down"};
  }
  if (!orchestrator_->unresolved().empty()) {
    return {CreateRefusal::kNotReady, {}, "admission closed: unresolved Jobs from a previous run"};
  }
  const auto created = orchestrator_->CreateJob();
  switch (created.ingress.code) {
    case IngressCode::kAdmitted:
      break;
    case IngressCode::kResidentLimit:
      return {CreateRefusal::kCapacityExhausted, {}, "job_created: resident limit reached"};
    case IngressCode::kAdmissionClosed:
      return {CreateRefusal::kNotReady, {}, "job_created: admission closed"};
    case IngressCode::kServiceFailed:
      return {CreateRefusal::kServiceFailed, {}, "job_created: the writer has failed"};
    default:
      // normal_full, and the pending results that serialized creation should not see.
      return {CreateRefusal::kBusy, {}, "job_created: the ingress queue is full"};
  }
  if (const auto step = Await(created.ingress, "job_created"); !step.ok) {
    return {CreateRefusal::kServiceFailed, {}, step.detail};
  }
  const auto& id = created.job_id;
  if (!id) {
    return {CreateRefusal::kServiceFailed, {}, "job_created committed without an identity"};
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
  return {CreateRefusal::kNone, record.job_id, {}};
}

JobDriver::Step JobDriver::Await(const core::internal::IngressResult& admitted, const char* what) {
  if (admitted.code != IngressCode::kAdmitted) {
    return {false, std::string(what) + ": not admitted (ingress code " +
                       std::to_string(static_cast<int>(admitted.code)) + ")"};
  }
  const auto completion = orchestrator_->AwaitCompletion(admitted.ingress_sequence);
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

bool JobDriver::Cancelled(const std::string& job_id) const {
  const auto snapshot = orchestrator_->SnapshotFor(core::Uuid{job_id});
  return snapshot && snapshot->latched_reason == core::TerminalOutcome::kCancelled;
}

CancelOutcome JobDriver::Cancel(const std::string& job_id) {
  const auto admitted = orchestrator_->SubmitCommand(
      core::Command{1, core::CommandType::kCancel, core::Uuid{job_id}, config_.cancel_principal});
  switch (admitted.code) {
    case IngressCode::kAdmitted:
      break;
    case IngressCode::kAdmissionClosed:
      return CancelOutcome::kNotReady;
    case IngressCode::kServiceFailed:
      return CancelOutcome::kServiceFailed;
    default:
      return CancelOutcome::kBusy;
  }
  const auto completion = orchestrator_->AwaitCompletion(admitted.ingress_sequence);
  if (!completion) return CancelOutcome::kServiceFailed;
  if (completion->code == core::internal::Completion::Code::kSuccess)
    return CancelOutcome::kAccepted;
  if (completion->code != core::internal::Completion::Code::kReducerRejection ||
      !completion->rejection)
    return CancelOutcome::kServiceFailed;
  switch (completion->rejection->reason) {
    case core::RejectionReason::kJobNotFound:
      return CancelOutcome::kJobNotFound;
    case core::RejectionReason::kStopCauseAlreadyLatched:
      return CancelOutcome::kStopCauseAlreadyLatched;
    case core::RejectionReason::kCommandNotAllowedInState:
      return CancelOutcome::kCommandNotAllowedInState;
    default:
      return CancelOutcome::kServiceFailed;
  }
}

void JobDriver::Run(std::string job_id) {
  const auto record = Get(job_id);
  if (!record || !record->worker_id || !record->launch_operation_id) {
    Fail(job_id, "generated identities are missing");
    return;
  }
  const auto& worker = *record->worker_id;
  const auto& operation = *record->launch_operation_id;
  // A committed cancel moves the Job to stopping or finalizing, so the step this thread submits
  // next is refused. Before finalization that is not a failure: the thread skips to finalization.
  bool finalizing = false;
  bool overtaken = false;
  auto step = [&](const char* type, const json& payload) {
    const auto result = SubmitCandidate(Candidate(job_id, type, payload), type);
    if (result.ok)
      Record(job_id, type);
    else if (!finalizing && Cancelled(job_id))
      overtaken = true;
    else
      Fail(job_id, result.detail);
    return result.ok;
  };

  bool worker_succeeded = false;
  bool worker_reported = false;
  core::RawCandidateEvent worker_event;
  // Steps 1 to 3. False when the Job cannot be driven further.
  const auto launch_and_wait = [&]() -> bool {
    // 1. admitted -> preparing
    if (!step("resources_committed",
              {{"allocation_id", k_allocation_id},
               {"allocation_digest", k_allocation_digest},
               {"resolved_allocation",
                {{"schema_id", "allocation.v1"}, {"schema_version", 1}, {"payload_utf8", "{}"}}}}))
      return overtaken;

    // 2. launch intent -> writer hands the launch to the runner port
    if (!step("worker_launch_intent",
              {{"operation_id", operation},
               {"application",
                {{"application_id", record->spec.application_id.empty()
                                        ? StableApplicationId(record->spec.executable)
                                        : record->spec.application_id},
                 {"version", "0.0.0-spike"},
                 {"bundle_sha256", k_bundle_placeholder}}},
               {"allocation_id", k_allocation_id},
               {"allocation_digest", k_allocation_digest},
               {"worker_id", worker}}))
      return overtaken;

    const auto launch = runner_.AwaitLaunch(core::Uuid{job_id});
    if (!launch) {
      Fail(job_id, "launch handoff never arrived (shutdown?)");
      return false;
    }

    // 3. spawn the real process and report what the runner observed
    LaunchSpec spec = record->spec;
    if (spec.working_directory.empty()) spec.working_directory = config_.working_directory;
    // Spawn and publish the pid under the same lock that Shutdown() scans with, so a shutdown
    // either refuses the launch before it happens or sees the pid it has to signal.
    SpawnResult spawned;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        spawned.error = "shutdown began before launch";
      } else {
        spawned = ProcessRunner::Spawn(spec);
        jobs_[job_id].pid = spawned.pid;
      }
    }
    const bool started = spawned.pid > 0;
    // A cancel committed after the launch intent finds no child yet; attaching delivers its stop.
    if (started) runner_.Attach(core::Uuid{job_id}, spawned.pid);
    const bool observed =
        step("worker_launch_observed",
             {{"operation_id", operation}, {"outcome", started ? "started" : "failed"}});
    if (!started) {
      Fail(job_id, "spawn failed: " + spawned.error);
      // launch_observed{failed} already moved the Job to finalizing with a latched failure.
      return observed || overtaken;
    }
    // The skeleton has no Worker protocol: process start is taken as "running".
    const bool running = observed && step("worker_running", {{"worker_id", worker}});
    if (!running && !overtaken) return false;
    const auto exit = runner_.WaitAttached(core::Uuid{job_id}, spawned.pid);
    {
      std::lock_guard lock(mutex_);
      jobs_[job_id].exit = exit;
      exited_.notify_all();
    }
    // A cancel before worker_running leaves no Worker to report; the exit is confirmed below.
    if (!running) return true;
    worker_succeeded = exit.exited_normally && exit.exit_code == 0;
    worker_event = Candidate(job_id, worker_succeeded ? "worker_completed" : "worker_failed",
                             {{"worker_id", worker}, {"event_sequence", 1}});
    const auto result = Await(orchestrator_->SubmitWorker(worker_event), "worker_terminal");
    if (!result.ok) {
      Fail(job_id, result.detail);
      return false;
    }
    Record(job_id, worker_event.event_type);
    worker_reported = true;
    return true;
  };
  if (!launch_and_wait()) return;
  finalizing = true;

  // A cancel that found a process leaves the Job stopping until the Worker reports or the exit is
  // confirmed. The child has exited by now, so the exit is confirmed here.
  auto snapshot = orchestrator_->SnapshotFor(core::Uuid{job_id});
  if (snapshot && snapshot->state == core::JobState::kStopping &&
      !step("process_exit_confirmed",
            {{"completion_mode", "cooperative"}, {"launch_operation_id", operation}}))
    return;

  // 4. finalizing: session retention, finalization, terminal outcome
  if (!step("session_retain_requested", {{"session_id", job_id}})) return;
  if (!session_.AwaitRetain(core::Uuid{job_id})) {
    Fail(job_id, "session handoff never arrived (shutdown?)");
    return;
  }
  if (!step("session_retained", {{"session_id", job_id}})) return;
  if (!step("finalization_completed", json::object())) return;
  // The latched reason decides the outcome; no cancel is accepted once the Job is finalizing.
  const char* outcome = "failed";
  if (Cancelled(job_id))
    outcome = "cancelled";
  else if (worker_succeeded)
    outcome = "succeeded";
  if (!step("terminal_outcome_committed", {{"outcome", outcome}})) return;
  if (worker_reported && !orchestrator_->RetireWorkerAck(worker_event))
    Record(job_id, "worker_ack_not_retired");

  // 5. terminal axes: process exit, resource release, cleanup. A cancel before the launch intent
  // leaves no launch whose exit could be confirmed (the reducer has confirmed it), and a cancel
  // before resources_committed leaves nothing to release.
  snapshot = orchestrator_->SnapshotFor(core::Uuid{job_id});
  if (!snapshot) {
    Fail(job_id, "the terminal snapshot is missing");
    return;
  }
  if (snapshot->launch_operation_id &&
      !step("process_exit_confirmed",
            {{"completion_mode", snapshot->completion_mode == core::CompletionMode::kCooperative
                                     ? "cooperative"
                                     : "process_already_exited"},
             {"launch_operation_id", operation}}))
    return;
  if (snapshot->resource_status == core::ResourceStatus::kCommitted &&
      !step("resources_released",
            {{"allocation_id", k_allocation_id}, {"allocation_digest", k_allocation_digest}}))
    return;
  step("cleanup_status_recorded", {{"status", "completed"}});
}

json JobDriver::Describe(const std::string& job_id) const {
  const auto record = Get(job_id);
  if (!record) return nullptr;
  // A recovered Job has only its replayed snapshot: the launch request is not journaled.
  const auto known = [&record](const auto& value) {
    return record->recovered ? json(nullptr) : json(value);
  };
  json out{{"job_id", record->job_id},
           {"recovered", record->recovered},
           {"created_at", known(record->created_at)},
           {"executable", known(record->spec.executable)},
           {"arguments", known(record->spec.arguments)},
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
      out.push_back({{"job_id", id},
                     {"recovered", entry["recovered"]},
                     {"state", entry["state"]},
                     {"terminal", entry["terminal"]}});
  }
  return out;
}

json JobDriver::Resource(const std::string& job_id) const {
  {
    std::lock_guard lock(mutex_);
    if (!jobs_.contains(job_id)) return nullptr;
  }
  const auto snapshot = orchestrator_->SnapshotFor(core::Uuid{job_id});
  if (!snapshot) return nullptr;
  const auto state = std::string(core::ToString(snapshot->state));
  const bool terminal = core::IsTerminalState(snapshot->state);
  return {{"job_id", job_id},
          {"state", state},
          {"terminal", terminal},
          {"outcome", terminal ? json(state) : json(nullptr)}};
}

json JobDriver::Resources() const {
  std::vector<std::string> ids;
  {
    std::lock_guard lock(mutex_);
    ids = order_;
  }
  json out = json::array();
  for (const auto& id : ids) {
    if (auto resource = Resource(id); !resource.is_null()) out.push_back(std::move(resource));
  }
  return out;
}

json JobDriver::ReadinessReasons() const {
  json reasons = json::array();
  if (orchestrator_->failed()) reasons.push_back({{"code", "service_failed"}});
  if (const auto unresolved = Unresolved(); !unresolved.empty()) {
    reasons.push_back({{"code", "unresolved_jobs"}, {"job_ids", unresolved}});
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_) reasons.push_back({{"code", "shutting_down"}});
  }
  return reasons;
}

bool JobDriver::Ready() const { return orchestrator_->ready(); }

std::vector<std::string> JobDriver::Unresolved() const {
  std::vector<std::string> ids;
  for (const auto& id : orchestrator_->unresolved()) ids.push_back(id.value);
  return ids;
}

json JobDriver::Stats() const {
  std::size_t count = 0;
  {
    std::lock_guard lock(mutex_);
    count = order_.size();
  }
  return {{"jobs", count},
          {"max_jobs", config_.max_jobs},
          {"journal_next_sequence", journal_.NextSequence()},
          {"journal_poisoned", journal_.Poisoned()},
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
    (void)orchestrator_->AwaitCompletion(marker.ingress_sequence);
    (void)orchestrator_->BeginShutdown();
  }
}

}  // namespace sitometron::spike
