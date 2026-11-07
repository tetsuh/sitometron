#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "job_orchestrator.hpp"
#include "job_orchestrator_harness.hpp"
#include "reducer_snapshot_json.hpp"
#include "supervisor_contract.hpp"

#if __has_include("job_supervisor.hpp")
#include "job_supervisor.hpp"
#define SITOMETRON_HAS_SUPERVISOR_DECISION 1
#endif

namespace {
using namespace sitometron::core;
using sitometron::core::internal::Completion;
using sitometron::core::internal::Config;
using sitometron::core::internal::IngressCode;
using sitometron::core::internal::IngressResult;
using sitometron::core::internal::JobOrchestrator;
using sitometron::core::internal::WriterPhase;
using namespace sitometron::test::supervisor;
namespace fixtures = sitometron::test;

void Require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
Json Contract() { return Load("schemas/core/v1/supervisor-decision-contract.json"); }

std::optional<Json> ActualDecision(const Json& snapshot, bool launch) {
#ifdef SITOMETRON_HAS_SUPERVISOR_DECISION
  const auto state = snapshot.is_null() ? std::optional<Snapshot>{}
                                        : std::optional{fixtures::SnapshotFrom(snapshot)};
  const auto decision =
      sitometron::core::internal::DecideSupervisor(state ? &*state : nullptr, launch);
  Json sources = Json::array();
  for (const auto source : decision.sources) sources.push_back(std::string(source));
  return Json{{"rule", decision.rule},
              {"kind", std::string(decision.kind)},
              {"event", std::string(decision.event)},
              {"sources", sources},
              {"payload_rule", std::string(decision.payload_rule)}};
#else
  (void)snapshot;
  (void)launch;
  return std::nullopt;
#endif
}

std::map<std::string, Json> ClassIndex(const Json& document) {
  std::map<std::string, Json> classes;
  for (const auto& item : document.at("classes"))
    Require(classes.emplace(item.at("id"), item.at("snapshot")).second, "duplicate class ID");
  return classes;
}
void CheckSeeds(const Json& document, const Graph& graph,
                const std::map<std::string, Json>& classes) {
  Require(document.at("seeds").size() == 25, "expected reconciled 25 seeds");
  std::set<std::string> ids;
  std::set<std::string> control_cases;
  for (const auto& seed : document.at("seeds")) {
    const auto id = seed.at("id").get<std::string>();
    Require(ids.insert(id).second, "duplicate seed ID");
    const auto& before = classes.at(seed.at("before_class"));
    const auto& after = classes.at(seed.at("after_class"));
    const auto pair = seed.at("pair_id").get<std::string>();
    const auto& row =
        graph.Row(before.at("state").get<std::string>(), pair.substr(pair.find("__") + 2));
    const auto& branch = Select(row, before, seed.at("payload"));
    Require(row.at("pair_id") == seed.at("pair_id") && branch.at("case_id") == seed.at("case_id"),
            "seed does not select its named accepting reducer case");
    Require(
        branch.at("disposition") != "reject" && Fold(before, branch, seed.at("payload")) == after,
        "seed is not reachable by its quoted accepting contract entry");
    Require(graph.classes.contains(before.dump()) && graph.classes.contains(after.dump()),
            "seed snapshot is not reachable from job_created");
    if (id == "S25") {
      Require(before.at("finalization_status") == "completed" &&
                  before.at("completion_candidate") == "succeeded" &&
                  after.at("latched_reason") == "timed_out" &&
                  after.at("completion_candidate").is_null(),
              "S25 must be the ADR-0012 Context late-timeout example");
      const Json stale_payload{{"outcome", "succeeded"}};
      const auto& stale =
          Select(graph.Row("finalizing", "terminal_outcome_committed"), after, stale_payload);
      Require(stale.at("case_id") == "outcome_mismatch", "S25 stale success must be rejected");
    } else
      Require(control_cases.insert(pair + "/" + seed.at("case_id").get<std::string>()).second,
              "first 24 seeds must be distinct accepting control branches");
  }
  std::set<std::string> expected;
  for (const auto& row : graph.contract.at("event_matrix")) {
    const auto event = row.at("event_kind").get<std::string>();
    if (event != "cancel_accepted" && event != "terminate_accepted" && event != "timeout_expired")
      continue;
    for (const auto& branch : row.at("cases"))
      if (branch.at("disposition") != "reject")
        expected.insert(row.at("pair_id").get<std::string>() + "/" +
                        branch.at("case_id").get<std::string>());
  }
  Require(control_cases == expected,
          "25-seed reconciliation omitted or invented an accepting branch");
}
int CoreSupervisorDecisionContract() {
  const auto document = Contract();
  const auto classes = ClassIndex(document);
  const Graph graph;
  CheckSeeds(document, graph, classes);
  std::set<std::string> consumed;
  std::set<std::pair<std::string, bool>> covered;
  std::size_t unavailable = 0;
  for (const auto& vector : document.at("vectors")) {
    const auto id = vector.at("id").get<std::string>();
    Require(consumed.insert(id).second, "decision vector consumed more than once");
    const auto key =
        vector.at("class_id").is_null() ? "absent" : vector.at("class_id").get<std::string>();
    const auto snapshot = key == "absent" ? Json(nullptr) : classes.at(key);
    const bool launch = vector.at("launch_inputs_present");
    Require(covered.emplace(key, launch).second, "duplicate decision class/presence vector");
    Require(vector.at("expected") == Expected(snapshot, launch),
            "vector disagrees with ADR-0012 first-match rules: " + id);
    const auto actual = ActualDecision(snapshot, launch);
    if (!actual)
      ++unavailable;
    else
      Require(*actual == vector.at("expected"), "real C++ decision disagrees with vector " + id);
  }
  Require(covered.size() == 2 * (classes.size() + 1),
          "missing class/presence or absent decision vector");
  std::cout << "decision vectors consumed exactly once=" << consumed.size() << "; seeds=25\n";
  Require(
      unavailable == 0,
      "real supervisor decision unavailable for 1238 contract vectors (ADR-0012 not implemented)");
  return 0;
}

struct Inventory {
  std::vector<const Edge*> deliverable;
  Json undeliverable = Json::array();
};
Inventory Overtakes(const Graph& graph, const std::map<std::string, Json>& classes) {
  std::map<std::string, std::string> ids;
  for (const auto& [id, snapshot] : classes) ids[snapshot.dump()] = id;
  const auto histories = graph.ArmedPaths();
  Inventory inventory;
  for (const auto& edge : graph.edges) {
    if (!OtherProducer(edge.event) ||
        Expected(graph.classes.at(edge.before), true).at("kind") != "Submit")
      continue;
    const auto found = histories.find(edge.before);
    std::string reason;
    if (found == histories.end() || found->second.empty())
      reason = "no_producer_deliverable_witness";
    else if (edge.event == "timeout_expired") {
      const auto phase = edge.payload.at("phase").get<std::string>();
      bool armed = false;
      for (const auto& history : found->second) armed = armed || history.contains(phase);
      if (!armed) reason = "no_current_armed_phase_for_this_class";
    } else if ((edge.event == "worker_completed" || edge.event == "worker_failed") &&
               graph.classes.at(edge.before).at("pending_worker_event_ack").get<bool>()) {
      bool matching_delivery = false;
      for (const auto& history : found->second)
        matching_delivery =
            matching_delivery ||
            ProducerAllows(history, graph.classes.at(edge.before), edge.event, edge.payload);
      if (!matching_delivery) reason = "worker_delivery_identity_conflicts_with_pending";
    }
    if (reason.empty())
      inventory.deliverable.push_back(&edge);
    else
      inventory.undeliverable.push_back(Json{{"class_id", ids.at(edge.before)},
                                             {"pair_id", edge.pair},
                                             {"case_id", edge.branch},
                                             {"payload", edge.payload},
                                             {"reason", reason}});
  }
  return inventory;
}
int CoreSupervisorDecisionComplete() {
  const auto document = Contract();
  const auto classes = ClassIndex(document);
  const Graph graph;
  std::set<std::string> committed;
  for (const auto& [id, snapshot] : classes) {
    (void)id;
    Require(committed.insert(snapshot.dump()).second, "duplicate reachable class");
    // Exercise typed snapshot conversion as well as the abstract reducer interpreter.
    Require(fixtures::SnapshotJson(fixtures::SnapshotFrom(snapshot)) == snapshot,
            "snapshot representative cannot roundtrip through the real core type");
  }
  std::set<std::string> reached;
  std::size_t unavailable = 0;
  for (const auto& [key, snapshot] : graph.classes) {
    reached.insert(key);
    for (const bool launch : {false, true}) {
      const auto expected = Expected(snapshot, launch);
      Require(graph.AcceptsSubmit(snapshot, expected),
              "assigned Submit has no accepting reducer payload");
      const auto actual = ActualDecision(snapshot, launch);
      if (!actual)
        ++unavailable;
      else
        Require(*actual == expected, "reachable class has wrong effective decision");
    }
  }
  Require(committed == reached,
          "committed class inventory differs from reducer-derived fixed point");
  std::set<std::string> consumed_vectors;
  std::set<std::pair<std::string, bool>> covered_vectors;
  for (const auto& vector : document.at("vectors")) {
    Require(consumed_vectors.insert(vector.at("id")).second,
            "completeness consumed a vector twice");
    const auto key =
        vector.at("class_id").is_null() ? "absent" : vector.at("class_id").get<std::string>();
    const bool launch = vector.at("launch_inputs_present");
    Require(covered_vectors.emplace(key, launch).second, "duplicate class/presence vector");
    const auto snapshot = key == "absent" ? Json(nullptr) : classes.at(key);
    Require(vector.at("expected") == Expected(snapshot, launch),
            "completeness vector has wrong expected answer");
    if (key == "absent") {
      const auto actual = ActualDecision(snapshot, launch);
      if (!actual)
        ++unavailable;
      else
        Require(*actual == vector.at("expected"), "absent boundary must be Done");
    }
  }
  Require(covered_vectors.size() == 2 * (reached.size() + 1),
          "completeness omitted a decision vector");
  std::cout << "completeness decision vectors consumed exactly once=" << consumed_vectors.size()
            << '\n';
  const auto inventory = Overtakes(graph, classes);
  std::set<std::string> declared, generated;
  for (const auto& row : document.at("reducer_accepted_but_undeliverable"))
    Require(declared.insert(row.dump()).second, "duplicate undeliverable edge");
  for (const auto& row : inventory.undeliverable) {
    Require(generated.insert(row.dump()).second, "generated edge consumed twice");
    std::cout << "REDUCER_ACCEPTED_BUT_UNDELIVERABLE " << row.dump() << '\n';
  }
  Require(declared == generated, "undeliverable list differs from armed-producer reachability");
  std::cout << "reachable classes=" << reached.size() << "; accepting edges=" << graph.edges.size()
            << "; deliverable overtakes=" << inventory.deliverable.size()
            << "; explicitly undeliverable=" << generated.size() << '\n';
  Require(unavailable == 0,
          "real supervisor decision unavailable for reducer-reachable classes (completeness cannot "
          "be claimed)");
  return 0;
}

// Optional test bridges let this acceptance commit compile BEFORE production implementation.
// They never emulate supervisor progress and cannot make missing behavior pass.
template <typename Writer>
bool Drain(Writer& writer) {
  if constexpr (requires { writer.WaitForSupervisorIdleForTest(); })
    return writer.WaitForSupervisorIdleForTest();
  else
    return writer.WaitForWriterIdle();
}
template <typename Writer>
bool Arm(Writer& writer, std::string_view point, std::string_view event = {}) {
  if constexpr (requires { writer.ArmSupervisorBarrierForTest(point, event); })
    return writer.ArmSupervisorBarrierForTest(point, event);
  else {
    (void)writer;
    (void)point;
    (void)event;
    return false;
  }
}
template <typename Writer>
bool WaitBarrier(Writer& writer) {
  if constexpr (requires { writer.WaitForSupervisorBarrierForTest(); })
    return writer.WaitForSupervisorBarrierForTest();
  else {
    (void)writer;
    return false;
  }
}
template <typename Writer>
bool ReleaseBarrier(Writer& writer) {
  if constexpr (requires { writer.ReleaseSupervisorBarrierForTest(); })
    return writer.ReleaseSupervisorBarrierForTest();
  else {
    (void)writer;
    return false;
  }
}
template <typename Writer>
Json View(Writer& writer, const Uuid& job) {
  if constexpr (requires { writer.InspectSupervisorForTest(job); }) {
    const auto state = writer.InspectSupervisorForTest(job);
    return Json{{"supported", true},
                {"physical_slots", state.physical_slots},
                {"marked", state.marked},
                {"parked", state.parked},
                {"in_flight", state.in_flight},
                {"max_in_flight", state.max_in_flight},
                {"decisions", state.decisions},
                {"submissions", state.submissions},
                {"breaches", state.breaches},
                {"stopped", state.stopped},
                {"joined", state.joined},
                {"writer_reentries", state.writer_reentries},
                {"handoff_reentries", state.handoff_reentries},
                {"reads_after_sealed", state.reads_after_sealed},
                {"distinct_threads", state.supervisor_thread != state.writer_thread &&
                                         state.supervisor_thread != std::thread::id{}}};
  } else {
    (void)writer;
    (void)job;
    return Json{{"supported", false}};
  }
}
auto Create(JobOrchestrator& writer) {
#ifdef SITOMETRON_HAS_SUPERVISOR_DECISION
  return writer.CreateJob(sitometron::core::internal::JobLaunchInputs{
      StableId{"demo-worker"}, std::string("1"), Digest{std::string(64, 'a')}});
#else
  return writer.CreateJob();
#endif
}
void Completed(JobOrchestrator& writer, const IngressResult& admitted) {
  Require(
      admitted.code == IngressCode::kAdmitted || admitted.code == IngressCode::kCoalescedPending,
      "producer input not admitted");
  const auto completion = writer.AwaitCompletion(admitted.ingress_sequence);
  Require(completion && completion->code == Completion::Code::kSuccess,
          "producer input not committed");
}

// Thread-safe passive ports: handoff only moves into preallocated storage. Test producers
// take a request after the handoff returns, and submit outside the writer/handoff stack.
class Ports final : public ClockPort,
                    public JobJournalPort,
                    public ApplicationRunnerPort,
                    public SessionRetainerPort,
                    public IdentitySourcePort {
 public:
  Ports() { journal.reserve(1024); }
  ClockReading Read() override { return {{"2026-11-01T00:00:00Z"}, {0}}; }
  LogicalCommitResult Commit(const LogicalJobEvent& event) noexcept override {
    try {
      std::lock_guard lock(mutex);
      journal.push_back(event);
      return LogicalCommitResult::kCommitted;
    } catch (...) {
      return LogicalCommitResult::kDefiniteFailure;
    }
  }
  void HandoffLaunch(ApplicationLaunchRequest&& request) noexcept override {
    std::lock_guard lock(mutex);
    if (launch)
      overflow = true;
    else
      launch.emplace(std::move(request));
  }
  void HandoffCooperativeStop(ApplicationStopRequest&& request) noexcept override {
    std::lock_guard lock(mutex);
    stop.emplace(std::move(request));
  }
  void HandoffForcedStop(ApplicationStopRequest&& request) noexcept override {
    std::lock_guard lock(mutex);
    stop.emplace(std::move(request));
  }
  void HandoffRetainSameIdentity(SessionRetainRequest&& request) noexcept override {
    std::lock_guard lock(mutex);
    if (session)
      overflow = true;
    else
      session.emplace(std::move(request));
  }
  JobSessionIdentityResult GenerateJobSessionIdentity() override {
    ++generation;
    if (generation == 1) return GeneratedJobSessionIdentity{Uuid{kJob}};
    const auto suffix = std::to_string(generation);
    return GeneratedJobSessionIdentity{
        Uuid{"01890f30-7b54-7cc3-98c4-" + std::string(12 - suffix.size(), '0') + suffix}};
  }
  WorkerIdentityResult GenerateWorkerIdentity() override {
    if (generation == 1) return GeneratedWorkerIdentity{Uuid{kWorker}};
    const auto suffix = std::to_string(generation);
    return GeneratedWorkerIdentity{
        Uuid{"550e8400-e29b-41d4-a716-" + std::string(12 - suffix.size(), '0') + suffix}};
  }
  LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    return GeneratedLaunchOperationIdentity{StableId{"launch-op-" + std::to_string(generation)}};
  }
  std::optional<ApplicationLaunchRequest> TakeLaunch() {
    std::lock_guard lock(mutex);
    auto result = std::move(launch);
    launch.reset();
    return result;
  }
  std::optional<SessionRetainRequest> TakeSession() {
    std::lock_guard lock(mutex);
    auto result = std::move(session);
    session.reset();
    return result;
  }
  std::vector<LogicalJobEvent> Journal() {
    std::lock_guard lock(mutex);
    return journal;
  }
  bool Good() {
    std::lock_guard lock(mutex);
    return !overflow;
  }

