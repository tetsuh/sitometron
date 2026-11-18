#ifndef SITOMETRON_TEST_PROCESS_COMPOSITION_CHECKS_HPP_
#define SITOMETRON_TEST_PROCESS_COMPOSITION_CHECKS_HPP_

#include "registry_composition_checks.hpp"

// Included in http_rest_v1_test.cpp's job_routes namespace after registry fixtures.
// The optional deterministic drain is test observation only, not a production state API.
template <typename Composition>
bool ProcessCompositionDrained(Composition& daemon) {
  if constexpr (requires { daemon.WaitForRunnerExitForTest(); })
    return daemon.WaitForRunnerExitForTest() && daemon.WaitForSupervisorIdleForTest();
  else
    return daemon.WaitForSupervisorIdleForTest();
}

int ProductionRunnerFailureSealing() {
  constexpr std::string_view name = "rest_v1_protocol_less_process_fails";
  RegistryFixture fixture("issue141-failure-sealing-");
  sitometron::test::process::Control control;
  fixture.bundle = fixture.root / "live helper.exe";
  auto& application = fixture.document["applications"][0];
  application["executable"] = RegistryFixture::Text(fixture.bundle);
  application["bundle_sha256"] = CopyRegisteredProcessHelper(fixture.bundle);
  application["arguments"] = RegisteredProcessArguments(fixture, control.port());
  fixture.Save();
  auto config = fixture.Config();
  config.hold_runner_observations_for_test = true;
  SetRegistryPath(config, fixture.file);
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error)) return Check(false, name, "failure-sealing startup refused: " + error);
  if (!daemon.HandoffProbeSelfTestForTest())
    return Check(false, name, "owned composition mutex/core-entry probe missed planted accesses");
  const auto created =
      SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("demo-worker")));
  int result = ExpectStatus(created, name, 202);
  if (created.status != 202) return result;
  daemon.WaitForRunnerObservationForTest();
  (void)control.Ready();
  result |= Check(daemon.LatchFailureForTest() && !daemon.backend().GetReadiness().ready, name,
                  "production readiness did not become false at the fail-closed latch");
  result |= Check(daemon.WaitForSupervisorIdleForTest() && !daemon.FailureSealedForTest(), name,
                  "failure sealed before the composition joined the live runner");
  control.Ping();  // Deterministic live-process witness: closure must neither force nor join it.
  daemon.Stop();
  result |=
      Check(daemon.FailureSealedForTest(), name,
            "external join did not re-drive deferred failure sealing before core destruction");
  const auto replayed = sitometron::journal::ReplayJournal(
      sitometron::journal::SystemFileSystem(), config.journal_path,
      sitometron::journal::ReplayOptions{config.max_jobs});
  result |= Check(replayed.status == sitometron::journal::ReplayStatus::kReplayed &&
                      replayed.jobs.size() == 1 && replayed.unresolved.size() == 1 &&
                      replayed.jobs[0].state == core::JobState::kPreparing &&
                      !replayed.jobs[0].process_exit_confirmed,
                  name, "failure/shutdown manufactured a process result or resolved the Job");
  return result;
}

int ProductionRunnerCompletionBudget() {
  constexpr std::string_view name = "rest_v1_protocol_less_process_fails";
  RegistryFixture fixture("issue141-completion-budget-");
  sitometron::test::process::Control control;
  fixture.bundle = fixture.root / "budget helper.exe";
  auto& application = fixture.document["applications"][0];
  application["executable"] = RegistryFixture::Text(fixture.bundle);
  application["bundle_sha256"] = CopyRegisteredProcessHelper(fixture.bundle);
  application["arguments"] = RegisteredProcessArguments(fixture, control.port());
  fixture.Save();
  auto config = fixture.Config();
  config.max_jobs = 1;
  config.hold_runner_observations_for_test = true;
  SetRegistryPath(config, fixture.file);
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error))
    return Check(false, name, "completion-budget startup refused: " + error);
  {
    // Real verified-registry window: preserve the recorded startup digest, change the image
    // before its first launch, and require the current valid ELF/PE bytes to start unchanged.
    std::ofstream different(fixture.bundle, std::ios::binary | std::ios::app);
    different.put('\0');
    different.close();
    if (!different) return Check(false, name, "cannot replace verified helper after startup");
  }
  const auto created =
      SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("demo-worker")));
  int result = ExpectStatus(created, name, 202);
  if (created.status != 202) return result;
  daemon.WaitForRunnerObservationForTest();
  (void)control.Ready();
  result |= Check(daemon.FillOtherCompletionBudgetForTest(), name,
                  "could not retain the full pre-runner completion budget");
  daemon.ReleaseRunnerObservationForTest();
  control.Exit();
  result |= Check(daemon.WaitForRunnerExitForTest() && daemon.WaitForSupervisorIdleForTest(), name,
                  "runner exit exhausted completion storage held by other producers");
  const auto id = Json::parse(created.body).at("job_id").get<std::string>();
  const auto job = daemon.backend().GetJob(id);
  result |=
      Check(job && job->state == "failed" && job->terminal && daemon.backend().GetReadiness().ready,
            name, "completion budget pressure latched failure or lost terminal cleanup");
  daemon.Stop();
  return result;
}

