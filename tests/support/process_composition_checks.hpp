#ifndef SITOMETRON_TEST_PROCESS_COMPOSITION_CHECKS_HPP_
#define SITOMETRON_TEST_PROCESS_COMPOSITION_CHECKS_HPP_

#include "registry_composition_checks.hpp"

// Included in http_rest_v1_test.cpp's job_routes namespace after registry fixtures.
// The optional deterministic drain is test observation only, not a production state API.
template <typename Composition>
bool ProcessCompositionDrained(Composition& daemon) {
  if constexpr (requires { daemon.WaitForProcessIdleForTest(); })
    return daemon.WaitForProcessIdleForTest() && daemon.WaitForSupervisorIdleForTest();
  else
    return daemon.WaitForSupervisorIdleForTest();
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
  return result;
}

#endif