 private:
  std::mutex mutex;
  std::vector<LogicalJobEvent> journal;
  std::optional<ApplicationLaunchRequest> launch;
  std::optional<ApplicationStopRequest> stop;
  std::optional<SessionRetainRequest> session;
  unsigned generation = 0;
  bool overflow = false;
};
struct Fixture {
  Ports ports;
  JobOrchestrator writer;
  static Config Configuration(Ports& ports, std::size_t max_jobs = 4) {
    Config config;
    config.max_jobs = max_jobs;
    config.normal_capacity = 2;
    config.trace_capacity = 1024;
    config.completion_capacity = 128;
    config.handoff_capacity = 128;
    config.ack_capacity = 64;
    config.callback_registration_capacity = 32;
    config.ports = {&ports, &ports, &ports, &ports, &ports, nullptr, nullptr};
    return config;
  }
  std::set<std::uint64_t> consumed_producer_completions;
  std::optional<RawCandidateEvent> pending_worker_delivery;
  explicit Fixture(std::size_t max_jobs = 4) : writer(Configuration(ports, max_jobs)) {}
  Uuid New() {
    const auto created = Create(writer);
    Completed(writer, created.ingress);
    Require(created.job_id.has_value(), "admitted create lacks generated Job ID");
    return *created.job_id;
  }
  void Idle() { Require(Drain(writer), "supervisor/writer did not drain"); }
  ApplicationLaunchRequest Running(const Uuid& job) {
    Idle();
    const auto snapshot = writer.SnapshotFor(job);
    Require(snapshot && snapshot->state == JobState::kPreparing &&
                snapshot->worker_launch_status == LaunchStatus::kIntentRecorded,
            "expected autonomous worker_launch_intent; committed Job is still admitted");
    auto request = ports.TakeLaunch();
    Require(request.has_value(), "launch intent did not hand off a launch request");
    Require(writer.GeneratedWorker(job) == request->intent.worker_id &&
                writer.GeneratedLaunch(job) == request->intent.operation_id &&
                snapshot->allocation_id == request->intent.allocation_id &&
                snapshot->allocation_digest == request->intent.allocation_digest,
            "launch intent lost creation-generated identities or committed allocation bindings");
    Require(request->intent.application_id == StableId{"demo-worker"} &&
                request->intent.application_version == "1" &&
                request->intent.bundle_sha256 == Digest{std::string(64, 'a')},
            "launch intent changed immutable creation launch inputs");
    const auto journal = ports.Journal();
    const auto allocation = std::find_if(journal.begin(), journal.end(), [&](const auto& event) {
      return event.job_id == job && event.event_type == EventType::kResourcesCommitted;
    });
    Require(allocation != journal.end(), "launch intent has no preceding committed allocation");
    const auto& payload = std::get<ResourcesCommittedPayload>(allocation->payload);
    Require(payload.allocation_id == StableId{kAllocation} &&
                payload.allocation_digest == Digest{kDigest} &&
                payload.schema_id == StableId{"allocation.v1"} && payload.schema_version == 1 &&
                payload.payload_utf8 == "{}",
            "placeholder must use one fixed empty allocation for every Job");
    const Json observed{{"operation_id", request->intent.operation_id.value},
                        {"outcome", "started"}};
    Completed(writer, writer.SubmitCandidate({1, job, "worker_launch_observed", observed.dump()}));
    Completed(writer,
              writer.SubmitCandidate(fixtures::MakeWorkerRunning(job, request->intent.worker_id)));
    Idle();
    return std::move(*request);
  }
  void Retain(bool drain_after = true) {
    Idle();
    auto request = ports.TakeSession();
    Require(request.has_value(), "supervisor did not request Session retention");
    Require(request->job_id == request->session_id,
            "retainer identity differs from creation binding");
    Completed(writer, writer.SubmitCandidate(fixtures::MakeSessionRetained(request->job_id)));
    if (drain_after) Idle();
  }
  void Exit(const Uuid& job, const ApplicationLaunchRequest& launch) {
    const Json payload{{"completion_mode", "cooperative"},
                       {"launch_operation_id", launch.intent.operation_id.value}};
    Completed(writer, writer.SubmitCandidate({1, job, "process_exit_confirmed", payload.dump()}));
    Idle();
  }
  void Closed(const Uuid& job, std::string_view outcome) {
    Idle();
    const auto snapshot = writer.SnapshotFor(job);
    Require(snapshot.has_value(), "Job disappeared before cleanup observation");
    const auto json = fixtures::SnapshotJson(*snapshot);
    Require(json.at("state") == outcome && json.at("cleanup_status") == "completed" &&
                json.at("resource_status") != "committed",
            "wrong terminal outcome, cleanup, or resource release");
    const auto events = ports.Journal();
    std::set<EventType> generated;
    for (const auto& event : events)
      if (event.job_id == job) generated.insert(event.event_type);
    Require(generated.contains(EventType::kSessionRetainRequested) &&
                generated.contains(EventType::kFinalizationCompleted) &&
                generated.contains(EventType::kTerminalOutcomeCommitted) &&
                generated.contains(EventType::kCleanupStatusRecorded),
            "lifecycle silently omitted an owed supervisor event");
    Require(ports.Good() && writer.NoForbiddenPostcommitActions() &&
                writer.NoPostcommitAllocationOrCopy(),
            "handoff storage or no-fail commit discipline violated");
  }
};