int ProtocolLessProcessFails() {
  constexpr std::string_view name = "rest_v1_protocol_less_process_fails";
  RegistryFixture fixture("issue141-protocol-less-");
  const auto executable = std::filesystem::path(SITOMETRON_PROCESS_TEST_CHILD);
  if (!std::filesystem::is_regular_file(executable))
    return Check(false, name, "built test child executable missing");
  boost::hash2::sha2_256 hasher;
  {
    std::ifstream input(executable, std::ios::binary);
    std::array<char, 65536> bytes{};
    while (input) {
      input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (input.gcount() > 0) hasher.update(bytes.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof() || input.bad()) return Check(false, name, "cannot hash built test child");
  }
  fixture.document["applications"][0]["executable"] = RegistryFixture::Text(executable);
  fixture.document["applications"][0]["bundle_sha256"] = boost::hash2::to_string(hasher.result());
  const auto report = fixture.root / "process report.json";
  fixture.document["applications"][0]["arguments"] =
      Json::array({"--mode", "exit", "--control-port", "", "--status", "31", "--report",
                   RegistryFixture::Text(report)});
  fixture.Save();
  auto config = fixture.Config();
  SetRegistryPath(config, fixture.file);
  sitometron::daemon::Phase1Composition daemon(config);
  std::string error;
  if (!daemon.Start(error)) return Check(false, name, "verified startup refused: " + error);
  const auto created =
      SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody("demo-worker")));
  int result = ExpectStatus(created, name, 202);
  std::string id;
  if (created.status == 202) {
    id = Json::parse(created.body).at("job_id").get<std::string>();
    result |= Check(created.headers.at("location") == JobPath(id), name, "create Location changed");
    result |= Check(ProcessCompositionDrained(daemon), name, "runner/supervisor drain failed");
    const auto read = SendToPort(daemon.listen_port(), Request("GET", JobPath(id)));
    result |= ExpectStatus(read, name, 200);
    if (read.status == 200) {
      const auto resource = Json::parse(read.body);
      result |= Check(resource.at("state") == "failed" && resource.at("terminal") == true &&
                          resource.at("outcome") == "failed",
                      name, "protocol-less exiting process must end failed, not wait at preparing");
    }
  }
  daemon.Stop();  // Joins producer and reaps before inspecting or deleting test files.
  std::size_t observations = 0;
  std::size_t exits = 0;
  std::size_t cleanups = 0;
  std::size_t worker_facts = 0;
  for (const auto& file : std::filesystem::directory_iterator(config.journal_path)) {
    if (file.path().extension() != ".ndjson") continue;
    std::ifstream input(file.path(), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
      const auto event = Json::parse(line);
      if (event.at("job_id") != id) continue;
      const auto kind = event.at("event_type").get<std::string>();
      if (kind == "worker_launch_observed") ++observations;
      if (kind == "process_exit_confirmed") ++exits;
      if (kind == "cleanup_status_recorded") {
        ++cleanups;
        result |= Check(event.at("payload").at("status") == "completed", name,
                        "process cleanup status was not recorded completed");
      }
      if (kind == "worker_running" || kind == "worker_completed" || kind == "worker_failed")
        ++worker_facts;
    }
  }
  result |= Check(observations == 1 && exits == 1 && cleanups == 1 && worker_facts == 0, name,
                  "expected one started observation, confirmed exit and cleanup; no Worker fact");
  const auto replayed = sitometron::journal::ReplayJournal(
      sitometron::journal::SystemFileSystem(), config.journal_path,
      sitometron::journal::ReplayOptions{config.max_jobs});
  result |= Check(replayed.status == sitometron::journal::ReplayStatus::kReplayed &&
                      replayed.jobs.size() == 1 && replayed.unresolved.empty(),
                  name, "protocol-less process did not leave a resolved replayable Job");
  if (replayed.jobs.size() == 1) {
    const auto& snapshot = replayed.jobs[0];
    result |= Check(snapshot.state == core::JobState::kFailed && snapshot.process_exit_confirmed &&
                        snapshot.cleanup_status == core::CleanupStatus::kCompleted,
                    name, "persisted Job lacks failed outcome, confirmed exit or recorded cleanup");
  }
  result |= Check(std::filesystem::is_regular_file(report), name,
                  "registered executable never wrote its process report");
  result |= ProductionRunnerFailureSealing();
  result |= ProductionRunnerCompletionBudget();
  return result;
}

#endif
