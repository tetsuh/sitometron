#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "process_child_platform.hpp"
#include "process_control_test_support.hpp"
#include "process_handoff_allocation_probe.hpp"
#include "process_runner_test_support.hpp"

namespace {
namespace test = sitometron::test::process;
namespace core = sitometron::core;
using Json = nlohmann::json;
using test::Require;

constexpr std::string_view kDigest =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
core::ApplicationLaunchRequest Launch(unsigned number = 1) {
  const auto suffix = std::to_string(number);
  return {
      core::Uuid{"01890f30-7b54-7cc3-98c4-" + std::string(12 - suffix.size(), '0') + suffix},
      core::WorkerLaunchIntentPayload{
          core::StableId{"launch-op-" + suffix}, core::StableId{"test-child"}, "1",
          core::Digest{std::string(kDigest)}, core::StableId{"allocation-1"},
          core::Digest{std::string(kDigest)},
          core::Uuid{"550e8400-e29b-41d4-a716-" + std::string(12 - suffix.size(), '0') + suffix}}};
}
std::string MissingExecutable() {
  static const test::Directory directory;
  return test::PathText(directory.path() / "missing executable");
}
core::ApplicationStopRequest Stop(unsigned number = 1) {
  const auto launch = Launch(number);
  return {launch.job_id, launch.intent.operation_id, launch.intent.worker_id};
}
struct Fixture {
  test::Directory directory;
  test::Control control;
  test::Environment report;
  test::Environment port;
  test::Environment mode;
  test::Environment status;
  std::unique_ptr<test::Driver> driver;
  explicit Fixture(test::Options options, std::string_view missing_reason,
                   std::string child_mode = "wait", int child_status = 0)
      : report("ISSUE141_TEST_REPORT", test::PathText(directory.path() / "report.json")),
        port("ISSUE141_TEST_CONTROL_PORT", control.port()),
        mode("ISSUE141_TEST_MODE", std::move(child_mode)),
        status("ISSUE141_TEST_STATUS", std::to_string(child_status)) {
    driver = test::MakeDriver(options);
    Require(driver != nullptr,
            std::string(missing_reason) + ": sitometron_process production adapter is absent");
  }
  ~Fixture() {
    if (driver) {
      driver->Release();
      driver->RequestStop();
      driver->Join();
    }
    control.ExitAll();
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};
test::Options Options(std::string_view executable, std::string hold = {}) {
  test::Options options;
  options.specifications.push_back(
      {"test-child", std::string(executable), {}, std::string(kDigest)});
  options.hold_at = std::move(hold);
  return options;
}
std::vector<test::Attempt> Facts(test::Driver& driver, std::string_view kind) {
  std::vector<test::Attempt> result;
  for (const auto& attempt : driver.Attempts())
    if (attempt.event.event_type == kind && attempt.answer.admission == test::Admission::kAdmitted)
      result.push_back(attempt);
  return result;
}
void ExactFacts(test::Driver& driver, std::size_t observations, std::size_t exits,
                std::string_view mode = {}) {
  const auto observed = Facts(driver, "worker_launch_observed");
  const auto exited = Facts(driver, "process_exit_confirmed");
  Require(observed.size() == observations && exited.size() == exits,
          "wrong accepted observation/exit cardinality");
  for (const auto& fact : exited) {
    const auto payload = Json::parse(fact.event.payload_json);
    if (!mode.empty())
      Require(payload.at("completion_mode") == std::string(mode), "wrong exit mode");
    const auto requested = Launch(static_cast<unsigned>(
        std::stoul(fact.event.job_id.value.substr(fact.event.job_id.value.size() - 2))));
    Require(payload.at("launch_operation_id") == requested.intent.operation_id.value,
            "exit changed the launch operation identity");
  }
  for (const auto& attempt : driver.Attempts())
    Require(attempt.event.event_type == "worker_launch_observed" ||
                attempt.event.event_type == "process_exit_confirmed",
            "runner manufactured a Worker protocol event");
}
void ReapedAndJoined(test::Driver& driver) {
  driver.Release();
  driver.RequestStop();
  driver.Join();
  const auto view = driver.Inspect();
  Require(view.processes_created == view.processes_reaped, "owned child was not reaped");
  Require(view.threads_created == view.threads_joined, "runner thread outlived join");
}

void LaunchObservedAfterStart(const std::string& child) {
  // These vectors also run against the pure Windows serializer on Linux.
  const std::array quoting = {
      std::pair{std::string("word"), std::string("word")},
      std::pair{std::string{}, std::string("\"\"")},
      std::pair{std::string("two words"), std::string("\"two words\"")},
      std::pair{std::string("a\tb"), std::string("\"a\tb\"")},
      std::pair{std::string("a\"b"), std::string("\"a\\\"b\"")},
      std::pair{std::string("tail\\"), std::string("tail\\")},
      std::pair{std::string("space tail\\"), std::string("\"space tail\\\\\"")},
      std::pair{std::string("a\\\"b"), std::string("\"a\\\\\\\"b\"")}};
  const std::vector<std::vector<std::string>> shapes{
      {},
      {"--plain", "batch"},
      {"", "two words", "tab\tvalue", "a\"b", "tail\\", "a\\\"b"},
      {"; literal", "$(literal)", "a|b", "*", "あ"}};
  for (const auto& parameters : shapes) {
    auto options = Options(child);
    options.specifications[0].arguments = parameters;
    test::Environment sentinel("ISSUE141_TEST_SENTINEL", "preserved-value");
    test::Environment conflicting_job("SITOMETRON_JOB_ID", "wrong-parent-job");
    test::Environment conflicting_worker("SITOMETRON_WORKER_ID", "wrong-parent-worker");
    test::Environment conflicting_operation("SITOMETRON_LAUNCH_OPERATION_ID", "wrong-parent-op");
    Fixture fixture(options, "launch must start the executable before reporting started");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    const auto report = test::ReadReport(fixture.directory.path() / "report.json");
    auto expected = parameters;
    expected.insert(expected.begin(), child);
    Require(report.at("argv") == expected, "registered parameters changed argv boundaries/order");
    Require(report.at("cwd") == test::PathText(std::filesystem::current_path()), "CWD changed");
    Require(report.at("sentinel") == "preserved-value", "inherited environment was discarded");
    Require(report.at("job_id") == Launch().job_id.value &&
                report.at("worker_id") == Launch().intent.worker_id.value &&
                report.at("operation_id") == Launch().intent.operation_id.value,
            "identity environment did not override inherited values");
    const auto facts = Facts(*fixture.driver, "worker_launch_observed");
    Require(Json::parse(facts.at(0).event.payload_json).at("outcome") == "started",
            "OS-created child was not observed started");
    fixture.control.Exit();
    fixture.driver->WaitFacts(1, 1);
    ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
    ReapedAndJoined(*fixture.driver);
    for (const auto& [input, expected_text] : quoting)
      Require(test::QuoteWindowsArgument(input) == expected_text, "Windows quoting vector differs");
  }
}
void FailedStartReportsFailed(const std::string& child) {
  for (const std::string cause : {"missing", "unreadable", "unknown-application"}) {
    test::Directory files;
    const auto executable = files.path() / "unreadable executable.exe";
    auto options = Options(test::PathText(executable));
    std::unique_ptr<test::FileReadBlock> unreadable;
    if (cause == "unreadable") {
      std::filesystem::copy_file(child, executable);
      unreadable = test::DenyExecutableRead(test::PathText(executable));
    }
    if (cause == "unknown-application") options.specifications.clear();
    Fixture fixture(options, "failed start must report failed without inventing an exit fact");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitFacts(1, 0);
    fixture.driver->WaitPoint("cell_released");
    ExactFacts(*fixture.driver, 1, 0);
    Require(Json::parse(Facts(*fixture.driver, "worker_launch_observed").at(0).event.payload_json)
                    .at("outcome") == "failed",
            "pre-creation failure reported started");
    Require(fixture.driver->Inspect().processes_created == 0, "failed launch created a process");
    ReapedAndJoined(*fixture.driver);
  }
  test::Directory files;
#ifdef _WIN32
  const auto executable = files.path() / "replaced child.exe";
#else
  const auto executable = files.path() / "replaced child";
#endif
  std::filesystem::copy_file(child, executable);
  auto options = Options(test::PathText(executable), "before_spawn");
  // A deliberately different recorded digest cannot prevent launching the current file.
  options.specifications[0].bundle_sha256 = std::string(64, '0');
  Fixture fixture(options, "replacement after startup must be started without a digest recheck");
  fixture.driver->Launch(Launch());
  fixture.driver->WaitHeld();
  std::filesystem::remove(executable);
  std::filesystem::copy_file(child, executable);
  fixture.driver->Release();
  (void)fixture.control.Ready();
  fixture.driver->WaitFacts(1, 0);
  fixture.control.Exit();
  fixture.driver->WaitFacts(1, 1);
  ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
  ReapedAndJoined(*fixture.driver);
}
void ExitConfirmedMode(const std::string& child) {
  for (const std::string order :
       {"natural", "cooperative", "forced", "cooperative-forced", "forced-cooperative"}) {
    auto options = Options(child, order == "natural" ? "" : "before_stop_delivery");
    Fixture fixture(options, "process exit must report the strongest requested stop mode",
                    "ignore-term", 23);
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    if (order == "natural") {
      fixture.control.Exit();
    } else {
      if (order == "forced" || order == "forced-cooperative") fixture.driver->Forced(Stop());
      if (order != "forced") fixture.driver->Cooperative(Stop());
      if (order == "cooperative-forced") fixture.driver->Forced(Stop());
      fixture.driver->WaitHeld();
      fixture.driver->Release();
#ifndef _WIN32
      if (order == "cooperative") {
        fixture.control.TermObserved();
        fixture.control.Exit();
      }
#endif
    }
    fixture.driver->WaitFacts(1, 1);
    const auto mode = order == "natural"       ? "process_already_exited"
                      : order == "cooperative" ? "cooperative"
                                               : "forced";
    ExactFacts(*fixture.driver, 1, 1, mode);
    ReapedAndJoined(*fixture.driver);
  }
}
void StopBeforeStartSkipsLaunch(const std::string& child) {
  for (const std::string stop : {"cooperative", "forced", "both", "duplicate"}) {
    Fixture fixture(Options(child, "before_spawn_check"),
                    "a pre-start stop must skip spawn and report exactly one exit");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    if (stop != "forced") fixture.driver->Cooperative(Stop());
    if (stop == "forced" || stop == "both") fixture.driver->Forced(Stop());
    if (stop == "duplicate") fixture.driver->Cooperative(Stop());
    fixture.driver->Release();
    fixture.driver->WaitFacts(0, 1);
    fixture.driver->WaitPoint("cell_released");
    ExactFacts(*fixture.driver, 0, 1,
               stop == "forced" || stop == "both" ? "forced" : "cooperative");
    Require(fixture.driver->Inspect().processes_created == 0, "pre-start stop still spawned");
    ReapedAndJoined(*fixture.driver);
  }
}
void StopDelivery(const std::string& child) {
  Fixture fixture(Options(child, "before_stop_delivery"),
                  "stop flags must be delivered once in cooperative-then-forced order",
                  "ignore-term");
  fixture.driver->Launch(Launch());
  (void)fixture.control.Ready();
  fixture.driver->WaitFacts(1, 0);
  fixture.driver->Cooperative(Stop());
  fixture.driver->Cooperative(Stop());
  fixture.driver->Forced(Stop());
  fixture.driver->Forced(Stop());
  fixture.driver->WaitHeld();
  fixture.driver->Release();
  fixture.driver->WaitFacts(1, 1);
  fixture.driver->WaitPoint("cell_released");
  const auto before = fixture.driver->Inspect();
  Require(before.cooperative_deliveries == 1 && before.forced_deliveries == 1,
          "duplicate stop reached the operating system");
  Require(before.delivery_order == std::vector<std::string>{"cooperative", "forced"},
          "pending stops were delivered in the wrong order");
  fixture.driver->Cooperative(Stop());
  fixture.driver->Forced(Stop());
  fixture.driver->Forced(Stop(99));
  ReapedAndJoined(*fixture.driver);
  const auto after = fixture.driver->Inspect();
  Require(after.cooperative_deliveries == before.cooperative_deliveries &&
              after.forced_deliveries == before.forced_deliveries,
          "released/reaped/unknown Job received an OS stop");
  for (const bool forced : {false, true}) {
    Fixture family(Options(child), "stops must target only the created process, never descendants",
                   "descendant-parent");
    struct DescendantGuard {
      test::Driver& runner;
      test::DescendantOwnership ownership;
      explicit DescendantGuard(test::Driver& value) : runner(value) {}
      ~DescendantGuard() {
        runner.Release();
        runner.RequestStop();
        runner.Join();
        // ownership then stops/reaps the test-owned descendant, after its parent was reaped.
      }
    } guard(*family.driver);
    family.driver->Launch(Launch());
    (void)family.control.Ready();
    const auto descendant_index = family.control.Ready();
    guard.ownership.Adopt(family.control.ProcessId(descendant_index));
    const auto report = test::ReadReport(family.directory.path() / "report.json.desc");
    Require(report.at("process_id") == family.control.ProcessId(descendant_index),
            "descendant report differs from its test control identity");
    family.driver->WaitFacts(1, 0);
    if (forced)
      family.driver->Forced(Stop());
    else
      family.driver->Cooperative(Stop());
    family.driver->WaitFacts(1, 1);
    family.driver->WaitPoint("cell_released");
    Require(guard.ownership.Alive(), "runner signaled a descendant of its created process");
    ExactFacts(*family.driver, 1, 1, forced ? "forced" : "cooperative");
  }
}
void HandoffBounded(const std::string& child) {
  const auto allocation_before = test::HandoffAllocationCount();
  {
    const test::HandoffAllocationScope probe;
    void* memory = ::operator new(1);
    ::operator delete(memory);
  }
  Require(test::HandoffAllocationCount() == allocation_before + 1,
          "handoff allocation probe did not detect its planted allocation");
  {
    Fixture fixture(Options(child, "mailbox_lock_held"),
                    "handoffs must return while the producer holds its mailbox lock");
    fixture.driver->Launch(Launch(1));
    fixture.driver->WaitHeld();
    fixture.driver->Cooperative(Stop(1));
    fixture.driver->Forced(Stop(1));
    fixture.driver->Launch(Launch(2));
    // Reaching this point before Release proves neither stop nor launch took that lock.
    fixture.driver->Release();
    ReapedAndJoined(*fixture.driver);
  }
  {
    Fixture fixture(Options(child, "before_spawn_check"),
                    "handoffs need bounded two-cell storage and construction-time threads");
    const auto initial = fixture.driver->Inspect();
    fixture.driver->Launch(Launch(1));
    fixture.driver->Launch(Launch(2));
    fixture.driver->WaitHeld(2);
    fixture.driver->Cooperative(Stop(1));
    fixture.driver->Forced(Stop(2));
    fixture.driver->Forced(Stop(99));
    Require(fixture.driver->Inspect().occupied == 2, "max_jobs+1 launches lost a mailbox");
    fixture.driver->Release();
    fixture.driver->WaitFacts(0, 2);
    fixture.driver->WaitPoint("cell_released", 2);
    const auto view = fixture.driver->Inspect();
    Require(view.threads_created == initial.threads_created && view.threads_created == 4,
            "launch created a thread or omitted a fixed producer thread");
    Require(view.handoff_allocations == 0 && view.handoff_locks == 0 && view.handoff_ingress == 0,
            "handoff allocated, locked or re-entered ingress");
    Require(view.max_completions_per_thread <= 1, "thread retained two completions");
    ReapedAndJoined(*fixture.driver);
  }
  {
    Fixture fixture(Options(child, "before_final_completion"),
                    "retirement must offer the closing cell's stable partner", "exit");
    fixture.driver->Launch(Launch(1));
    fixture.driver->Launch(Launch(2));
    fixture.driver->WaitHeld(2);
    Require(fixture.driver->Inspect().closing == 2, "last fact did not enter closing first");
    fixture.driver->Launch(Launch(3));
    Require(fixture.driver->Inspect().occupied == 3,
            "three-Job max_jobs=1 interleaving lost the third launch");
    fixture.driver->Cooperative(Stop(3));
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released", 3);
    ReapedAndJoined(*fixture.driver);
  }
  for (const std::string point : {"stop_before_cas", "stop_between_scan_reads"}) {
    Fixture fixture(Options(child, point),
                    "stop scan/release interleaving must preserve generation");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    const unsigned target = point == "stop_before_cas" ? 1 : 2;
    if (target == 2) {
      fixture.driver->Launch(Launch(2));
      (void)fixture.control.Ready();
    }
    fixture.driver->WaitFacts(target, 0);
    struct Stopper {
      test::Driver& driver;
      std::thread thread;
      Stopper(test::Driver& value, unsigned number)
          : driver(value), thread([&value, number] { value.Cooperative(Stop(number)); }) {}
      ~Stopper() {
        driver.Release();
        if (thread.joinable()) thread.join();
      }
    } stopper(*fixture.driver, target);
    fixture.driver->WaitHeld();
    fixture.control.Exit(target - 1);
    fixture.driver->WaitFacts(target, 1);
    fixture.driver->WaitPoint("cell_released");
    fixture.driver->Release();
    stopper.thread.join();
    if (point == "stop_before_cas") fixture.driver->WaitPoint("stop_before_rescan");
    // The writer serializes handoffs: only the slot's release can overlap the stop.
    // Reuse follows that stop's return, never a second concurrent writer.
    fixture.driver->Launch(Launch(3));
    const auto replacement = fixture.control.Ready();
    fixture.control.Exit(replacement);
    fixture.driver->WaitFacts(target + 1, 2);
    for (const auto& attempt : fixture.driver->Attempts())
      if (attempt.event.job_id == Launch(3).job_id &&
          attempt.event.event_type == "process_exit_confirmed")
        Require(Json::parse(attempt.event.payload_json).at("completion_mode") ==
                    "process_already_exited",
                "the released Job's stop flag reached its cell's next occupant");
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().invariant_failures == 0 &&
                fixture.driver->Inspect().cooperative_deliveries == 0,
            "stale stop reached a reused cell or corrupted the table");
  }
  {
    Fixture fixture(Options(child, "before_spawn_check"),
                    "mailbox exhaustion must latch failure outside the handoff");
    fixture.driver->Launch(Launch(1));
    fixture.driver->Launch(Launch(2));
    fixture.driver->WaitHeld(2);
    fixture.driver->Launch(Launch(3));  // Deliberately violate the resident-bound premise.
    fixture.driver->WaitPoint("failure_latched");
    Require(fixture.driver->Inspect().invariant_failures == 1, "overflow was silently discarded");
    ReapedAndJoined(*fixture.driver);
  }
}
void NormalFullWaits(const std::string& child) {
  {
    auto options = Options(child, "after_park");
    options.observations = {{test::Admission::kNormalFull}};
    Fixture fixture(options,
                    "parked observation must not delay another Job's exit or stop delivery");
    fixture.driver->Launch(Launch(1));
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld();
    fixture.driver->Launch(Launch(2));
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    fixture.control.Exit(1);
    fixture.driver->WaitFacts(1, 1);
    fixture.driver->Forced(Stop(1));
    fixture.driver->WaitPoint("stop_delivered");
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released", 2);
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().forced_deliveries == 1,
            "another Job or parked normal observation blocked the service thread");
  }
  for (const std::string gap : {"before_park", "after_park", "relay_before_wait"}) {
    auto options = Options(child, gap);
    options.observations = {{test::Admission::kNormalFull}, {test::Admission::kAdmitted}};
    Fixture fixture(options,
                    "normal_full must park only the observation behind its own capacity flag");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld();
    // Capacity freed before the park and between scan/wait must survive independently.
    fixture.driver->Capacity(2);  // relay for max_jobs=1: two slots then relay.
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 0);
    const auto attempts = fixture.driver->Attempts();
    Require(
        attempts.size() >= 2 && attempts.front().answer.admission == test::Admission::kNormalFull,
        "normal_full fixture never refused the first observation");
    const auto accepted = Facts(*fixture.driver, "worker_launch_observed");
    Require(accepted.at(0).thread != attempts.front().thread,
            "live slot retried instead of handing the observation to relay");
    fixture.control.Exit();
    fixture.driver->WaitFacts(1, 1);
    ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(child, "relay_before_submit");
    options.observations = {{test::Admission::kNormalFull}};
    Fixture fixture(options, "exit while parked must drop the started retry");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld();
    fixture.control.Exit();
    fixture.driver->WaitPoint("process_gone");
    fixture.driver->Release();
    fixture.driver->WaitFacts(0, 1);
    ExactFacts(*fixture.driver, 0, 1, "process_already_exited");
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(MissingExecutable(), "failed_before_capacity_wait");
    options.observations = {{test::Admission::kNormalFull}, {test::Admission::kAdmitted}};
    Fixture fixture(options,
                    "failed normal_full must retain its cell and retry on the slot thread");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    Require(fixture.driver->Inspect().occupied == 1, "failed retry released its obligation");
    fixture.driver->Capacity(0);
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 0);
    fixture.driver->WaitPoint("cell_released");
    const auto attempts = fixture.driver->Attempts();
    Require(attempts.size() == 2 && attempts[0].thread == attempts[1].thread,
            "failed observation was retried by another thread");
    ExactFacts(*fixture.driver, 1, 0);
    ReapedAndJoined(*fixture.driver);
  }
}
void ExitReportedAfterRejectedObservation(const std::string& child) {
  for (const bool started : {false, true}) {
    for (const std::string stop : {"cooperative", "forced", "both"}) {
      auto options = Options(started ? child : MissingExecutable(), "before_observation");
      options.observations = {{test::Admission::kAdmitted, test::Completion::kRejected}};
      Fixture fixture(options,
                      "rejected launch observation still owes one exit after leaving preparing",
                      "ignore-term");
      fixture.driver->Launch(Launch());
      if (started) (void)fixture.control.Ready();
      fixture.driver->WaitHeld();
      if (stop != "forced") fixture.driver->Cooperative(Stop());
      if (stop != "cooperative") fixture.driver->Forced(Stop());
      fixture.driver->Release();
#ifndef _WIN32
      if (started && stop == "cooperative") {
        fixture.control.TermObserved();
        fixture.control.Exit();
      }
#endif
      fixture.driver->WaitFacts(1, 1);
      ExactFacts(*fixture.driver, 1, 1, stop == "cooperative" ? "cooperative" : "forced");
      ReapedAndJoined(*fixture.driver);
    }
  }
  for (const std::string landing : {"spawn_before_publication", "slot_waiting_for_exit"}) {
    Fixture fixture(Options(child, landing),
                    "stops during spawn or exit wait must be delivered without slot re-entry",
                    "ignore-term");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    (void)fixture.control.Ready();
    fixture.driver->Cooperative(Stop());
    fixture.driver->Forced(Stop());
    if (landing == "slot_waiting_for_exit") fixture.driver->WaitPoint("stop_delivered", 2);
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 1);
    const auto view = fixture.driver->Inspect();
    Require(view.cooperative_deliveries == 1 && view.forced_deliveries == 1,
            "spawn/service delivery duplicated or lost a flag");
    ReapedAndJoined(*fixture.driver);
  }
}
void ReapedAfterFailure(const std::string& child) {
  {
    auto options = Options(child);
    options.observations = {{test::Admission::kAdmitted, test::Completion::kMissing}};
    Fixture fixture(options,
                    "an admitted input with no completion must report an invariant breach");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint("missing_completion_reported");
    ReapedAndJoined(*fixture.driver);
    Require(Facts(*fixture.driver, "process_exit_confirmed").empty(),
            "missing completion was silently treated as an accepted start");
  }
  const std::array failures = {
      test::Answer{test::Admission::kServiceFailed},
      test::Answer{test::Admission::kAdmissionClosed},
      test::Answer{test::Admission::kAdmitted, test::Completion::kServiceFailed}};
  for (const auto& result : failures) {
    for (const bool during_observation : {false, true}) {
      auto options = Options(child);
      if (during_observation)
        options.observations = {result};
      else
        options.exits = {result};
      Fixture fixture(options, "closed/failed ingress must never abandon an owned process");
      fixture.driver->Launch(Launch());
      (void)fixture.control.Ready();
      fixture.driver->WaitPoint("observation_answered");
      Require(fixture.driver->Inspect().processes_reaped == 0 &&
                  fixture.driver->Inspect().forced_deliveries == 0,
              "admission failure force-stopped a live process");
      fixture.control.Exit();
      fixture.driver->WaitPoint("cell_released");
      ReapedAndJoined(*fixture.driver);
      if (during_observation)
        Require(Facts(*fixture.driver, "process_exit_confirmed").empty(),
                "closed observation was followed by another fact");
    }
  }
  {
    auto options = Options(child, "failed_before_capacity_wait");
    options.specifications.push_back(
        {"missing-child", MissingExecutable(), {}, std::string(kDigest)});
    options.observations = {{test::Admission::kAdmitted}, {test::Admission::kNormalFull}};
    Fixture fixture(options, "core closure must wake all retries without stopping a live process");
    fixture.driver->Launch(Launch(1));
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    auto missing = Launch(2);
    missing.intent.application_id.value = "missing-child";
    fixture.driver->Launch(std::move(missing));
    fixture.driver->WaitHeld();
    fixture.driver->CloseAdmission();
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released");
    const auto closed = fixture.driver->Inspect();
    Require(
        closed.forced_deliveries == 0 && closed.threads_joined == 0 && closed.processes_reaped == 0,
        "admission closure alone forced/stopped/joined the live child");
    fixture.control.Exit();
    fixture.driver->WaitPoint("cell_released", 2);
    ReapedAndJoined(*fixture.driver);
  }
  {
    Fixture fixture(Options(child, "before_final_completion"),
                    "shutdown must drop a launch still queued behind a closing cell", "exit");
    fixture.driver->Launch(Launch(1));
    (void)fixture.control.Ready();
    fixture.driver->Launch(Launch(2));
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld(2);
    fixture.driver->Launch(Launch(3));
    fixture.driver->RequestStop();
    fixture.driver->Release();
    fixture.driver->Join();
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().processes_created == 2, "queued shutdown launch was spawned");
    for (const auto& attempt : fixture.driver->Attempts())
      Require(attempt.event.job_id != Launch(3).job_id, "queued shutdown launch submitted a fact");
  }
  {
    Fixture fixture(Options(child, "before_reap"),
                    "shutdown must force every live process without waiting for another reap");
    fixture.driver->Launch(Launch(1));
    (void)fixture.control.Ready();
    fixture.driver->Launch(Launch(2));
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(2, 0);
    fixture.driver->RequestStop();
    fixture.driver->WaitHeld();
    fixture.driver->WaitPoint("stop_delivered", 2);
    Require(fixture.driver->Inspect().forced_deliveries == 2,
            "shutdown waited for one child before forcing the other");
    ReapedAndJoined(*fixture.driver);
  }
  const std::array unexpected = {test::Admission::kCoalescedPending,
                                 test::Admission::kAlreadyPending, test::Admission::kResidentLimit,
                                 test::Admission::kNormalFull};
  for (const auto answer : unexpected) {
    for (const bool observation : {false, true}) {
      if (observation && answer == test::Admission::kNormalFull) continue;
      auto options = Options(child);
      if (observation)
        options.observations = {{answer}};
      else
        options.exits = {{answer}};
      Fixture fixture(options, "unexpected ingress answers must not duplicate runner facts");
      fixture.driver->Launch(Launch());
      (void)fixture.control.Ready();
      fixture.driver->WaitPoint("observation_answered");
      fixture.control.Exit();
      fixture.driver->WaitPoint("cell_released");
      std::size_t observations = 0;
      std::size_t exits = 0;
      for (const auto& attempt : fixture.driver->Attempts()) {
        if (attempt.event.event_type == "worker_launch_observed") ++observations;
        if (attempt.event.event_type == "process_exit_confirmed") ++exits;
      }
      Require(observations == 1 && exits <= 1,
              "non-retryable ingress answer retried an observation or critical exit");
      ReapedAndJoined(*fixture.driver);
    }
  }
  for (const std::string point :
       {"before_spawn_check", "spawn_before_publication", "slot_waiting_for_exit",
        "relay_before_wait", "failed_before_capacity_wait"}) {
    const bool failed = point == "failed_before_capacity_wait";
    auto options = Options(failed ? MissingExecutable() : child, point);
    if (failed) options.observations = {{test::Admission::kNormalFull}};
    Fixture fixture(
        options, "shutdown must wake waiters, drop unspawned work, force and reap live children");
    if (point != "relay_before_wait") fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    fixture.driver->RequestStop();
    fixture.driver->Release();
    fixture.driver->Join();
    ReapedAndJoined(*fixture.driver);
    if (point == "before_spawn_check") {
      Require(
          fixture.driver->Inspect().processes_created == 0 && fixture.driver->Attempts().empty(),
          "shutdown-before-spawn started a process or submitted a fact");
    }
  }
  {
    auto options = Options(MissingExecutable(), "failed_before_capacity_wait");
    options.observations = {{test::Admission::kNormalFull}};
    Fixture fixture(
        options, "core closure without runner stop must wake every failed retry without joining");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    fixture.driver->CloseAdmission();
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released");
    Require(Facts(*fixture.driver, "process_exit_confirmed").empty() &&
                fixture.driver->Inspect().threads_joined == 0,
            "core closure submitted an exit or joined runner threads");
    ReapedAndJoined(*fixture.driver);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sitometron_process_tests <check> <built-test-child>\n";
    return 2;
  }
  const std::string name = argv[1];
  try {
    const std::string child = test::PathText(std::filesystem::absolute(argv[2]));
    Require(std::filesystem::is_regular_file(child), "built test child missing");
    if (name == "process_launch_observed_after_start")
      LaunchObservedAfterStart(child);
    else if (name == "process_failed_start_reports_failed")
      FailedStartReportsFailed(child);
    else if (name == "process_exit_confirmed_mode")
      ExitConfirmedMode(child);
    else if (name == "process_stop_before_start_skips_launch")
      StopBeforeStartSkipsLaunch(child);
    else if (name == "process_stop_delivery")
      StopDelivery(child);
    else if (name == "process_handoff_bounded")
      HandoffBounded(child);
    else if (name == "process_normal_full_waits")
      NormalFullWaits(child);
    else if (name == "process_exit_reported_after_rejected_observation")
      ExitReportedAfterRejectedObservation(child);
    else if (name == "process_reaped_after_failure")
      ReapedAfterFailure(child);
    else
      throw std::runtime_error("unknown process check");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << name << ": " << error.what() << '\n';
    return 1;
  }
}