std::vector<Json> RuntimeVectors(std::string_view check) {
  std::vector<Json> result;
  std::set<std::string> unique;
  const auto document = Contract();
  for (const auto& vector : document.at("runtime_vectors")) {
    Require(unique.insert(vector.at("id")).second, "duplicate ordering vector ID");
    if (vector.at("check") == check) result.push_back(vector);
  }
  Require(!result.empty(), "missing ordering vector inventory");
  return result;
}
void Lifecycle(const Json& vector) {
  Fixture fixture;
  const auto job = fixture.New();
  const auto request = fixture.Running(job);
  const auto parameters = vector.at("parameters");
  const auto outcome = parameters.at("outcome").get<std::string>();
  if (outcome == "cancelled" || outcome == "terminated")
    Completed(fixture.writer,
              fixture.writer.SubmitCommand(
                  {1, outcome == "cancelled" ? CommandType::kCancel : CommandType::kTerminate, job,
                   "operator@example"}));
  if (outcome == "timed_out") {
    const auto slot = fixture.writer.InspectSlotForTest(job);
    Require(slot.has_value(), "missing execution timer registration");
    const auto timeout =
        fixture.writer.SubmitTimeout({job, TimeoutPhase::kExecution, slot->timer_generation[1]});
    Require(!timeout.discarded, "current armed execution timeout discarded");
    Completed(fixture.writer, timeout.admitted);
  }
  Completed(
      fixture.writer,
      fixture.writer.SubmitWorker(
          outcome == "failed" ? fixtures::MakeWorkerFailed(job, request.intent.worker_id, 1)
                              : fixtures::MakeWorkerCompleted(job, request.intent.worker_id, 1)));
  if (parameters.at("exit_order") == "before_terminal") fixture.Exit(job, request);
  fixture.Retain();
  if (parameters.at("exit_order") == "after_terminal") fixture.Exit(job, request);
  Require(!fixture.writer.SnapshotFor(job)->pending_worker_event_ack,
          "confirmed process exit did not clear Worker ACK binding");
  fixture.Closed(job, outcome);
}
void EarlyCommand(const Json& vector) {
  Fixture fixture;
  const auto parameters = vector.at("parameters");
  const auto landing = parameters.at("landing").get<std::string>();
  const std::string event = landing == "admitted" ? "resources_committed" : "worker_launch_intent";
  if (landing != "intent_recorded")
    Require(Arm(fixture.writer, "after_decision", event),
            "missing deterministic supervisor decision barrier");
  const auto job = fixture.New();
  if (landing != "intent_recorded")
    Require(WaitBarrier(fixture.writer), "supervisor did not reach command landing");
  else
    fixture.Idle();
  const bool cancel = parameters.at("command") == "cancel";
  Completed(fixture.writer, fixture.writer.SubmitCommand(
                                {1, cancel ? CommandType::kCancel : CommandType::kTerminate, job,
                                 "operator@example"}));
  if (landing != "intent_recorded")
    Require(ReleaseBarrier(fixture.writer), "could not release supervisor decision");
  else {
    auto launch = fixture.ports.TakeLaunch();
    Require(launch.has_value(), "missing launch before intent-recorded cancellation");
    fixture.Exit(job, *launch);
  }
  fixture.Retain();
  fixture.Closed(job, cancel ? "cancelled" : "terminated");
}
int JobSupervisorDrivesToCleanup() {
  const auto vectors = RuntimeVectors("job_supervisor_drives_to_cleanup");
  for (const auto& vector : vectors) {
    const auto scenario = vector.at("scenario").get<std::string>();
    if (scenario == "lifecycle")
      Lifecycle(vector);
    else if (scenario == "early_command")
      EarlyCommand(vector);
    else if (scenario == "launch_failed") {
      Fixture fixture;
      const auto job = fixture.New();
      fixture.Idle();
      auto request = fixture.ports.TakeLaunch();
      Require(request.has_value(), "supervisor never recorded launch intent");
      Completed(
          fixture.writer,
          fixture.writer.SubmitCandidate(
              {1, job, "worker_launch_observed",
               Json{{"operation_id", request->intent.operation_id.value}, {"outcome", "failed"}}
                   .dump()}));
      fixture.Retain();
      fixture.Closed(job, "failed");
    } else
      throw std::runtime_error("unconsumed lifecycle vector: " + scenario);
    std::cout << "consumed " << vector.at("id") << '\n';
  }
  return 0;
}

