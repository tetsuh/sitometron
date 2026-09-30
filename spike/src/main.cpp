// sitometron_spike: the non-normative walking skeleton (Issue #48).
//
//   sitometron_spike --listen 127.0.0.1:8080 --journal ./journal --workdir /tmp/work
//   sitometron_spike journal quarantine-tail --journal ./journal
//   sitometron_spike journal prune --journal ./journal [--dry-run]
//
// Composes the Phase 0A core (reducer + single writer) with real adapters: a file Journal, a
// posix_spawn process runner, a loopback HTTP surface, and system clock/identity sources.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "http_server.hpp"
#include "job_driver.hpp"
#include "sitometron/core/version.hpp"
#include "sitometron/journal/maintenance.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace {

using nlohmann::json;
using sitometron::spike::HttpRequest;
using sitometron::spike::HttpResponse;

// Self-pipe: the handler only records the signal and writes one byte, both async-signal-safe.
int g_signal_pipe[2] = {-1, -1};
volatile sig_atomic_t g_signal = 0;

void OnSignal(int signal) {
  g_signal = signal;
  const char byte = 1;
  (void)!::write(g_signal_pipe[1], &byte, 1);
}

struct Options {
  std::string host = "127.0.0.1";
  unsigned short port = 8080;
  std::string journal = "sitometron-spike-journal";
  std::string workdir;
  std::size_t max_jobs = 32;
  std::size_t trace_capacity = 4096;
  std::uint64_t segment_limit = sitometron::journal::SegmentJournalOptions{}.segment_limit_bytes;
};

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    auto value = [&]() -> const char* {
      if (index + 1 >= argc) return nullptr;
      return argv[++index];
    };
    if (flag == "--listen") {
      const char* raw = value();
      if (raw == nullptr) {
        error = "--listen needs host:port";
        return false;
      }
      const std::string text = raw;
      const auto colon = text.rfind(':');
      if (colon == std::string::npos) {
        error = "--listen needs host:port";
        return false;
      }
      options.host = text.substr(0, colon);
      options.port = static_cast<unsigned short>(std::stoul(text.substr(colon + 1)));
    } else if (flag == "--journal") {
      const char* raw = value();
      if (raw == nullptr) {
        error = "--journal needs a directory";
        return false;
      }
      options.journal = raw;
    } else if (flag == "--workdir") {
      const char* raw = value();
      if (raw == nullptr) {
        error = "--workdir needs a path";
        return false;
      }
      options.workdir = raw;
    } else if (flag == "--max-jobs") {
      const char* raw = value();
      if (raw == nullptr) {
        error = "--max-jobs needs a number";
        return false;
      }
      options.max_jobs = std::stoul(raw);
    } else if (flag == "--trace-capacity") {
      const char* raw = value();
      if (raw == nullptr) {
        error = "--trace-capacity needs a number";
        return false;
      }
      options.trace_capacity = std::stoul(raw);
    } else if (flag == "--segment-limit") {
      const char* raw = value();
      const std::string_view text = raw == nullptr ? std::string_view() : std::string_view(raw);
      const auto [end, code] =
          std::from_chars(text.data(), text.data() + text.size(), options.segment_limit);
      if (text.empty() || code != std::errc() || end != text.data() + text.size()) {
        error = "--segment-limit needs a number of bytes";
        return false;
      }
    } else if (flag == "--help" || flag == "-h") {
      error.clear();
      return false;
    } else {
      error = "unknown option: " + flag;
      return false;
    }
  }
  return true;
}

HttpResponse Json(int status, const json& body) { return {status, body.dump()}; }

