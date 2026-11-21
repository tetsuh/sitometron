#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
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

constexpr std::string_view k_digest =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
core::ApplicationLaunchRequest Launch(unsigned number = 1) {
  const auto suffix = std::to_string(number);
  return {
      core::Uuid{"01890f30-7b54-7cc3-98c4-" + std::string(12 - suffix.size(), '0') + suffix},
      core::WorkerLaunchIntentPayload{
          core::StableId{"launch-op-" + suffix}, core::StableId{"test-child"}, "1",
          core::Digest{std::string(k_digest)}, core::StableId{"allocation-1"},
          core::Digest{std::string(k_digest)},
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
  test::Environment nonce;
  std::unique_ptr<test::Driver> driver;
  explicit Fixture(const test::Options& options, std::string_view missing_reason,
                   std::string child_mode = "wait", int child_status = 0)
      : report("ISSUE141_TEST_REPORT", test::PathText(directory.path() / "report.json")),
        port("ISSUE141_TEST_CONTROL_PORT", control.port()),
        mode("ISSUE141_TEST_MODE", std::move(child_mode)),
        status("ISSUE141_TEST_STATUS", std::to_string(child_status)),
        nonce("ISSUE141_TEST_NONCE", control.nonce()) {
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
      {"test-child", std::string(executable), {}, std::string(k_digest)});
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
bool NoExitAttempts(test::Driver& driver) {
  for (const auto& attempt : driver.Attempts())
    if (attempt.event.event_type == "process_exit_confirmed") return false;
  return true;
}
void ExactFacts(test::Driver& driver, std::size_t observations, std::size_t exits,
                std::string_view mode = {}) {
  const auto submitted = driver.SubmittedLaunches();
  const auto compare = [&driver, &submitted, mode](std::string_view kind, std::size_t count) {
    const auto operation =
        kind == "process_exit_confirmed" ? "launch_operation_id" : "operation_id";
    const auto facts = Facts(driver, kind);
    Require(facts.size() == count && count <= submitted.size(),
            "wrong accepted observation/exit cardinality");
    std::set<std::pair<std::string, std::string>> expected;
    std::set<std::pair<std::string, std::string>> actual;
    for (std::size_t index = 0; index != count; ++index)
      Require(expected.emplace(submitted[index].job_id, submitted[index].operation_id).second,
              "fixture submitted duplicate expected Job/operation pairs");
    for (const auto& fact : facts) {
      const auto payload = Json::parse(fact.event.payload_json);
      Require(
          actual.emplace(fact.event.job_id.value, payload.at(operation).get<std::string>()).second,
          "duplicate settled Job/operation fact");
      if (kind == "process_exit_confirmed" && !mode.empty())
        Require(payload.at("completion_mode") == std::string(mode), "wrong exit mode");
    }
    Require(actual == expected, "settled facts have a missing or misrouted Job/operation pair");
  };
  compare("worker_launch_observed", observations);
  compare("process_exit_confirmed", exits);
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
  Require(view.start_order_breaches == 0,
          "started observation arrived before process-created state existed at fake ingress");
  Require(view.processes_created == view.processes_reaped, "owned child was not reaped");
  Require(view.threads_created == view.threads_joined, "runner thread outlived join");
  Require(view.registration_breaches == 0, "two producer threads reused one ingress registration");
  Require(view.undrained_requests == 0,
          "slot released a cell before moving out all request strings");
  Require(
      view.handoff_allocations == 0 && view.handoff_deallocations == 0 && view.handoff_locks == 0 &&
          view.handoff_ingress == 0,
      "handoff used C++ new/delete, a Sitometron-owned adapter/composition mutex, or a core entry; "
      "this probe does not measure hidden STL/CRT/OS locks");
}

void LaunchObservedAfterStart(const std::filesystem::path& child) {
  test::Directory copies;
  const auto unicode_path = copies.path() / std::filesystem::path(u8"子 process with spaces.exe");
  std::filesystem::copy_file(child, unicode_path);
  // These vectors also run against the pure Windows serializer on Linux.
  const std::array quoting = {
      std::pair{std::string("word"), std::string("word")},
      std::pair{std::string{}, std::string("\"\"")},
      std::pair{std::string("two words"), std::string("\"two words\"")},
      std::pair{std::string("a\tb"), std::string("\"a\tb\"")},
      std::pair{std::string("a\"b"), std::string("\"a\\\"b\"")},
      std::pair{std::string("tail\\"), std::string("tail\\")},
      std::pair{std::string("space tail\\"), std::string("\"space tail\\\\\"")},
      std::pair{std::string("a\\\"b"), std::string("\"a\\\\\\\"b\"")},
      std::pair{std::string("a") + std::string(3, '\\') + "\"b",
                std::string("\"a") + std::string(7, '\\') + "\"b\""},
      std::pair{std::string("space ") + std::string(3, '\\'),
                std::string("\"space ") + std::string(6, '\\') + "\""}};
  const std::vector<std::vector<std::string>> shapes{
      {},
      {"word"},
      {"--plain", "batch"},
      {"", "two words", "tab\tvalue", "a\"b", "tail\\", "a\\\"b"},
      {"; literal", "$(literal)", "a|b", "*", "あ"}};
#ifdef _WIN32
  // Windows treats environment keys case-insensitively: the inherited spelling
  // deliberately differs from the canonical keys the child reports.
  const std::array parent_keys{"sitometron_job_id", "sitometron_worker_id",
                               "sitometron_launch_operation_id"};
#else
  const std::array parent_keys{"SITOMETRON_JOB_ID", "SITOMETRON_WORKER_ID",
                               "SITOMETRON_LAUNCH_OPERATION_ID"};
#endif
  for (const auto& native_executable : std::array{child, unicode_path}) {
    const auto executable = test::PathText(native_executable);
    for (const bool conflicts : {false, true}) {
      for (const auto& parameters : shapes) {
        auto options = Options(executable);
        options.specifications[0].arguments = parameters;
        test::Environment sentinel("ISSUE141_TEST_SENTINEL", "preserved-value");
        test::Environment conflicting_job(
            parent_keys[0],
            conflicts ? std::optional<std::string>("wrong-parent-job") : std::nullopt);
        test::Environment conflicting_worker(
            parent_keys[1],
            conflicts ? std::optional<std::string>("wrong-parent-worker") : std::nullopt);
        test::Environment conflicting_operation(
            parent_keys[2],
            conflicts ? std::optional<std::string>("wrong-parent-op") : std::nullopt);
        Fixture fixture(options, "launch must start the executable before reporting started");
        fixture.driver->Launch(Launch());
        (void)fixture.control.Ready();
        fixture.driver->WaitFacts(1, 0);
        const auto report = test::ReadReport(fixture.directory.path() / "report.json");
        auto expected = parameters;
        expected.insert(expected.begin(), executable);
        Require(report.at("argv") == expected,
                "registered parameters changed argv boundaries/order");
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
          Require(test::QuoteWindowsArgument(input) == expected_text,
                  "Windows quoting vector differs");
      }
    }
  }
}
void FailedStartReportsFailed(const std::filesystem::path& child) {
  for (const std::string cause : {"missing", "unreadable", "unknown-application", "os-error"}) {
    test::Directory files;
    const auto executable = files.path() / "unreadable executable.exe";
    auto options = Options(test::PathText(executable));
    std::unique_ptr<test::FileReadBlock> unreadable;
    if (cause == "unreadable") {
      std::filesystem::copy_file(child, executable);
      unreadable = test::DenyExecutableRead(test::PathText(executable));
    }
    if (cause == "unknown-application") options.specifications.clear();
    if (cause == "os-error") {
      options.specifications[0].executable = test::PathText(child);
      options.fail_before_spawn = true;
    }
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
  {
    std::ofstream different(executable, std::ios::binary | std::ios::app);
    different.put('\0');  // A different valid ELF/PE image, not the original verified bytes.
    different.close();
    Require(static_cast<bool>(different), "replacement executable write failed");
  }
  fixture.driver->Release();
  (void)fixture.control.Ready();
  fixture.driver->WaitFacts(1, 0);
  fixture.control.Exit();
  fixture.driver->WaitFacts(1, 1);
  ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
  ReapedAndJoined(*fixture.driver);
}
void ExitConfirmedMode(const std::string& child) {
#ifndef _WIN32
  {
    const auto inherited = test::BlockAndIgnoreTerm();
    Require(test::TermBlocked() && test::TermIgnored(),
            "signal inheritance fixture not established");
    Fixture fixture(Options(child), "handler-less child must not inherit blocked/ignored SIGTERM",
                    "default-term");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    const auto report = test::ReadReport(fixture.directory.path() / "report.json");
    Require(!report.at("sigterm_blocked").get<bool>() && !report.at("sigterm_ignored").get<bool>(),
            "child inherited a blocked or ignored SIGTERM");
    fixture.driver->Cooperative(Stop());
    fixture.driver->WaitFacts(1, 1);
    ExactFacts(*fixture.driver, 1, 1, "cooperative");
    ReapedAndJoined(*fixture.driver);
    Require(test::TermBlocked() && test::TermIgnored(), "spawn changed the daemon signal state");
  }
#endif
  for (const std::string order : {"natural", "natural-zero", "cooperative", "forced",
                                  "cooperative-forced", "forced-cooperative"}) {
    const bool natural = order.starts_with("natural");
    auto options = Options(child, natural ? "" : "before_stop_delivery");
    Fixture fixture(options, "process exit must report the strongest requested stop mode",
                    "ignore-term", order == "natural-zero" ? 0 : 23);
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    if (natural) {
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
    const auto mode = natural                  ? "process_already_exited"
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
  {
    Fixture successive(Options(child, "slot_waiting_for_exit"),
                       "forced stop after cooperative delivery must not redeliver either kind",
                       "ignore-term");
    successive.driver->Launch(Launch());
    (void)successive.control.Ready();
    successive.driver->WaitHeld();
    successive.driver->Cooperative(Stop());
    successive.driver->WaitPoint("stop_delivered");
    successive.driver->Cooperative(Stop());
    successive.driver->Forced(Stop());
    successive.driver->WaitPoint("stop_delivered", 2);
    successive.driver->Forced(Stop());
    successive.driver->Release();
    successive.driver->WaitFacts(1, 1);
    ExactFacts(*successive.driver, 1, 1, "forced");
    ReapedAndJoined(*successive.driver);
    const auto deliveries = successive.driver->Inspect();
    Require(deliveries.cooperative_deliveries == 1 && deliveries.forced_deliveries == 1 &&
                deliveries.delivery_order == std::vector<std::string>{"cooperative", "forced"},
            "successive stop delivery duplicated or reordered a kind");
  }
  for (const bool forced : {false, true}) {
    Fixture gone(Options(child, "process_gone"),
                 "stops after gone must not signal an unreaped PID");
    gone.driver->Launch(Launch());
    (void)gone.control.Ready();
    gone.control.Exit();
    gone.driver->WaitHeld();
    if (forced)
      gone.driver->Forced(Stop());
    else
      gone.driver->Cooperative(Stop());
    gone.driver->WaitPoint("stop_checked");
    Require(gone.driver->Inspect().cooperative_deliveries == 0 &&
                gone.driver->Inspect().forced_deliveries == 0,
            "gone process received an OS stop");
    gone.driver->Release();
    gone.driver->WaitFacts(1, 1);
    ExactFacts(*gone.driver, 1, 1, "process_already_exited");
    ReapedAndJoined(*gone.driver);
  }
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
    const auto report_path = family.directory.path() / "report.json.desc";
    const auto report = test::ReadReport(report_path);
    // Bad reports must never create the capability accepted by Adopt(). No
    // unverified PID is stored or signalled, even when validation throws.
    for (const std::string field : {"process_id", "parent_process_id", "fixture_nonce"}) {
      auto invalid = report;
      if (field == "fixture_nonce")
        invalid[field] = "not-the-fixture-nonce";
      else
        invalid[field] = 0;
      {
        std::ofstream output(report_path);
        output << invalid.dump();
      }
      bool refused = false;
      try {
        (void)family.control.VerifyDescendant(descendant_index, 0, report_path);
      } catch (const std::runtime_error&) {
        refused = true;
      }
      {
        std::ofstream output(report_path);
        output << report.dump();
      }
      Require(refused, "unverified descendant report granted termination authority");
    }
    guard.ownership.Adopt(family.control.VerifyDescendant(descendant_index, 0, report_path));
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
  // Lock means every explicit Sitometron-owned mutex acquisition in the adapter
  // and composition bridges; core entries are tracked at their owned boundaries.
  // Owner decision (2026-11-18) excludes STL/OS-internal locks in ADR-0005
  // operations such as the request move. Hidden STL/CRT/OS locks are NOT measured.
  // The separate heap probe counts C++ new/delete, including sized/aligned forms.
  test::VerifyOwnedLockProbe();
  test::VerifyCellPhaseMatrix(Launch());
  {
    Fixture fixture(Options(MissingExecutable()),
                    "reused cell must transfer every heap-owned request buffer on its slot thread");
    for (unsigned number = 1; number <= 8; ++number) {
      auto request = Launch(number);
      // All eight strings own heap storage, including both UUIDs and both digests.
      // The unknown specification takes the failed-start path without creating a child.
      request.intent.operation_id.value = std::string(100, 'o');
      request.intent.application_id.value = std::string(100, 'a');
      request.intent.application_version = std::string(100, 'v');
      request.intent.allocation_id.value = std::string(100, 'r');
      fixture.driver->Launch(std::move(request));
      fixture.driver->WaitPoint("cell_released", number);
    }
    ExactFacts(*fixture.driver, 8, 0);
    ReapedAndJoined(*fixture.driver);
  }
  {
    Fixture fixture(Options(child, "stop_between_scan_reads"),
                    "an unrelated release between reads must not hide a still-live target");
    fixture.driver->Launch(Launch(1));
    (void)fixture.control.Ready();
    fixture.driver->Launch(Launch(2));
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(2, 0);
    struct Stopper {
      test::Driver& driver;
      std::thread thread;
      explicit Stopper(test::Driver& value)
          : driver(value), thread([&value] { value.Cooperative(Stop(2)); }) {}
      ~Stopper() {
        driver.Release();
        if (thread.joinable()) thread.join();
      }
    } stopper(*fixture.driver);
    fixture.driver->WaitHeld();
    fixture.control.Exit(0);
    fixture.driver->WaitPoint("cell_released");
    fixture.driver->Release();
    stopper.thread.join();
    fixture.driver->WaitFacts(2, 2);
    fixture.driver->WaitPoint("cell_released", 2);
    Require(fixture.driver->Inspect().cooperative_deliveries == 1,
            "scan missed its target after the unrelated release");
    fixture.driver->Launch(Launch(3));
    const auto replacement = fixture.control.Ready();
    fixture.control.Exit(replacement);
    fixture.driver->WaitFacts(3, 3);
    ExactFacts(*fixture.driver, 3, 3);
    for (const auto& attempt : Facts(*fixture.driver, "process_exit_confirmed"))
      if (attempt.event.job_id == Launch(3).job_id)
        Require(Json::parse(attempt.event.payload_json).at("completion_mode") ==
                    "process_already_exited",
                "stop reached a reused non-target cell");
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(MissingExecutable());
    options.hold_points = {"before_spawn", "stop_before_cas", "before_observation"};
    options.observations = {{test::Admission::kAdmitted, test::Completion::kRejected}};
    Fixture fixture(options, "a phase-only failed CAS must rescan before the last submission");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitPoint("before_spawn");
    struct Stopper {
      test::Driver& driver;
      std::thread thread;
      explicit Stopper(test::Driver& value)
          : driver(value), thread([&value] { value.Cooperative(Stop()); }) {}
      ~Stopper() {
        driver.Release();
        if (thread.joinable()) thread.join();
      }
    } stopper(*fixture.driver);
    fixture.driver->WaitPoint("stop_before_cas");
    fixture.driver->ReleasePoint("before_spawn");
    fixture.driver->WaitPoint("before_observation");
    Require(fixture.driver->Inspect().closing == 1,
            "failed observation was not closing before submit");
    fixture.driver->ReleasePoint("stop_before_cas");
    stopper.thread.join();
    fixture.driver->WaitPoint("stop_before_rescan");
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 1);
    ExactFacts(*fixture.driver, 1, 1, "cooperative");
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(child, "before_spawn_check");
    options.initial_generation = (std::numeric_limits<std::uint64_t>::max() / 128) - 2;
    Fixture fixture(options, "generation increments must retain all 57 bits without wrapping");
    for (unsigned number = 1; number <= 2; ++number) {
      fixture.driver->Launch(Launch(number));
      fixture.driver->WaitHeld(number);
      fixture.driver->Forced(Stop(number));
      fixture.driver->Release();
      fixture.driver->WaitPoint("cell_released", number);
    }
    Require(fixture.driver->Inspect().max_generation == options.initial_generation + 2,
            "generation exchange lost its high bits or failed to advance");
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(child);
    options.max_jobs = 3;
    Fixture fixture(options, "the mailbox inventory must scale to max_jobs+1 live slots");
    for (unsigned number = 1; number <= 4; ++number) fixture.driver->Launch(Launch(number));
    for (unsigned number = 1; number <= 4; ++number) (void)fixture.control.Ready();
    fixture.driver->WaitFacts(4, 0);
    for (unsigned number = 1; number <= 4; ++number) fixture.driver->Forced(Stop(number));
    fixture.driver->WaitFacts(4, 4);
    fixture.driver->WaitPoint("cell_released", 4);
    ExactFacts(*fixture.driver, 4, 4, "forced");
    Require(fixture.driver->Inspect().threads_created == 6, "scaled thread inventory changed");
    ReapedAndJoined(*fixture.driver);
  }
  const auto allocation_before = test::HandoffAllocationCount();
  const auto deallocation_before = test::HandoffDeallocationCount();
  {
    const test::HandoffAllocationScope probe;
    void* memory = ::operator new(1);
    ::operator delete(memory);
  }
  Require(test::HandoffAllocationCount() == allocation_before + 1 &&
              test::HandoffDeallocationCount() == deallocation_before + 1,
          "handoff C++ new/delete probe missed its planted allocation or deallocation");
  {
    const auto before_new = test::HandoffAllocationCount();
    const auto before_delete = test::HandoffDeallocationCount();
    {
      const test::HandoffAllocationScope probe;
      void* memory = ::operator new[](64, std::align_val_t{64});
      ::operator delete[](memory, std::size_t{64}, std::align_val_t{64});
    }
    Require(test::HandoffAllocationCount() == before_new + 1 &&
                test::HandoffDeallocationCount() == before_delete + 1,
            "handoff heap probe missed sized/aligned array new/delete");
  }
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
    Require(view.handoff_allocations == 0 && view.handoff_deallocations == 0 &&
                view.handoff_locks == 0 && view.handoff_ingress == 0,
            "handoff used C++ new/delete, a Sitometron-owned adapter/composition mutex, or core "
            "entry; hidden STL/CRT/OS locks are not measured");
    Require(view.max_completions_per_thread <= 1, "thread retained two completions");
    ReapedAndJoined(*fixture.driver);
  }
  {
    Fixture fixture(Options(child, "before_final_completion"),
                    "a launch after one closing cell must remain queued in its stable partner",
                    "exit");
    fixture.driver->Launch(Launch(1));
    fixture.driver->WaitPoint("before_final_completion");
    fixture.driver->Launch(Launch(2));
    const auto queued = fixture.driver->Inspect();
    Require(queued.closing == 1 && queued.occupied == 2 && queued.processes_created == 1,
            "closing partner did not retain the next launch behind the held completion");
    fixture.driver->Forced(Stop(2));
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released", 2);
    ExactFacts(*fixture.driver, 1, 2);
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().processes_created == 1,
            "stopped closing partner spawned instead of cancelling");
  }
  for (const bool cancel_queued : {false, true}) {
    auto options = Options(child, "before_final_completion");
    options.hold_points = {"before_spawn_check"};
    Fixture fixture(options, "retirement must offer the closing cell's stable partner", "exit");
    fixture.driver->Launch(Launch(1));
    fixture.driver->Launch(Launch(2));
    // Both claims must occur while active, before either fast child can enter
    // closing and make its same-mailbox partner eligible for the second launch.
    fixture.driver->WaitPoint("before_spawn_check", 2);
    fixture.driver->ReleasePoint("before_spawn_check");
    fixture.driver->WaitPoint("before_final_completion", 2);
    Require(fixture.driver->Inspect().closing == 2, "last fact did not enter closing first");
    fixture.driver->Launch(Launch(3));
    Require(fixture.driver->Inspect().occupied == 3,
            "three-Job max_jobs=1 interleaving lost the third launch");
    if (cancel_queued) fixture.driver->Cooperative(Stop(3));
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released", 3);
    ExactFacts(*fixture.driver, cancel_queued ? 2 : 3, 3);
    for (const auto& attempt : Facts(*fixture.driver, "process_exit_confirmed"))
      if (attempt.event.job_id == Launch(3).job_id)
        Require(Json::parse(attempt.event.payload_json).at("completion_mode") ==
                    (cancel_queued ? "cooperative" : "process_already_exited"),
                "queued partner lost its stop mode or started despite cancellation");
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().processes_created == (cancel_queued ? 2U : 3U),
            "queued partner failed to start after drain or spawned despite its stop");
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
  for (const std::string gap : {"relay_before_wait", "relay_capacity_wait"}) {
    auto options = Options(child);
    options.hold_points = {"after_park", gap};
    options.minimum_observations_for_wait_hold = 2;
    options.observations = {{test::Admission::kNormalFull},
                            {test::Admission::kNormalFull},
                            {test::Admission::kAdmitted}};
    Fixture fixture(options, "relay must retry its own normal_full after its own capacity wake");
    fixture.driver->WaitPoint("relay_before_wait");  // Initial empty scan, before any park.
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint(
        "after_park");  // Park's Set is held back, so it cannot mask this wake.
    fixture.driver->Capacity(2);
    fixture.driver->WaitPoint(gap, 2);
    const auto refused = fixture.driver->Attempts();
    Require(refused.size() == 2 && refused[0].answer.admission == test::Admission::kNormalFull &&
                refused[1].answer.admission == test::Admission::kNormalFull &&
                refused[0].thread != refused[1].thread,
            "relay's own attempt was not refused before the capacity wait");
    fixture.driver->Capacity(2);
    fixture.driver->ReleasePoint(gap);
    fixture.driver->WaitPoint("relay_before_wait",
                              3);  // Next complete scan, even if a mutant drops it.
    const auto accepted = Facts(*fixture.driver, "worker_launch_observed");
    const bool retried = accepted.size() == 1 && fixture.driver->Attempts().size() == 3;
    fixture.driver->ReleasePoint("after_park");
    fixture.control.Exit();
    fixture.driver->WaitPoint("cell_released");
    ReapedAndJoined(*fixture.driver);
    Require(retried, "relay did not retry after its own normal_full capacity wake");
    ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
  }
  {
    auto options = Options(child, "observation_completion_pending");
    options.observations = {{test::Admission::kNormalFull}, {test::Admission::kAdmitted}};
    Fixture fixture(options, "exit must settle the relay's admitted completion before release");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld();
    fixture.control.Exit();
    fixture.driver->WaitPoint("process_gone");
    Require(Facts(*fixture.driver, "process_exit_confirmed").empty(),
            "slot released its exit while relay still owned an admitted completion");
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 1);
    ExactFacts(*fixture.driver, 1, 1, "process_already_exited");
    ReapedAndJoined(*fixture.driver);
  }
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
  for (const bool rejected : {false, true}) {
    auto options = Options(MissingExecutable(), "failed_before_capacity_wait");
    options.observations = {{test::Admission::kNormalFull},
                            {test::Admission::kAdmitted,
                             rejected ? test::Completion::kRejected : test::Completion::kSuccess}};
    Fixture fixture(options,
                    "failed normal_full must retain its cell and retry on the slot thread");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    Require(fixture.driver->Inspect().occupied == 1, "failed retry released its obligation");
    if (rejected) fixture.driver->Forced(Stop());
    fixture.driver->Capacity(0);
    fixture.driver->Release();
    fixture.driver->WaitFacts(1, 0);
    fixture.driver->WaitPoint("cell_released");
    const auto attempts = fixture.driver->Attempts();
    Require(attempts.size() == (rejected ? 3U : 2U) && attempts[0].thread == attempts[1].thread &&
                (!rejected || attempts[2].thread == attempts[0].thread),
            "failed observation or its rejection-derived exit used another thread");
    ExactFacts(*fixture.driver, 1, rejected ? 1 : 0, rejected ? "forced" : "");
    ReapedAndJoined(*fixture.driver);
  }
}
void ExitReportedAfterRejectedObservation(const std::string& child) {
  for (const bool started : {false, true}) {
    for (const auto reason : {core::RejectionReason::kEventNotAllowedInState,
                              core::RejectionReason::kInvalidEventPayload,
                              core::RejectionReason::kInvariantViolation}) {
      for (const std::string stop : {"none", "cooperative", "forced", "both"}) {
        auto options = Options(started ? child : MissingExecutable(), "before_observation");
        options.observations = {{test::Admission::kAdmitted, test::Completion::kRejected, reason}};
        Fixture fixture(options,
                        "rejected launch observation still owes one exit after leaving preparing",
                        "ignore-term");
        fixture.driver->Launch(Launch());
        if (started) (void)fixture.control.Ready();
        fixture.driver->WaitHeld();
        if (stop == "cooperative" || stop == "both") fixture.driver->Cooperative(Stop());
        if (stop == "forced" || stop == "both") fixture.driver->Forced(Stop());
        fixture.driver->Release();
#ifndef _WIN32
        if (started && stop == "cooperative") {
          fixture.control.TermObserved();
          fixture.control.Exit();
        }
#endif
        if (started && stop == "none") fixture.control.Exit();
        fixture.driver->WaitFacts(1, 1);
        ExactFacts(*fixture.driver, 1, 1,
                   stop == "none"          ? "process_already_exited"
                   : stop == "cooperative" ? "cooperative"
                                           : "forced");
        ReapedAndJoined(*fixture.driver);
        Require(fixture.driver->Inspect().invariant_failures == 0,
                "a reducer rejection was converted to a service-wide failure");
      }
    }
  }
  {
    Fixture fixture(Options(child, "before_spawn"),
                    "a Job stop after the locked check belongs to the spawn interval");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    Require(fixture.driver->Inspect().processes_created == 0,
            "pre-spawn interval vector already created its child");
    fixture.driver->Cooperative(Stop());
    fixture.driver->Forced(Stop());
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released");
    ExactFacts(*fixture.driver, 1, 1, "forced");
    ReapedAndJoined(*fixture.driver);
    const auto view = fixture.driver->Inspect();
    Require(view.processes_created == 1 && view.cooperative_deliveries == 1 &&
                view.forced_deliveries == 1,
            "post-check flags cancelled spawn or were not delivered at publication");
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
#ifndef _WIN32
  bool disposition_refusals = true;
  for (const auto disposition :
       {test::ChildDisposition::kIgnored, test::ChildDisposition::kNoChildWait}) {
    const auto signal_guard = test::SetChildDisposition(disposition);
    auto options = Options(child);
    test::Diagnostics witness;
    options.queued_before_start = Launch();
    // The old implementation must fail safely before creating a thread or a child:
    // reaching this injected checkpoint proves the required disposition refusal is absent.
    options.fail_thread_after = 0;
    options.construction_failure = &witness;
    bool refused = false;
    try {
      Fixture fixture(options, "unsafe SIGCHLD disposition must refuse construction");
    } catch (const std::runtime_error& error) {
      refused =
          std::string_view(error.what()).find("runner requires waitable children: SIGCHLD") == 0;
    }
    const bool safe = refused && witness.threads_created == 0 && witness.threads_joined == 0 &&
                      witness.processes_created == 0 && witness.processes_reaped == 0;
    if (!safe)
      std::cerr << "SIGCHLD refusal missing for "
                << (disposition == test::ChildDisposition::kIgnored ? "SIG_IGN" : "SA_NOCLDWAIT")
                << '\n';
    disposition_refusals = disposition_refusals && safe;
  }
  Require(disposition_refusals, "SIGCHLD policy did not refuse before thread creation or launch");
  {
    const auto signal_guard = test::SetChildDisposition(test::ChildDisposition::kDefault);
    Fixture fixture(Options(child), "default SIGCHLD disposition must allow construction");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    fixture.control.Exit();
    fixture.driver->WaitFacts(1, 1);
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Inspect().processes_created == 1,
            "default SIGCHLD disposition did not start the child");
  }
#endif
  {
    Fixture fixture(Options(child, "service_scan_finished"),
                    "shutdown needs one complete shutdown-mode service scan");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint("slot_waiting_for_exit");
    fixture.driver->WakeService();
    fixture.driver->WaitPoint("service_scan_finished");
    fixture.driver->RequestStop();
    fixture.driver->ReleasePoint("service_scan_finished");
    fixture.driver->WaitPoint("service_exited");
    std::atomic_bool joined{false};
    std::jthread joiner([&] {
      fixture.driver->Join();
      joined.store(true, std::memory_order_release);
      joined.notify_all();
    });
    fixture.driver->WaitPoint("join_before_slot");
    const bool missed = fixture.driver->Inspect().forced_deliveries == 0;
    bool blocked = false;
    if (missed) {
      fixture.control.Ping();  // The controlled child cannot exit until the rescue below.
      blocked = !joined.load(std::memory_order_acquire);
      fixture.control.ExitAll();  // Rescue the RED without a timeout or leaked child.
    }
    joined.wait(false, std::memory_order_acquire);
    joiner.join();
    ReapedAndJoined(*fixture.driver);
    Require(!missed && !blocked,
            "shutdown service exited after a normal scan; join needed external child rescue");
  }
  {
    Fixture fixture(Options(child), "concurrent repeated stop/join must retain ownership");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitFacts(1, 0);
    std::barrier simultaneous(2);
    const auto operation = [&] {
      simultaneous.arrive_and_wait();
      fixture.driver->RequestStop();
      fixture.driver->Join();
    };
    std::jthread first;
    try {
      first = std::jthread(operation);
      std::jthread second(operation);
      first.join();
      second.join();
    } catch (...) {
      simultaneous.arrive_and_drop();
      if (first.joinable()) first.join();
      throw;
    }
    ReapedAndJoined(*fixture.driver);
  }
  for (std::size_t count = 0; count <= 4; ++count) {
    auto options = Options(child);
    test::Diagnostics witness;
    options.fail_thread_after = count;
    options.queued_before_start = Launch();
    options.construction_failure = &witness;
    bool failed = false;
    try {
      Fixture fixture(options, "partial construction must join all threads without spawning");
    } catch (const std::runtime_error& error) {
      failed = std::string_view(error.what()) == "injected runner thread construction failure";
    }
    Require(failed && witness.threads_created == count && witness.threads_joined == count &&
                witness.processes_created == 0 && witness.processes_reaped == 0,
            "partial thread construction escaped without complete unwind");
  }
  {
    auto options = Options(child);
    options.stop_on_point = "before_spawn_check";
    Fixture fixture(options, "producer-thread stop must refuse self-join and allow external join");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitPoint("slot_exited");
    ReapedAndJoined(*fixture.driver);
    Require(fixture.driver->Attempts().empty(), "self-stop before spawn submitted a fact");
  }
  {
    auto options = Options(child);
    options.observations = {{test::Admission::kAdmitted, test::Completion::kMissing}};
    Fixture fixture(options,
                    "an admitted input with no completion must report an invariant breach");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint("missing_completion_reported");
    ReapedAndJoined(*fixture.driver);
    Require(NoExitAttempts(*fixture.driver),
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
        Require(NoExitAttempts(*fixture.driver),
                "closed observation was followed by another attempted fact");
    }
  }
  for (const auto& answer : failures) {
    auto options = Options(child);
    options.observations = {answer};
    Fixture fixture(options, "a closed started observation must keep its child live until stop");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint("observation_answered");
    fixture.control.Ping();
    Require(fixture.driver->Inspect().forced_deliveries == 0,
            "a closed started observation forced the still-live child");
    ReapedAndJoined(*fixture.driver);
    Require(NoExitAttempts(*fixture.driver),
            "runner stop after closed observation attempted another fact");
  }
  {
    auto options = Options(child, "after_park");
    options.observations = {{test::Admission::kNormalFull}};
    Fixture fixture(options, "shutdown must drop a parked started retry and reap its child");
    fixture.driver->Launch(Launch());
    (void)fixture.control.Ready();
    fixture.driver->WaitHeld();
    fixture.driver->RequestStop();
    ReapedAndJoined(*fixture.driver);
    Require(Facts(*fixture.driver, "worker_launch_observed").empty() &&
                Facts(*fixture.driver, "process_exit_confirmed").empty(),
            "shutdown resubmitted a parked observation or invented a final fact");
  }
  {
    auto options = Options(child, "failed_before_capacity_wait");
    options.specifications.push_back(
        {"missing-child", MissingExecutable(), {}, std::string(k_digest)});
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
    Require(NoExitAttempts(*fixture.driver) && closed.forced_deliveries == 0 &&
                closed.threads_joined == 0 && closed.processes_reaped == 0,
            "admission closure attempted an exit or forced/stopped/joined the live child");
    fixture.control.Exit();
    fixture.driver->WaitPoint("cell_released", 2);
    ReapedAndJoined(*fixture.driver);
  }
  {
    auto options = Options(child, "before_final_completion");
    options.hold_points = {"before_spawn_check"};
    Fixture fixture(options, "shutdown must drop a launch still queued behind a closing cell",
                    "exit");
    fixture.driver->Launch(Launch(1));
    fixture.driver->Launch(Launch(2));
    // Establish two independent active mailboxes before allowing either child to
    // exit; otherwise launch 2 may validly queue behind the first closing cell.
    fixture.driver->WaitPoint("before_spawn_check", 2);
    fixture.driver->ReleasePoint("before_spawn_check");
    (void)fixture.control.Ready();
    (void)fixture.control.Ready();
    fixture.driver->WaitPoint("before_final_completion", 2);
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
      for (const bool started : {false, true}) {
        if (!started && !observation) continue;
        auto options = Options(started ? child : MissingExecutable());
        if (observation)
          options.observations = {{answer}};
        else
          options.exits = {{answer}};
        Fixture fixture(options, "unexpected ingress answers must not duplicate runner facts");
        fixture.driver->Launch(Launch());
        if (started) (void)fixture.control.Ready();
        fixture.driver->WaitPoint("observation_answered");
        if (started) fixture.control.Exit();
        fixture.driver->WaitPoint("cell_released");
        std::size_t observations = 0;
        std::size_t exits = 0;
        for (const auto& attempt : fixture.driver->Attempts()) {
          if (attempt.event.event_type == "worker_launch_observed") ++observations;
          if (attempt.event.event_type == "process_exit_confirmed") ++exits;
        }
        Require(observations == 1 && exits == 1,
                "non-retryable ingress answer lost or duplicated an observation or critical exit");
        ReapedAndJoined(*fixture.driver);
      }
    }
  }
  for (const auto answer : {test::Admission::kAdmissionClosed, test::Admission::kServiceFailed}) {
    auto options = Options(MissingExecutable(), "failed_before_capacity_wait");
    options.observations = {{test::Admission::kNormalFull}, {answer}};
    Fixture fixture(options, "each closure answer must end a failed normal_full retry");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitHeld();
    fixture.driver->Capacity(0);
    fixture.driver->Release();
    fixture.driver->WaitPoint("cell_released");
    Require(fixture.driver->Attempts().size() == 2 && NoExitAttempts(*fixture.driver),
            "failed observation closure was retried or followed by exit");
    ReapedAndJoined(*fixture.driver);
  }
  for (const auto& answer : failures) {
    auto options = Options(MissingExecutable());
    options.observations = {answer};
    Fixture fixture(options, "a closed failed observation owes no further fact");
    fixture.driver->Launch(Launch());
    fixture.driver->WaitPoint("cell_released");
    Require(NoExitAttempts(*fixture.driver),
            "closed failed start attempted another runner fact, regardless of admission");
    ReapedAndJoined(*fixture.driver);
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
    Require(NoExitAttempts(*fixture.driver) && fixture.driver->Inspect().threads_joined == 0,
            "core closure attempted an exit or joined runner threads");
    ReapedAndJoined(*fixture.driver);
  }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 3) {
    std::cerr << "usage: sitometron_process_tests <check> <built-test-child>\n";
    return 2;
  }
#ifdef _WIN32
  const std::string name = test::Utf8Argument(argv[1]);
#else
  const std::string name = argv[1];
#endif
  try {
    const auto child_path = std::filesystem::absolute(argv[2]);
    Require(std::filesystem::is_regular_file(child_path), "built test child missing");
    const auto child = test::PathText(child_path);
    if (name == "process_launch_observed_after_start")
      LaunchObservedAfterStart(child_path);
    else if (name == "process_failed_start_reports_failed")
      FailedStartReportsFailed(child_path);
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