#include "supervisor_ordering_checks.hpp"
#include "supervisor_overtake_checks.hpp"
int JobSupervisorOvertakes() {
  const auto vectors = RuntimeVectors("job_supervisor_overtakes");
  Require(vectors.size() == 1 && vectors.front().at("scenario") == "all_generated",
          "unknown overtake vector");
  const Graph graph;
  const auto inventory = Overtakes(graph, ClassIndex(Contract()));
  Require(!inventory.deliverable.empty(), "empty generated overtake set");
  std::set<std::string> consumed;
  for (const auto* edge : inventory.deliverable) {
    Require(consumed.insert(edge->before + edge->Name()).second,
            "generated overtake consumed twice");
    Fixture fixture;
    InjectOvertake(fixture, graph, *edge);
  }
  std::cout << "generated overtakes consumed=" << consumed.size()
            << "; explicit undeliverable=" << inventory.undeliverable.size() << '\n';
  return 0;
}
int OrderingCheck(std::string_view check) {
  const Graph graph;
  const auto vectors = RuntimeVectors(check);
  std::set<std::string> consumed;
  for (const auto& vector : vectors) {
    Require(consumed.insert(vector.at("id")).second, "ordering vector consumed twice");
    Fixture fixture;
    // Exercise existing writer checkpoints as well as the private supervisor checkpoints.
    Require(fixture.writer.ArmBarrier(WriterPhase::kAfterApply), "writer barrier unavailable");
    const auto created = Create(fixture.writer);
    Require(created.ingress.code == IngressCode::kAdmitted, "ordering create refused");
    Require(fixture.writer.WaitUntil(created.ingress.ingress_sequence, WriterPhase::kAfterApply),
            "writer did not install creation");
    Require(fixture.writer.Release(created.ingress.ingress_sequence, WriterPhase::kAfterApply),
            "writer barrier release failed");
    Completed(fixture.writer, created.ingress);
    OrderingVector(fixture, vector, graph);
  }
  std::cout << check << ": ordering vectors consumed=" << consumed.size() << '\n';
  return 0;
}
int JobSupervisorRejectionRederives() {
  return OrderingCheck("job_supervisor_rejection_rederives");
}
int JobSupervisorIngressResults() { return OrderingCheck("job_supervisor_ingress_results"); }
int JobSupervisorNoWriterReentry() { return OrderingCheck("job_supervisor_no_writer_reentry"); }
}  // namespace

int main(int argc, char** argv) {
  const std::string name = argc == 2 ? argv[1] : "";
  try {
    if (name == "core_supervisor_decision_contract") return CoreSupervisorDecisionContract();
    if (name == "core_supervisor_decision_complete") return CoreSupervisorDecisionComplete();
    if (name == "job_supervisor_drives_to_cleanup") return JobSupervisorDrivesToCleanup();
    if (name == "job_supervisor_overtakes") return JobSupervisorOvertakes();
    if (name == "job_supervisor_rejection_rederives") return JobSupervisorRejectionRederives();
    if (name == "job_supervisor_ingress_results") return JobSupervisorIngressResults();
    if (name == "job_supervisor_no_writer_reentry") return JobSupervisorNoWriterReentry();
    std::cerr << "unknown supervisor check: " << name << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << name << ": " << error.what() << '\n';
    return 1;
  }
}