HttpResponse Route(sitometron::spike::JobDriver& driver, const HttpRequest& request) {
  const auto& target = request.target;
  if (target == "/healthz" && request.method == "GET") {
    if (!driver.Ready()) {
      return Json(503, {{"status", "not_ready"},
                        {"ready", false},
                        {"unresolved", driver.Unresolved()},
                        {"version", sitometron::Version()}});
    }
    return Json(200, {{"status", "ok"}, {"ready", true}, {"version", sitometron::Version()}});
  }
  if (target == "/stats" && request.method == "GET") return Json(200, driver.Stats());
  if (target == "/jobs") {
    if (request.method == "GET") return Json(200, driver.List());
    if (request.method != "POST") return Json(405, {{"error", "method not allowed"}});
    json body;
    try {
      body = json::parse(request.body);
    } catch (const std::exception& e) {
      return Json(400, {{"error", std::string("invalid JSON: ") + e.what()}});
    }
    if (!body.is_object() || !body.contains("executable") || !body["executable"].is_string() ||
        body["executable"].get<std::string>().empty())
      return Json(400, {{"error", "body needs a non-empty string field \"executable\""}});
    sitometron::spike::LaunchSpec spec;
    spec.executable = body["executable"].get<std::string>();
    if (body.contains("args")) {
      if (!body["args"].is_array()) return Json(400, {{"error", "\"args\" must be an array"}});
      for (const auto& item : body["args"]) {
        if (!item.is_string()) return Json(400, {{"error", "\"args\" items must be strings"}});
        spec.arguments.push_back(item.get<std::string>());
      }
    }
    if (body.contains("workdir")) {
      if (!body["workdir"].is_string())
        return Json(400, {{"error", "\"workdir\" must be a string"}});
      spec.working_directory = body["workdir"].get<std::string>();
    }
    std::string error;
    const auto id = driver.Submit(std::move(spec), error);
    if (!id) return Json(503, {{"error", error}});
    return Json(202, {{"job_id", *id}});
  }
  if (target.rfind("/jobs/", 0) == 0) {
    if (request.method != "GET") return Json(405, {{"error", "method not allowed"}});
    const auto described = driver.Describe(target.substr(6));
    if (described.is_null()) return Json(404, {{"error", "unknown job"}});
    return Json(200, described);
  }
  return Json(404, {{"error", "unknown resource"}});
}

// Offline Journal maintenance (OPS-005): runs instead of the daemon, never starts the writer or the
// HTTP server, and refuses while a daemon holds the Journal lock.
namespace journal = sitometron::journal;

constexpr std::string_view k_tool_usage =
    "usage: sitometron_spike journal quarantine-tail --journal DIR\n"
    "       sitometron_spike journal prune --journal DIR [--dry-run]\n";

struct ToolOptions {
  std::string command;
  std::string directory;
  bool dry_run = false;
};

// Parses `journal <command> --journal DIR [--dry-run]`; returns false after printing the reason.
bool ParseToolOptions(int argc, char** argv, ToolOptions& options) {
  const std::vector<std::string_view> args(argv + 2, argv + argc);
  if (!args.empty()) options.command = args.front();
  std::size_t index = 1;
  while (index < args.size()) {
    const auto flag = args[index];
    if (flag == "--journal" && index + 1 < args.size()) {
      options.directory = args[index + 1];
      index += 2;
    } else if (flag == "--dry-run" && options.command == "prune") {
      options.dry_run = true;
      index += 1;
    } else {
      std::cerr << "error: unknown option " << flag << '\n' << k_tool_usage;
      return false;
    }
  }
  if ((options.command != "quarantine-tail" && options.command != "prune") ||
      options.directory.empty()) {
    std::cerr << k_tool_usage;
    return false;
  }
  return true;
}

// Prints the result detail to stdout on success or to stderr on refusal; returns the exit code.
int Report(journal::MaintenanceStatus status, const std::string& detail) {
  using enum journal::MaintenanceStatus;
  if (status == kDone || status == kPlanned || status == kNothingToDo) {
    std::cout << detail << '\n';
    return 0;
  }
  std::cerr << "error: " << detail << '\n';
  return 1;
}

int RunQuarantine(const ToolOptions& options) {
  const auto result =
      journal::QuarantineTornTail(journal::SystemMaintenanceFileSystem(), options.directory);
  if (result.status == journal::MaintenanceStatus::kDone) {
    std::cout << "quarantined " << result.bytes << " bytes of " << result.segment << " from byte "
              << result.offset << " into " << result.quarantine << '\n'
              << "next sequence " << result.next_sequence << '\n';
  }
  return Report(result.status, result.detail);
}

