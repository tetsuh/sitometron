// sitometron_spike: the non-normative walking skeleton (Issue #48).
//
//   sitometron_spike --listen 127.0.0.1:8080 --journal ./journal.jsonl --workdir /tmp/work
//
// Composes the Phase 0A core (reducer + single writer) with real adapters: a file Journal, a
// posix_spawn process runner, a loopback HTTP surface, and system clock/identity sources.

#include <signal.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "file_journal.hpp"
#include "http_server.hpp"
#include "job_driver.hpp"
#include "sitometron/core/version.hpp"

namespace {

using nlohmann::json;
using sitometron::spike::HttpRequest;
using sitometron::spike::HttpResponse;

std::mutex g_signal_mutex;
std::condition_variable g_signal_cv;
volatile sig_atomic_t g_signal = 0;

void OnSignal(int signal) {
  g_signal = signal;
  g_signal_cv.notify_all();
}

struct Options {
  std::string host = "127.0.0.1";
  unsigned short port = 8080;
  std::string journal = "sitometron-spike-journal.jsonl";
  std::string workdir;
  std::size_t max_jobs = 32;
  std::size_t trace_capacity = 4096;
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
        error = "--journal needs a path";
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
  if (target == "/healthz" && request.method == "GET")
    return Json(200, {{"status", "ok"}, {"version", sitometron::Version()}});
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
    if (body.contains("workdir") && body["workdir"].is_string())
      spec.working_directory = body["workdir"].get<std::string>();
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

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, options, error)) {
    if (!error.empty()) std::cerr << "error: " << error << '\n';
    std::cerr << "usage: sitometron_spike [--listen HOST:PORT] [--journal PATH] [--workdir DIR]"
                 " [--max-jobs N] [--trace-capacity N]\n";
    return error.empty() ? 0 : 2;
  }

  sitometron::spike::FileJournal journal(options.journal);
  if (!journal.Open(error)) {
    std::cerr << "error: cannot open journal " << options.journal << ": " << error << '\n';
    return 1;
  }
  sitometron::spike::DriverConfig config;
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

  struct sigaction action {};
  action.sa_handler = OnSignal;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  std::cout << "sitometron_spike " << sitometron::Version() << " listening on http://"
            << options.host << ':' << server.port() << " journal=" << options.journal
            << " (existing lines: " << journal.lines_on_open() << ")\n"
            << std::flush;

  {
    std::unique_lock lock(g_signal_mutex);
    g_signal_cv.wait(lock, [] { return g_signal != 0; });
  }
  std::cout << "signal " << static_cast<int>(g_signal) << ": shutting down\n" << std::flush;
  server.Stop();
  driver.Shutdown();
  std::cout << "journal committed " << journal.committed_count() << " events this run\n";
  return 0;
}