int RunPrune(const ToolOptions& options) {
  journal::PruneOptions prune;
  prune.dry_run = options.dry_run;
  const auto result =
      journal::PruneClosedPrefix(journal::SystemMaintenanceFileSystem(), options.directory, prune);
  // The lists are empty unless the prefix was moved (kDone) or planned (kPlanned).
  const std::string_view segment_verb = options.dry_run ? "would archive " : "archived ";
  const std::string_view job_verb = options.dry_run ? "would prune job " : "pruned job ";
  for (const auto& segment : result.segments) std::cout << segment_verb << segment << '\n';
  for (const auto& id : result.jobs) std::cout << job_verb << id.value << '\n';
  return Report(result.status, result.detail);
}

int RunJournalTool(int argc, char** argv) {
  ToolOptions options;
  if (!ParseToolOptions(argc, argv, options)) return 2;
  return options.command == "prune" ? RunPrune(options) : RunQuarantine(options);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "journal") return RunJournalTool(argc, argv);
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, options, error)) {
    if (!error.empty()) std::cerr << "error: " << error << '\n';
    std::cerr << "usage: sitometron_spike [--listen HOST:PORT] [--journal DIR] [--workdir DIR]"
                 " [--max-jobs N] [--trace-capacity N] [--segment-limit BYTES]\n"
                 "       sitometron_spike journal quarantine-tail --journal DIR\n"
                 "       sitometron_spike journal prune --journal DIR [--dry-run]\n";
    return error.empty() ? 0 : 2;
  }

  sitometron::journal::SegmentJournalOptions journal_options;
  journal_options.segment_limit_bytes = options.segment_limit;
  sitometron::journal::SegmentJournal journal(sitometron::journal::SystemFileSystem(),
                                              journal_options);
  const auto opened = journal.Open(options.journal);
  if (!opened.ok) {
    std::cerr << "error: cannot open journal " << options.journal << ": " << opened.detail << '\n';
    return 1;
  }
  // Validate and replay every segment before the writer starts (OPS-002, OPS-003). Replay only
  // reads; a refusal leaves the Journal untouched.
  const auto replayed =
      sitometron::journal::ReplayJournal(sitometron::journal::SystemFileSystem(), options.journal,
                                         sitometron::journal::ReplayOptions{options.max_jobs});
  if (replayed.status != sitometron::journal::ReplayStatus::kReplayed) {
    std::cerr << "error: cannot replay journal " << options.journal << ": " << replayed.detail
              << '\n';
    return 1;
  }
  for (const auto& id : replayed.unresolved)
    std::cout << "unresolved job " << id.value << ": admission closed\n";
  sitometron::spike::DriverConfig config;
  config.replayed_jobs = replayed.jobs;
  config.unresolved_jobs = replayed.unresolved;
  config.max_jobs = options.max_jobs;
  config.trace_capacity = options.trace_capacity;
  config.working_directory = options.workdir;
  sitometron::spike::JobDriver driver(config, journal);

  sitometron::spike::HttpServer server(
      options.host, options.port,
      [&driver](const HttpRequest& request) { return Route(driver, request); });
  if (!server.Start(error)) {
    std::cerr << "error: cannot listen on " << options.host << ':' << options.port << ": " << error
              << '\n';
    return 1;
  }

  if (::pipe2(g_signal_pipe, O_CLOEXEC) != 0) {
    std::cerr << "error: cannot create the signal pipe: " << std::strerror(errno) << '\n';
    return 1;
  }
  struct sigaction action {};
  action.sa_handler = OnSignal;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  std::cout << "sitometron_spike " << sitometron::Version() << " listening on http://"
            << options.host << ':' << server.port() << " journal=" << options.journal
            << " (next sequence: " << opened.next_sequence
            << ", replayed jobs: " << replayed.jobs.size()
            << ", unresolved: " << replayed.unresolved.size() << ")\n"
            << std::flush;

  for (;;) {
    char byte = 0;
    const auto count = ::read(g_signal_pipe[0], &byte, 1);
    if (count == 1 || (count < 0 && errno != EINTR)) break;
  }
  std::cout << "signal " << static_cast<int>(g_signal) << ": shutting down\n" << std::flush;
  server.Stop();
  driver.Shutdown();
  std::cout << "journal next sequence " << journal.NextSequence() << " at shutdown\n";
  return 0;
}
