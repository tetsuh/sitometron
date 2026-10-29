#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/sitometrond/composition.hpp"
#include "core_fakes.hpp"
#include "job_orchestrator.hpp"
#include "journal_test_events.hpp"
#include "reducer_snapshot_json.hpp"
#include "sitometron/http/server.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace {

using sitometron::http::Backend;
using sitometron::http::Options;
using sitometron::http::Readiness;
using sitometron::http::ReadinessReason;
using sitometron::http::Server;

constexpr std::string_view k_job_id = "01890f30-7b54-7cc3-98c4-dc0c0c07398f";

struct Response {
  int status = 0;
  std::string version;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

int Check(bool condition, std::string_view name, std::string_view detail) {
  if (condition) return 0;
  std::cerr << name << ": " << detail << '\n';
  return 1;
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

Response ParseResponse(const std::string& bytes) {
  Response response;
  const auto header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string::npos) return response;
  const auto first_end = bytes.find("\r\n");
  if (first_end == std::string::npos) return response;
  std::istringstream status_line(bytes.substr(0, first_end));
  status_line >> response.version >> response.status;
  std::size_t line_start = first_end + 2;
  while (line_start < header_end) {
    const auto line_end = bytes.find("\r\n", line_start);
    if (line_end == std::string::npos || line_end > header_end) break;
    const auto colon = bytes.find(':', line_start);
    if (colon != std::string::npos && colon < line_end) {
      auto value = bytes.substr(colon + 1, line_end - colon - 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      response.headers.emplace(Lower(bytes.substr(line_start, colon - line_start)),
                               std::move(value));
    }
    line_start = line_end + 2;
  }
  response.body = bytes.substr(header_end + 4);
  return response;
}

constexpr auto k_network_timeout = std::chrono::seconds(15);
constexpr auto k_process_timeout = std::chrono::milliseconds(15000);

class TimedSocket final {
 public:
  explicit TimedSocket(boost::asio::io_context& context,
                       std::chrono::steady_clock::duration timeout = k_network_timeout)
      : context_(context), socket_(context), timeout_(timeout) {}

  bool Connect(std::uint16_t port) {
    bool completed = false;
    bool timed_out = false;
    boost::system::error_code operation_error;
    boost::asio::steady_timer timer(context_);
    timer.expires_after(timeout_);
    timer.async_wait([&](const boost::system::error_code& error) {
      if (!error && !completed) {
        timed_out = true;
        timed_out_ = true;
        boost::system::error_code ignored;
        [[maybe_unused]] const auto cancelled = socket_.cancel(ignored);
      }
    });
    socket_.async_connect({boost::asio::ip::make_address("127.0.0.1"), port},
                          [&](const boost::system::error_code& error) {
                            operation_error = error;
                            completed = true;
                            (void)timer.cancel();
                          });
    context_.restart();
    context_.run();
    return completed && !timed_out && !operation_error;
  }

  bool Write(std::string_view bytes) {
    if (bytes.empty()) return true;
    bool completed = false;
    bool timed_out = false;
    boost::system::error_code operation_error;
    boost::asio::steady_timer timer(context_);
    timer.expires_after(timeout_);
    timer.async_wait([&](const boost::system::error_code& error) {
      if (!error && !completed) {
        timed_out = true;
        timed_out_ = true;
        boost::system::error_code ignored;
        [[maybe_unused]] const auto cancelled = socket_.cancel(ignored);
      }
    });
    boost::asio::async_write(socket_, boost::asio::buffer(bytes.data(), bytes.size()),
                             [&](const boost::system::error_code& error, std::size_t) {
                               operation_error = error;
                               completed = true;
                               (void)timer.cancel();
                             });
    context_.restart();
    context_.run();
    return completed && !timed_out && !operation_error;
  }

  bool ReadSome(std::array<char, 4096>& buffer, std::size_t& count,
                boost::system::error_code& operation_error) {
    count = 0;
    operation_error.clear();
    bool completed = false;
    bool timed_out = false;
    boost::asio::steady_timer timer(context_);
    timer.expires_after(timeout_);
    timer.async_wait([&](const boost::system::error_code& error) {
      if (!error && !completed) {
        timed_out = true;
        timed_out_ = true;
        boost::system::error_code ignored;
        [[maybe_unused]] const auto cancelled = socket_.cancel(ignored);
      }
    });
    socket_.async_read_some(boost::asio::buffer(buffer),
                            [&](const boost::system::error_code& error, std::size_t bytes_read) {
                              operation_error = error;
                              count = bytes_read;
                              completed = true;
                              (void)timer.cancel();
                            });
    context_.restart();
    context_.run();
    return completed && !timed_out;
  }

  [[nodiscard]] bool TimedOut() const noexcept { return timed_out_; }

  void ShutdownSend() {
    boost::system::error_code ignored;
    [[maybe_unused]] const auto shutdown =
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
  }

  void Close() {
    boost::system::error_code ignored;
    [[maybe_unused]] const auto cancelled = socket_.cancel(ignored);
    [[maybe_unused]] const auto closed = socket_.close(ignored);
  }

 private:
  boost::asio::io_context& context_;
  boost::asio::ip::tcp::socket socket_;
  std::chrono::steady_clock::duration timeout_;
  bool timed_out_ = false;
};

void ReadUntilClose(TimedSocket& socket, std::string& bytes) {
  std::array<char, 4096> buffer{};
  for (;;) {
    std::size_t count = 0;
    boost::system::error_code ec;
    const bool completed = socket.ReadSome(buffer, count, ec);
    if (count != 0) bytes.append(buffer.data(), count);
    if (!completed || ec) break;
  }
  if (socket.TimedOut()) bytes.clear();
}

bool ReadUntilPrefix(TimedSocket& socket, std::string& bytes, std::string_view prefix) {
  std::array<char, 4096> buffer{};
  while (bytes.size() < prefix.size()) {
    std::size_t count = 0;
    boost::system::error_code ec;
    const bool completed = socket.ReadSome(buffer, count, ec);
    if (count != 0) bytes.append(buffer.data(), count);
    if (!completed || ec) break;
  }
  if (socket.TimedOut()) bytes.clear();
  return bytes.starts_with(prefix);
}

Response Send(Backend& backend, Options options, const std::string& request,
              std::chrono::steady_clock::duration timeout = k_network_timeout) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  TimedSocket socket(context, timeout);
  std::string bytes;
  if (socket.Connect(server.port()) && socket.Write(request)) ReadUntilClose(socket, bytes);
  socket.Close();
  server.Stop();
  return ParseResponse(bytes);
}

Response SendPartial(Backend& backend, Options options, const std::string& prefix) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  TimedSocket socket(context);
  std::string bytes;
  if (socket.Connect(server.port()) && socket.Write(prefix)) ReadUntilClose(socket, bytes);
  socket.Close();
  server.Stop();
  return ParseResponse(bytes);
}

Response SendParts(Backend& backend, Options options, const std::vector<std::string>& parts) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  TimedSocket socket(context);
  bool wrote = socket.Connect(server.port());
  for (const auto& part : parts) {
    if (!wrote) break;
    wrote = socket.Write(part);
  }
  std::string bytes;
  if (wrote) ReadUntilClose(socket, bytes);
  socket.Close();
  server.Stop();
  return ParseResponse(bytes);
}

class ScopedTempDirectory final {
 public:
  explicit ScopedTempDirectory(std::string_view label) {
    const auto base = std::filesystem::temp_directory_path();
    for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
      path_ = base / ("issue105-" + std::string(label) + "-" +
                      std::to_string(next.fetch_add(1, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        owned_ = true;
        return;
      }
      if (error == std::errc::file_exists || (!error && std::filesystem::exists(path_))) continue;
      throw std::filesystem::filesystem_error("create_directory", path_, error);
    }
    throw std::runtime_error("could not create a unique temporary directory");
  }

  ~ScopedTempDirectory() {
    if (owned_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  static inline std::atomic_uint64_t next{0};
  std::filesystem::path path_;
  bool owned_ = false;
};

Response SendChunkedParts(Backend& backend, Options options, const std::string& header,
                          const std::vector<std::string>& parts, bool* saw_continue = nullptr,
                          bool wait_for_continue = false, bool shutdown_send = false) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  TimedSocket socket(context);
  bool wrote = socket.Connect(server.port()) && socket.Write(header);

  constexpr std::string_view k_continue = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string bytes;
  bool got_continue = false;
  if (wait_for_continue && wrote) {
    std::string interim;
    got_continue = ReadUntilPrefix(socket, interim, k_continue);
    if (got_continue) {
      bytes = interim.substr(k_continue.size());
    } else {
      bytes = std::move(interim);
    }
  }
  if ((!wait_for_continue || got_continue) && wrote) {
    for (const auto& part : parts) {
      wrote = socket.Write(part);
      if (!wrote) break;
    }
    if (wrote && shutdown_send) socket.ShutdownSend();
    if (wrote) ReadUntilClose(socket, bytes);
  }
  socket.Close();
  server.Stop();

  if (!wait_for_continue) {
    got_continue = bytes.starts_with(k_continue);
    if (got_continue) bytes.erase(0, k_continue.size());
  }
  if (saw_continue != nullptr) *saw_continue = got_continue;
  return ParseResponse(bytes);
}

Response SendToPort(std::uint16_t port, const std::string& request) {
  boost::asio::io_context context;
  TimedSocket socket(context);
  std::string bytes;
  if (socket.Connect(port) && socket.Write(request)) ReadUntilClose(socket, bytes);
  socket.Close();
  return ParseResponse(bytes);
}

int ExpectError(const Response& response, std::string_view name, int status,
                std::string_view domain, const char* code) {
  int result = Check(response.status == status, name,
                     "expected HTTP status " + std::to_string(status) + ", got " +
                         std::to_string(response.status));
  if (response.status != status) return result;
  try {
    const auto body = nlohmann::json::parse(response.body);
    const auto& error = body.at("error");
    result |= Check(error.at("domain").get<std::string>() == std::string(domain), name,
                    "error domain mismatch");
    result |= Check(error.at("code").get<std::string>() == std::string(code), name,
                    "error code mismatch");
    result |= Check(error.at("details").is_object() && error.at("details").empty(), name,
                    "error details is not an empty object");
  } catch (const std::exception& error) {
    result |= Check(false, name, std::string("error envelope parse failed: ") + error.what());
  }
  return result;
}

int ExpectStatus(const Response& response, std::string_view name, int status) {
  return Check(response.status == status, name,
               "expected HTTP status " + std::to_string(status) + ", got " +
                   std::to_string(response.status));
}

class FakeBackend : public Backend {
 public:
  Readiness readiness{};

  Readiness GetReadiness() override { return readiness; }
  sitometron::http::JobResult CreateJob(const std::string&) override {
    return {sitometron::http::JobResultCode::kUnknownApplication, std::nullopt};
  }
  std::optional<sitometron::http::JobResource> GetJob(const std::string&) override {
    return std::nullopt;
  }
  std::vector<sitometron::http::JobResource> ListJobs() override { return {}; }
  sitometron::http::JobResult CancelJob(const std::string&, const std::string&) override {
    return {readiness.ready ? sitometron::http::JobResultCode::kJobNotFound
                            : sitometron::http::JobResultCode::kNotReady,
            std::nullopt};
  }
};

std::string HealthRequest(std::string_view method = "GET", std::string_view target = "/v1/health",
                          std::string_view version = "HTTP/1.1") {
  return std::string(method) + " " + std::string(target) + " " + std::string(version) +
         "\r\nHost: loopback\r\n\r\n";
}

int LoopbackOnly() {
  FakeBackend backend;
  Options options;
  options.listen_address = "0.0.0.0";
  Server server(options, backend);
  std::string error;
  return Check(!server.Start(error), "rest_v1_loopback_only",
               "non-loopback listener unexpectedly started");
}

int UnknownRouteAndMethod() {
  FakeBackend backend;
  int result = ExpectError(Send(backend, Options{}, HealthRequest("GET", "/v1/nope")),
                           "rest_v1_unknown_route_and_method", 404, "request", "route_not_found");
  const auto response = Send(backend, Options{}, HealthRequest("PUT"));
  result |= ExpectError(response, "rest_v1_unknown_route_and_method", 405, "request",
                        "method_not_allowed");
  if (response.status == 405)
    result |= Check(response.headers.at("allow") == "GET", "rest_v1_unknown_route_and_method",
                    "Allow header mismatch");
  return result;
}

int JobRoutesNotReady() {
  FakeBackend backend;
  int result = ExpectStatus(Send(backend, Options{}, HealthRequest("GET", "/v1/jobs")),
                            "rest_v1_job_routes_not_ready", 200);
  result |= ExpectError(Send(backend, Options{}, HealthRequest("POST", "/v1/jobs")),
                        "rest_v1_job_routes_not_ready", 400, "request", "malformed_json");
  result |= ExpectError(
      Send(backend, Options{}, HealthRequest("GET", "/v1/jobs/" + std::string(k_job_id))),
      "rest_v1_job_routes_not_ready", 404, "job", "job_not_found");
  result |=
      ExpectError(Send(backend, Options{},
                       HealthRequest("POST", "/v1/jobs/" + std::string(k_job_id) + "/cancel")),
                  "rest_v1_job_routes_not_ready", 404, "job", "job_not_found");
  bool http11_expect_continue = false;
  const auto http11_expect_response = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
      {"1", "\r\nx\r\n0\r\n\r\n"}, &http11_expect_continue, true);
  result |= Check(http11_expect_continue, "rest_v1_job_routes_not_ready",
                  "HTTP/1.1 Expect did not produce 100 Continue");
  result |= ExpectError(http11_expect_response, "rest_v1_job_routes_not_ready", 400, "request",
                        "malformed_json");

  const std::array<std::string_view, 4> empty_transfer_elements = {",chunked", "chunked,",
                                                                   "chunked,,", ", chunked ,"};
  for (const auto transfer_encoding : empty_transfer_elements) {
    const auto request =
        std::string("POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\n") +
        "Content-Type: application/json\r\nTransfer-Encoding: " + std::string(transfer_encoding) +
        "\r\n\r\n0\r\n\r\n";
    result |= ExpectError(Send(backend, Options{}, request), "rest_v1_job_routes_not_ready", 400,
                          "request", "malformed_json");
  }
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding:\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
      "rest_v1_job_routes_not_ready", 400, "request", "malformed_json");

  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 5,,5\r\n\r\n12345"),
      "rest_v1_job_routes_not_ready", 422, "request", "validation_failed");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 5, 5\r\n\r\n12345"),
      "rest_v1_job_routes_not_ready", 422, "request", "validation_failed");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 5,,6\r\n\r\n12345"),
      "rest_v1_job_routes_not_ready", 400, "request", "malformed_request");

  const auto expect_with_empty_element = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nExpect: , 100-continue\r\n\r\n",
      {"1", "\r\nx\r\n0\r\n\r\n"}, &http11_expect_continue, true);
  result |= Check(http11_expect_continue, "rest_v1_job_routes_not_ready",
                  "Expect list did not produce 100 Continue");
  result |= ExpectError(expect_with_empty_element, "rest_v1_job_routes_not_ready", 400, "request",
                        "malformed_json");

  bool quoted_expect_continue = false;
  const auto quoted_expect_response = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 2\r\nExpect: foo=\"a,100-continue,b\"\r\n\r\n",
      {"{}"}, &quoted_expect_continue);
  result |= Check(!quoted_expect_continue, "rest_v1_job_routes_not_ready",
                  "quoted Expect extension produced 100 Continue");
  result |= ExpectError(quoted_expect_response, "rest_v1_job_routes_not_ready", 422, "request",
                        "validation_failed");

  bool http10_expect_continue = false;
  const auto http10_expect_response = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.0\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 2\r\nExpect: 100-continue\r\n\r\n",
      {"{}"}, &http10_expect_continue);
  result |= Check(!http10_expect_continue, "rest_v1_job_routes_not_ready",
                  "HTTP/1.0 Expect produced 100 Continue");
  result |= ExpectError(http10_expect_response, "rest_v1_job_routes_not_ready", 422, "request",
                        "validation_failed");
  return result;
}

int HealthAndReadiness() {
  FakeBackend backend;
  int result =
      ExpectStatus(Send(backend, Options{}, HealthRequest()), "rest_v1_health_and_readiness", 200);

  result |= ExpectStatus(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: :80\r\n\r\n"),
                         "rest_v1_health_and_readiness [Host: :80]", 200);

  Options large_prefix_options;
  const auto leading_empty_lines =
      std::string(large_prefix_options.combined_request_limit_bytes + 1, '\n');
  result |= ExpectStatus(Send(backend, large_prefix_options, leading_empty_lines + HealthRequest()),
                         "rest_v1_health_and_readiness", 200);

  std::string split_leading_empty_lines;
  while (split_leading_empty_lines.size() < large_prefix_options.combined_request_limit_bytes)
    split_leading_empty_lines.append("\r\n");
  split_leading_empty_lines.push_back('\r');
  result |= ExpectStatus(
      SendParts(backend, large_prefix_options, {split_leading_empty_lines, "\n" + HealthRequest()}),
      "rest_v1_health_and_readiness", 200);

  if (result == 0) {
    try {
      const auto body = nlohmann::json::parse(Send(backend, Options{}, HealthRequest()).body);
      result |=
          Check(body.at("status") == "ok", "rest_v1_health_and_readiness", "health body mismatch");
    } catch (...) {
      result |= Check(false, "rest_v1_health_and_readiness", "health body is not JSON");
    }
  }
  backend.readiness = {false, {ReadinessReason{"unresolved_jobs", {std::string(k_job_id)}}}};
  const auto ready = Send(backend, Options{}, HealthRequest("GET", "/v1/ready"));
  result |= ExpectStatus(ready, "rest_v1_health_and_readiness", 503);
  if (ready.status == 503) {
    try {
      const auto body = nlohmann::json::parse(ready.body);
      result |= Check(body.at("ready") == false, "rest_v1_health_and_readiness",
                      "readiness body is ready");
      result |= Check(body.at("reasons").at(0).at("code") == "unresolved_jobs",
                      "rest_v1_health_and_readiness", "readiness reason mismatch");
      result |= Check(
          body.at("reasons").at(0).at("job_ids") == nlohmann::json::array({std::string(k_job_id)}),
          "rest_v1_health_and_readiness", "readiness Job IDs mismatch");
    } catch (...) {
      result |= Check(false, "rest_v1_health_and_readiness", "readiness body is not JSON");
    }
  }
  return result;
}

int MalformedRequest() {
  FakeBackend backend;
  int result = 0;
  const auto request = "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nContent-Length: nope\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, request), "rest_v1_malformed_request_refused", 400,
                        "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nBrokenField\r\n\r\n"),
      "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nContent-Length: 5\r\n"
                             "Content-Length: 6\r\n\r\n"),
                        "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nContent-Length: 5, 6\r\n\r\n"),
                  "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 18446744073709551616x\r\n\r\n"),
      "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 18446744073709551616, 18446744073709551616x\r\n\r\n"),
      "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nHost Name: loopback\r\n\r\n"),
                  "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{}, "GET /v1/health HTTP/1.1 extra\r\nHost: loopback\r\n\r\n"),
      "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  return result;
}

int BareLfRefused() {
  FakeBackend backend;
  Options options;
  options.header_timeout_ms = 5000;
  const auto expect_malformed = [&](const std::string& request, std::string_view case_name) {
    const auto started = std::chrono::steady_clock::now();
    const auto response = Send(backend, options, request, std::chrono::milliseconds(500));
    int check =
        ExpectError(response, "rest_v1_bare_lf_refused", 400, "request", "malformed_request");
    check |=
        Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
              "rest_v1_bare_lf_refused", std::string(case_name) + " waited for the header timeout");
    return check;
  };
  int result =
      expect_malformed("GET /v1/health HTTP/1.1\nHost: loopback\n\n", "LF-terminated request");
  result |=
      expect_malformed("GET /v1/health HTTP/1.1\r\nHost: loopback\n\n", "LF-terminated field");
  result |= expect_malformed("GET /v1/health HTTP/1.1\r\nHost: loopback\r\n\n",
                             "LF-terminated empty line");

  const auto head_lf = Send(backend, options, "HEAD /v1/health HTTP/1.1\nHost: loopback\n\n",
                            std::chrono::milliseconds(500));
  result |= ExpectStatus(head_lf, "rest_v1_bare_lf_refused", 400);
  result |=
      Check(head_lf.body.empty(), "rest_v1_bare_lf_refused", "HEAD malformed response has content");
  return result;
}

int Http10Answered() {
  FakeBackend backend;
  int result = 0;
  const auto expect_http11 = [&](const Response& response) {
    int check = ExpectStatus(response, "rest_v1_http10_request_answered", 200);
    check |= Check(response.version == "HTTP/1.1", "rest_v1_http10_request_answered",
                   "response version is not HTTP/1.1");
    return check;
  };
  result |= expect_http11(Send(backend, Options{}, HealthRequest("GET", "/v1/health", "HTTP/1.0")));
  result |= expect_http11(Send(backend, Options{}, "GET /v1/health HTTP/1.0\r\n\r\n"));
  result |= expect_http11(
      SendParts(backend, Options{}, {"GET /v1/health HTTP/1.2", "\r\nHost: loopback\r\n\r\n"}));
  result |= expect_http11(
      SendParts(backend, Options{}, {"GET /v1/health HTTP/1.2\r", "\nHost: loopback\r\n\r\n"}));
  result |= expect_http11(
      SendParts(backend, Options{}, {"\r\n", "GET /v1/health HTTP/1.2\r\nHost: loopback\r\n\r\n"}));
  result |= expect_http11(SendParts(
      backend, Options{}, {"\r\n\n\r\n", "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n\r\n"}));

  Options unsupported_version_options;
  unsupported_version_options.header_timeout_ms = 5000;
  const auto unsupported_version_started = std::chrono::steady_clock::now();
  const auto unsupported_version =
      SendPartial(backend, unsupported_version_options, "GET /v1/health HTTP/2.0\r\n");
  const auto unsupported_version_elapsed =
      std::chrono::steady_clock::now() - unsupported_version_started;
  result |= ExpectError(unsupported_version, "rest_v1_http10_request_answered", 400, "request",
                        "malformed_request");
  result |=
      Check(unsupported_version_elapsed < std::chrono::seconds(1),
            "rest_v1_http10_request_answered", "unsupported version waited for the header timeout");
  return result;
}

int AbsoluteForm() {
  FakeBackend backend;
  struct Case {
    std::string_view target;
    std::string_view host;
    int status;
  };
  constexpr std::array cases = {
      Case{"http://example.test/v1/health", "loopback", 200},
      Case{"http://example.test", "loopback", 404},
      Case{"http://example.test?q", "loopback", 404},
      Case{"Http://example.test/v1/health", "loopback", 200},
      Case{"Http://example.test/v1/health", "", 400},
      Case{"foo:///v1/health", "loopback", 200},
      Case{"foo:///v1/health", "", 200},
      Case{"foo://:80/v1/health", "loopback", 200},
      Case{"foo://:80/v1/health", "", 200},
      Case{"foo://user@/v1/health", "loopback", 200},
      Case{"foo://user@/v1/health", "", 200},
      Case{"foo://", "loopback", 404},
      Case{"foo://", "", 404},
      Case{"foo://?q", "loopback", 404},
      Case{"foo://?q", "", 404},
      Case{"foo:/v1/health", "loopback", 200},
      Case{"foo:/v1/missing", "loopback", 404},
      Case{"foo:v1/health", "loopback", 404},
      Case{"foo:", "loopback", 404},
      Case{"foo:?q", "loopback", 404},
      Case{"foo:/v1/health?q", "loopback", 404},
      Case{"foo:v1/health?q", "loopback", 404},
      Case{"example.test:443", "loopback", 404},
      Case{"foo:/v1/health", "", 200},
      Case{"foo:v1/health", "", 404},
      Case{"foo:", "", 404},
      Case{"foo:?q", "", 404},
      Case{"http://example.test/v1/health", "", 400},
      Case{"1foo:/v1/health", "loopback", 400},
      Case{"foo:/v1/health%GG", "loopback", 400},
      Case{"foo:v1/health%GG", "loopback", 400},
      Case{"foo:?q=%GG", "loopback", 400},
  };
  int result = 0;
  for (const auto& test_case : cases) {
    const auto name = std::string("rest_v1_absolute_form_target [") +
                      std::string(test_case.target) + ", Host: " + std::string(test_case.host) +
                      "]";
    const auto request = std::string("GET ") + std::string(test_case.target) +
                         " HTTP/1.1\r\nHost: " + std::string(test_case.host) + "\r\n\r\n";
    const auto response = Send(backend, Options{}, request);
    if (test_case.status == 200)
      result |= ExpectStatus(response, name, 200);
    else
      result |= ExpectError(response, name, test_case.status, "request",
                            test_case.status == 404 ? "route_not_found" : "malformed_request");
  }
  return result;
}

int TargetsWithoutPath() {
  FakeBackend backend;
  int result = ExpectError(Send(backend, Options{}, HealthRequest("OPTIONS", "*")),
                           "rest_v1_targets_without_a_path", 404, "request", "route_not_found");
  result |= ExpectError(Send(backend, Options{}, HealthRequest("CONNECT", "example.test:443")),
                        "rest_v1_targets_without_a_path", 404, "request", "route_not_found");
  return result;
}

int HeaderTimeout() {
  FakeBackend backend;
  Options options;
  options.header_timeout_ms = 25;
  const auto response =
      SendPartial(backend, options, "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n");
  return ExpectError(response, "rest_v1_header_receive_timeout", 408, "request", "request_timeout");
}

int TargetTooLong() {
  FakeBackend backend;
  Options options;
  options.target_limit_bytes = 8192;
  const std::string target_prefix = "http://";
  const std::string target_suffix = "/v1/health";
  const auto authority_size = static_cast<std::size_t>(options.target_limit_bytes) -
                              target_prefix.size() - target_suffix.size();
  const auto exact_target = target_prefix + std::string(authority_size, 'a') + target_suffix;
  int result = ExpectStatus(Send(backend, options, HealthRequest("GET", exact_target)),
                            "rest_v1_target_too_long", 200);
  const auto over_target = target_prefix + std::string(authority_size + 1, 'a') + target_suffix;
  result |= ExpectError(Send(backend, options, HealthRequest("GET", over_target)),
                        "rest_v1_target_too_long", 414, "request", "target_too_long");
  return result;
}

int HeadersTooLarge() {
  FakeBackend backend;
  Options options;
  options.header_limit_bytes = 16384;
  const std::string header_prefix = "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nX-Fill: ";
  const std::string header_suffix = "\r\n\r\n";
  const auto fill_size = static_cast<std::size_t>(options.header_limit_bytes) -
                         header_prefix.size() - header_suffix.size();
  const auto exact_header = header_prefix + std::string(fill_size, 'a') + header_suffix;
  const auto over_header = header_prefix + std::string(fill_size + 1, 'a') + header_suffix;
  const std::string leading_empty_lines = "\r\n\n";
  int result = ExpectStatus(Send(backend, options, leading_empty_lines + exact_header),
                            "rest_v1_headers_too_large", 200);
  result |= ExpectError(Send(backend, options, leading_empty_lines + over_header),
                        "rest_v1_headers_too_large", 431, "request", "headers_too_large");
  struct Case {
    std::string_view framing;
    std::string_view body;
  };
  constexpr std::array cases = {
      Case{"Content-Length:0", ""},
      Case{"Content-Length:1", "x"},
      Case{"Transfer-Encoding:chunked", "0\r\n\r\n"},
      Case{"Content-Length: 0", ""},
      Case{"Content-Length: 1", "x"},
      Case{"Transfer-Encoding: chunked", "0\r\n\r\n"},
  };
  for (const auto& test_case : cases) {
    const auto name =
        std::string("rest_v1_headers_too_large [") + std::string(test_case.framing) + "]";
    const auto prefix = std::string("POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\n") +
                        "Content-Type: application/json\r\n" + std::string(test_case.framing) +
                        "\r\nX-Fill: ";
    const auto padding =
        static_cast<std::size_t>(options.header_limit_bytes) - prefix.size() - header_suffix.size();
    auto header = prefix;
    header.append(padding, 'a').append(header_suffix);
    result |= ExpectError(Send(backend, options, header + std::string(test_case.body)), name, 400,
                          "request", "malformed_json");
    auto too_large = prefix;
    too_large.append(padding + 1, 'a').append(header_suffix);
    result |= ExpectError(Send(backend, options, too_large + std::string(test_case.body)),
                          name + " over limit", 431, "request", "headers_too_large");
  }
  return result;
}

int FragmentedNearLimitHeaders() {
  FakeBackend backend;
  Options options;
  options.header_limit_bytes = 16384;

  struct Case {
    std::string method;
    std::string target;
    std::string content_type;
    int status;
    std::string_view domain;
    const char* code;
  };
  const std::array cases = {
      Case{"POST", "/v1/jobs", "application/json", 400, "request", "malformed_json"},
      Case{"POST", "/v1/nope", "application/json", 404, "request", "route_not_found"},
      Case{"PUT", "/v1/jobs", "application/json", 405, "request", "method_not_allowed"},
      Case{"POST", "/v1/jobs", "text/plain", 415, "request", "unsupported_media_type"},
  };

  int result = 0;
  for (const bool chunked : {false, true}) {
    for (const auto& test_case : cases) {
      const auto framing = chunked ? "Transfer-Encoding: chunked\r\n" : "Content-Length: 1000\r\n";
      const auto body =
          chunked ? "3e8\r\n" + std::string(1000, 'x') + "\r\n0\r\n\r\n" : std::string(1000, 'x');
      std::string header =
          test_case.method + " " + test_case.target +
          " HTTP/1.1\r\nHost: loopback\r\nContent-Type: " + test_case.content_type + "\r\n" +
          framing + "X-Fill: ";
      constexpr std::size_t k_header_size = 16300;
      const auto fill_size = k_header_size - header.size() - 4;
      header.append(fill_size, 'a');
      header.append("\r\n\r\n");
      const auto request = header + body;
      const auto response =
          SendParts(backend, options, {request.substr(0, 100), request.substr(100)});
      result |= ExpectError(response, "rest_v1_body_bound_after_routing", test_case.status,
                            test_case.domain, test_case.code);
    }
  }
  return result;
}

int UnknownTransferCoding() {
  FakeBackend backend;
  int result = ExpectError(
      Send(backend, Options{},
           "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: gzip\r\n\r\n"),
      "rest_v1_unknown_transfer_coding", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/nope HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: gzip, chunked\r\n\r\n"),
                        "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/nope HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: gzip,,chunked\r\n\r\n"),
                        "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/nope HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: , gzip , , chunked,\r\n\r\n"),
                        "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");

  const auto transfer_prefix =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n";
  const std::array<std::string, 6> quoted_transfer_values = {
      "gzip;foo=\"a,b\", chunked",     "gzip;foo=\"a\\,b\", chunked",
      "gzip;foo=\"a\\\",b\", chunked", "gzip;foo=\"a,b\";bar=token, chunked",
      "gzip;foo=bar;baz=qux, chunked", "gzip;foo=\"a,b\";bar=\"c,d\", chunked",
  };
  for (const auto& transfer_value : quoted_transfer_values) {
    result |= ExpectError(
        Send(backend, Options{},
             std::string(transfer_prefix) + "Transfer-Encoding: " + transfer_value + "\r\n\r\n"),
        "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
  }
  auto obs_text_transfer = std::string("gzip;foo=\"a");
  obs_text_transfer.push_back(static_cast<char>(0x80));
  obs_text_transfer.append(",b\", chunked");
  result |= ExpectError(
      Send(backend, Options{},
           std::string(transfer_prefix) + "Transfer-Encoding: " + obs_text_transfer + "\r\n\r\n"),
      "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
  result |=
      ExpectError(Send(backend, Options{},
                       std::string(transfer_prefix) + "Transfer-Encoding: gzip;foo=\"a,b\"\r\n"
                                                      "Transfer-Encoding: chunked\r\n\r\n"),
                  "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
  return result;
}

int BodyBoundAfterRouting() {
  FakeBackend backend;
  int result = FragmentedNearLimitHeaders();
  const auto large_unknown_route =
      "GET /v1/nope HTTP/1.1\r\nHost: loopback\r\nContent-Length: 1048577\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, large_unknown_route),
                        "rest_v1_body_bound_after_routing", 404, "request", "route_not_found");
  const auto large_wrong_method =
      "PUT /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 1048577\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, large_wrong_method),
                        "rest_v1_body_bound_after_routing", 405, "request", "method_not_allowed");

  Options chunked_body_options;
  chunked_body_options.body_limit_bytes = 4096;
  const auto over_body_chunk = std::string("1001\r\n{") + std::string(4095, ' ') + "}\r\n0\r\n\r\n";
  const auto chunked_unknown_route =
      "POST /v1/nope HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n" +
      over_body_chunk;
  result |= ExpectError(Send(backend, chunked_body_options, chunked_unknown_route),
                        "rest_v1_body_bound_after_routing", 404, "request", "route_not_found");
  const auto chunked_wrong_method =
      "PUT /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n" +
      over_body_chunk;
  result |= ExpectError(Send(backend, chunked_body_options, chunked_wrong_method),
                        "rest_v1_body_bound_after_routing", 405, "request", "method_not_allowed");

  const auto large_wrong_media_type =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: text/plain\r\n"
      "Content-Length: 1048577\r\n\r\n";
  result |=
      ExpectError(Send(backend, Options{}, large_wrong_media_type),
                  "rest_v1_body_bound_after_routing", 415, "request", "unsupported_media_type");
  const auto large_known_route =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 1048577\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, large_known_route),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");

  Options exact_options;
  exact_options.body_limit_bytes = 4096;
  const auto exact_body =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 4096\r\n\r\n" +
      std::string(4096, 'x');
  result |= ExpectError(Send(backend, exact_options, exact_body),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_json");

  Options chunked_options;
  chunked_options.body_limit_bytes = 4096;
  const auto chunked_over_body =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n1001\r\n{" +
      std::string(4095, ' ') + "}\r\n0\r\n\r\n";
  result |= ExpectError(Send(backend, chunked_options, chunked_over_body),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");

  Options raw_options;
  raw_options.header_limit_bytes = 8192;
  raw_options.body_limit_bytes = 4096;
  raw_options.combined_request_limit_bytes = 16384;

  const auto content_length_prefix =
      std::string(
          "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
          "Content-Length: ") +
      std::to_string(raw_options.body_limit_bytes) + "\r\nX-Fill: ";
  const auto content_length_suffix = std::string("\r\n\r\n");
  const auto content_length_header =
      content_length_prefix +
      std::string(static_cast<std::size_t>(raw_options.header_limit_bytes) -
                      content_length_prefix.size() - content_length_suffix.size(),
                  'a') +
      content_length_suffix;
  const auto content_length_body = std::string(raw_options.body_limit_bytes, 'x');
  const auto content_length_wire = content_length_header + content_length_body;
  const auto content_length_padding =
      static_cast<std::size_t>(raw_options.combined_request_limit_bytes) -
      content_length_wire.size();
  const auto exact_content_length = content_length_wire + std::string(content_length_padding, 'p');
  result |=
      Check(exact_content_length.size() == raw_options.combined_request_limit_bytes,
            "rest_v1_body_bound_after_routing", "Content-Length boundary request size mismatch");
  result |= ExpectError(Send(backend, raw_options, exact_content_length),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_json");

  const auto chunked_prefix = std::string(
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n2;extension=\"");
  const auto chunked_suffix = std::string("\"\r\n{}\r\n0\r\n\r\n");
  const auto chunked_fixed_size = chunked_prefix.size() + chunked_suffix.size();
  const auto exact_extension_size =
      static_cast<std::size_t>(raw_options.combined_request_limit_bytes) - chunked_fixed_size;
  const auto make_chunked_request = [&](std::size_t extension_size) {
    return chunked_prefix + std::string(extension_size, 'a') + chunked_suffix;
  };
  const auto exact_chunked_body = make_chunked_request(exact_extension_size);
  const auto chunked_over_combined = make_chunked_request(exact_extension_size + 1);
  result |= Check(exact_chunked_body.size() == raw_options.combined_request_limit_bytes,
                  "rest_v1_body_bound_after_routing", "chunked boundary request size mismatch");
  result |= ExpectError(Send(backend, raw_options, exact_chunked_body),
                        "rest_v1_body_bound_after_routing", 422, "request", "validation_failed");
  result |= ExpectError(Send(backend, raw_options, chunked_over_combined),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");

  Options pipelined_chunked_options = raw_options;
  ++pipelined_chunked_options.combined_request_limit_bytes;
  const auto pipelined_chunked_size =
      static_cast<std::size_t>(pipelined_chunked_options.combined_request_limit_bytes) -
      chunked_fixed_size;
  const auto pipelined_chunked_request =
      make_chunked_request(pipelined_chunked_size) + HealthRequest();
  result |= Check(
      pipelined_chunked_request.size() > pipelined_chunked_options.combined_request_limit_bytes,
      "rest_v1_body_bound_after_routing",
      "pipelined chunked request did not cross the combined cap");
  result |= ExpectError(Send(backend, pipelined_chunked_options, pipelined_chunked_request),
                        "rest_v1_body_bound_after_routing", 422, "request", "validation_failed");

  const auto pipelined_content_length = exact_content_length + HealthRequest();
  result |= Check(pipelined_content_length.size() > raw_options.combined_request_limit_bytes,
                  "rest_v1_body_bound_after_routing",
                  "pipelined Content-Length request did not cross the combined cap");
  result |= ExpectError(Send(backend, raw_options, pipelined_content_length),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_json");

  const auto unknown_chunked_over_combined =
      std::string(
          "POST /v1/nope HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
          "Transfer-Encoding: chunked\r\n\r\n2;extension=\"") +
      std::string(exact_extension_size + 1, 'a') + "\"\r\n{}\r\n0\r\n\r\n";
  result |= ExpectError(Send(backend, raw_options, unknown_chunked_over_combined),
                        "rest_v1_body_bound_after_routing", 404, "request", "route_not_found");

  const auto exact_chunked_body_limit =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n1000\r\n{" +
      std::string(raw_options.body_limit_bytes - 2, ' ') + "}\r\n0\r\n\r\n";
  result |= ExpectError(Send(backend, raw_options, exact_chunked_body_limit),
                        "rest_v1_body_bound_after_routing", 422, "request", "validation_failed");

  const auto overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, overflow), "rest_v1_body_bound_after_routing", 413,
                        "request", "payload_too_large");
  const auto repeated_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 18446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, repeated_overflow),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");
  const auto padded_overflow_pair =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 018446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, padded_overflow_pair),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");
  const auto padded_overflow_unknown_route =
      "POST /v1/nope HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 018446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, padded_overflow_unknown_route),
                        "rest_v1_body_bound_after_routing", 404, "request", "route_not_found");
  const auto padded_overflow_wrong_method =
      "PUT /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 018446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, padded_overflow_wrong_method),
                        "rest_v1_body_bound_after_routing", 405, "request", "method_not_allowed");
  const auto padded_overflow_single =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 018446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, padded_overflow_single),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");
  const auto conflicting_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 18446744073709551617\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, conflicting_overflow),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_request");
  const auto quoted_content_length =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 5,\"5\"\r\n\r\n12345";
  result |= ExpectError(Send(backend, Options{}, quoted_content_length),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_request");
  const auto bare_lf_after_chunk_data =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n1\r\nx\nFFFFFFFFFFFFFFFFF\r\n";
  result |= ExpectError(Send(backend, Options{}, bare_lf_after_chunk_data),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_request");
  const auto overflow_with_non_hex_suffix =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFzz\r\n";
  result |= ExpectError(Send(backend, Options{}, overflow_with_non_hex_suffix),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_request");
  const auto leading_space_before_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\n FFFFFFFFFFFFFFFFF\r\n";
  result |= ExpectError(Send(backend, Options{}, leading_space_before_overflow),
                        "rest_v1_body_bound_after_routing", 400, "request", "malformed_request");
  const auto valid_chunk_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\n";
  result |= ExpectError(Send(backend, Options{}, valid_chunk_overflow),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");
  const auto overflow_with_long_extension =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF;ext=\"" +
      std::string(8200, 'a') + "\"\r\n";
  result |= ExpectError(Send(backend, Options{}, overflow_with_long_extension),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");

  Options partial_size_options;
  partial_size_options.body_limit_bytes = 4096;
  partial_size_options.body_timeout_ms = 50;
  const auto partial_size_header =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n";
  const auto over_limit_prefix = "1000\r\n" + std::string(4096, 'x') + "\r\n1\r\nx\r\n";
  const std::array<std::string_view, 4> partial_size_lines = {"F", "F\r", "F;ext=\"", "1;abc="};
  for (const auto partial_size_line : partial_size_lines) {
    bool saw_continue = false;
    const auto response =
        SendChunkedParts(backend, partial_size_options, partial_size_header,
                         {over_limit_prefix + std::string(partial_size_line)}, &saw_continue, true);
    result |= Check(saw_continue, "rest_v1_body_bound_after_routing",
                    "body-bound probe did not receive 100 Continue");
    result |= ExpectError(response, "rest_v1_body_bound_after_routing", 413, "request",
                          "payload_too_large");
  }

  bool saw_continue = false;
  const auto exact_partial_response =
      SendChunkedParts(backend, partial_size_options, partial_size_header,
                       {"1000\r\n" + std::string(4096, 'x') + "\r\nF"}, &saw_continue, true);
  result |= Check(saw_continue, "rest_v1_body_bound_after_routing",
                  "exact-bound probe did not receive 100 Continue");
  result |= ExpectError(exact_partial_response, "rest_v1_body_bound_after_routing", 408, "request",
                        "request_timeout");

  saw_continue = false;
  const auto eof_partial_response =
      SendChunkedParts(backend, partial_size_options, partial_size_header,
                       {over_limit_prefix + "F"}, &saw_continue, true, true);
  result |= Check(saw_continue, "rest_v1_body_bound_after_routing",
                  "EOF body-bound probe did not receive 100 Continue");
  result |= ExpectError(eof_partial_response, "rest_v1_body_bound_after_routing", 413, "request",
                        "payload_too_large");
  return result;
}

int BodyTimeout() {
  FakeBackend backend;
  Options options;
  options.body_timeout_ms = 25;
  const auto request =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 10\r\n\r\n{";
  return ExpectError(SendPartial(backend, options, request), "rest_v1_body_receive_timeout", 408,
                     "request", "request_timeout");
}

int QueryRefused() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{}, HealthRequest("GET", "/v1/health?x=1")),
                     "rest_v1_query_refused", 404, "request", "route_not_found");
}

struct HeadRefusalVector {
  std::string name;
  Options options;
  std::string get_request;
  std::string head_request;
  std::string head_header_reference;
  bool partial = false;
  int get_status = 0;
  int head_status = 0;
  std::string_view get_domain;
  const char* get_code = "";
};

int CheckHeadRefusalVector(Backend& backend, const HeadRefusalVector& vector) {
  const auto send = [&](const std::string& request) {
    return vector.partial ? SendPartial(backend, vector.options, request)
                          : Send(backend, vector.options, request);
  };
  const auto get_response = send(vector.get_request);
  const auto head_response = send(vector.head_request);
  int result =
      ExpectError(get_response, vector.name, vector.get_status, vector.get_domain, vector.get_code);
  result |= ExpectStatus(head_response, vector.name, vector.head_status);
  result |= Check(head_response.body.empty(), vector.name, "HEAD response carried content");

  const auto header_reference = vector.head_header_reference.empty()
                                    ? get_response
                                    : Send(backend, vector.options, vector.head_header_reference);
  for (const auto header : {"allow", "content-type", "content-length"}) {
    const auto expected = header_reference.headers.find(header);
    const auto actual = head_response.headers.find(header);
    result |= Check(
        (expected == header_reference.headers.end()) == (actual == head_response.headers.end()),
        vector.name, std::string("HEAD ") + header + " presence mismatch");
    if (expected != header_reference.headers.end() && actual != head_response.headers.end()) {
      result |= Check(actual->second == expected->second, vector.name,
                      std::string("HEAD ") + header + " value mismatch");
    }
  }
  return result;
}

int UnlistedMethods() {
  FakeBackend backend;
  constexpr std::string_view name = "rest_v1_unlisted_methods_refused";
  std::vector<HeadRefusalVector> vectors;
  vectors.push_back(
      HeadRefusalVector{std::string(name),
                        Options{},
                        HealthRequest("GET", "/v1/jobs/" + std::string(k_job_id) + "/cancel"),
                        HealthRequest("HEAD", "/v1/jobs/" + std::string(k_job_id) + "/cancel"),
                        {},
                        false,
                        405,
                        405,
                        "request",
                        "method_not_allowed"});
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      HealthRequest("GET", "/v1/missing"),
                                      HealthRequest("HEAD", "/v1/missing"),
                                      {},
                                      false,
                                      404,
                                      404,
                                      "request",
                                      "route_not_found"});

  Options oversized_options;
  oversized_options.header_limit_bytes = 8192;
  const auto oversized_target = "/" + std::string(9000, 'a');
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      oversized_options,
                                      HealthRequest("GET", oversized_target),
                                      HealthRequest("HEAD", oversized_target),
                                      {},
                                      false,
                                      431,
                                      431,
                                      "request",
                                      "headers_too_large"});
  Options timeout_options;
  timeout_options.header_timeout_ms = 25;
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      timeout_options,
                                      "GET /v1/health HTTP/1.1",
                                      "HEAD /v1/health HTTP/1.1",
                                      {},
                                      true,
                                      408,
                                      408,
                                      "request",
                                      "request_timeout"});
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      HealthRequest("GET", "/v1/health", "HTTP/2.0"),
                                      HealthRequest("HEAD", "/v1/health", "HTTP/2.0"),
                                      {},
                                      false,
                                      400,
                                      400,
                                      "request",
                                      "malformed_request"});
  Options target_options;
  target_options.target_limit_bytes = 2048;
  const auto long_target = "/" + std::string(3000, 'a');
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      target_options,
                                      HealthRequest("GET", long_target),
                                      HealthRequest("HEAD", long_target),
                                      {},
                                      false,
                                      414,
                                      414,
                                      "request",
                                      "target_too_long"});
  const auto bodied = [](std::string_view method, std::string_view target,
                         std::string_view content_type, std::string_view framing,
                         std::string_view body) {
    return std::string(method) + " " + std::string(target) +
           " HTTP/1.1\r\nHost: loopback\r\nContent-Type: " + std::string(content_type) + "\r\n" +
           std::string(framing) + "\r\n\r\n" + std::string(body);
  };
  // HEAD remains unlisted per ADR-0009, so body-gated HEAD forms stop at 405 before media/size.
  const auto wrong_media_get = bodied("GET", "/v1/jobs", "text/plain", "Content-Length: 1", "x");
  const auto wrong_media_head = bodied("HEAD", "/v1/jobs", "text/plain", "Content-Length: 1", "x");
  vectors.push_back(HeadRefusalVector{std::string(name), Options{}, wrong_media_get,
                                      wrong_media_head, HealthRequest("PUT", "/v1/jobs"), false,
                                      415, 405, "request", "unsupported_media_type"});
  const auto oversized_length_get =
      bodied("GET", "/v1/jobs", "application/json", "Content-Length: 1048577", "");
  const auto oversized_length_head =
      bodied("HEAD", "/v1/jobs", "application/json", "Content-Length: 1048577", "");
  vectors.push_back(HeadRefusalVector{std::string(name), Options{}, oversized_length_get,
                                      oversized_length_head, HealthRequest("PUT", "/v1/jobs"),
                                      false, 413, 405, "request", "payload_too_large"});
  Options chunk_options;
  chunk_options.body_limit_bytes = 4096;
  const auto oversized_chunk = "1001\r\n" + std::string(4097, 'x') + "\r\n0\r\n\r\n";
  const auto oversized_chunk_get =
      bodied("GET", "/v1/jobs", "application/json", "Transfer-Encoding: chunked", oversized_chunk);
  const auto oversized_chunk_head =
      bodied("HEAD", "/v1/jobs", "application/json", "Transfer-Encoding: chunked", oversized_chunk);
  vectors.push_back(HeadRefusalVector{std::string(name), chunk_options, oversized_chunk_get,
                                      oversized_chunk_head, HealthRequest("PUT", "/v1/jobs"), false,
                                      413, 405, "request", "payload_too_large"});
  const auto malformed_get = "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nBrokenField\r\n\r\n";
  const auto malformed_head = "HEAD /v1/health HTTP/1.1\r\nHost: loopback\r\nBrokenField\r\n\r\n";
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      malformed_get,
                                      malformed_head,
                                      {},
                                      false,
                                      400,
                                      400,
                                      "request",
                                      "malformed_request"});

  const auto empty_target_get = "GET  HTTP/1.1\r\nHost: x\r\n\r\n";
  const auto empty_target_head = "HEAD  HTTP/1.1\r\nHost: x\r\n\r\n";
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      empty_target_get,
                                      empty_target_head,
                                      {},
                                      false,
                                      400,
                                      400,
                                      "request",
                                      "malformed_request"});
  const auto missing_version_get = "GET /v1/health\r\nHost: x\r\n\r\n";
  const auto missing_version_head = "HEAD /v1/health\r\nHost: x\r\n\r\n";
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      missing_version_get,
                                      missing_version_head,
                                      {},
                                      false,
                                      400,
                                      400,
                                      "request",
                                      "malformed_request"});
  const auto incomplete_request_line_get = "GET \r\n\r\n";
  const auto incomplete_request_line_head = "HEAD \r\n\r\n";
  vectors.push_back(HeadRefusalVector{std::string(name),
                                      Options{},
                                      incomplete_request_line_get,
                                      incomplete_request_line_head,
                                      {},
                                      false,
                                      400,
                                      400,
                                      "request",
                                      "malformed_request"});

  int result = 0;
  for (const auto& vector : vectors) result |= CheckHeadRefusalVector(backend, vector);
  return result;
}

int ErrorDetailsEmpty() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{}, HealthRequest("GET", "/v1/nope")),
                     "rest_v1_error_details_empty", 404, "request", "route_not_found");
}

int InvalidTarget() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{}, HealthRequest("GET", "foo")),
                  "rest_v1_invalid_request_target_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, HealthRequest("GET", "http://host:bad/v1/health")),
                  "rest_v1_invalid_request_target_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, HealthRequest("GET", "*")),
                  "rest_v1_invalid_request_target_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, HealthRequest("CONNECT", "[:::]:443")),
                  "rest_v1_invalid_request_target_refused", 400, "request", "malformed_request");
  for (const auto& target : {":443", "example.test:", "[::1]:", "example.test:65536",
                             "[::1]:9999999999999999999999999"}) {
    result |=
        ExpectError(Send(backend, Options{}, HealthRequest("CONNECT", target)),
                    "rest_v1_invalid_request_target_refused", 400, "request", "malformed_request");
  }
  for (const auto& target : {"example.test:65535", "[::1]:443", "example.test:0", "[::1]:000443"}) {
    result |=
        ExpectError(Send(backend, Options{}, HealthRequest("CONNECT", target)),
                    "rest_v1_invalid_request_target_refused", 404, "request", "route_not_found");
  }
  for (const auto target :
       {"http:/v1/health", "http:v1/health", "http:", "https:?q", "http:///v1/health",
        "http://user@/v1/health", "http://:80/v1/health", "HTTP:/v1/health", "1foo:/v1/health",
        "foo:/v1/health%GG", "foo:v1/health%GG", "foo:?q=%GG"}) {
    for (const auto host : {"loopback", ""}) {
      const auto name = std::string("rest_v1_invalid_request_target_refused [") + target +
                        ", Host: " + host + "]";
      const auto request = std::string("GET ") + target + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
      result |=
          ExpectError(Send(backend, Options{}, request), name, 400, "request", "malformed_request");
    }
  }
  return result;
}

int MissingHost() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\n\r\n"),
                     "rest_v1_missing_host_refused", 400, "request", "malformed_request");
}

int RepeatedHost() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nHost: loopback\r\n\r\n"),
                  "rest_v1_repeated_host_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/health HTTP/1.0\r\nHost: loopback\r\nHost: loopback\r\n\r\n"),
                        "rest_v1_repeated_host_refused", 400, "request", "malformed_request");
  return result;
}

int InvalidHost() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: bad host\r\n\r\n"),
                  "rest_v1_invalid_host_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: [:::]\r\n\r\n"),
                        "rest_v1_invalid_host_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: host:bad\r\n\r\n"),
                  "rest_v1_invalid_host_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: [::1]junk\r\n\r\n"),
                  "rest_v1_invalid_host_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{}, "GET /v1/health HTTP/1.0\r\nHost: bad host\r\n\r\n"),
                  "rest_v1_invalid_host_refused", 400, "request", "malformed_request");
  return result;
}

int NonfinalTransferCoding() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{},
                          "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                          "Transfer-Encoding: gzip, chunked, gzip\r\n\r\n"),
                     "rest_v1_nonfinal_transfer_coding_refused", 400, "request",
                     "malformed_request");
}

int RepeatedChunked() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{},
                          "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                          "Transfer-Encoding: chunked, chunked\r\n\r\n"),
                     "rest_v1_repeated_chunked_refused", 400, "request", "malformed_request");
}

int ChunkedParameters() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                       "Transfer-Encoding: chunked;foo\r\n\r\n"),
                  "rest_v1_chunked_parameters_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: chunked;foo=bar\r\n\r\n"),
                        "rest_v1_chunked_parameters_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: chunked;foo=\"bar\"\r\n\r\n"),
                        "rest_v1_chunked_parameters_refused", 400, "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                             "Transfer-Encoding: gzip, chunked;foo=bar\r\n\r\n"),
                        "rest_v1_chunked_parameters_refused", 400, "request", "malformed_request");
  return result;
}

int InvalidTransferCoding() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                       "Transfer-Encoding: gzip;foo\r\n\r\n"),
                  "rest_v1_invalid_transfer_coding_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding: chunked\r\n , gzip\r\n\r\n2\r\n{}\r\n0\r\n\r\n"),
      "rest_v1_invalid_transfer_coding_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding:\r\n chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n"),
      "rest_v1_invalid_transfer_coding_refused", 400, "request", "malformed_request");

  const auto transfer_prefix =
      "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: gzip;foo=\"";
  auto raw_control = std::string(transfer_prefix);
  raw_control.push_back(static_cast<char>(0x01));
  raw_control.append("\", chunked\r\n\r\n");
  result |=
      ExpectError(Send(backend, Options{}, raw_control), "rest_v1_invalid_transfer_coding_refused",
                  400, "request", "malformed_request");

  auto escaped_control = std::string(transfer_prefix);
  escaped_control.append("\\");
  escaped_control.push_back(static_cast<char>(0x01));
  escaped_control.append("\", chunked\r\n\r\n");
  result |=
      ExpectError(Send(backend, Options{}, escaped_control),
                  "rest_v1_invalid_transfer_coding_refused", 400, "request", "malformed_request");

  auto valid_obs_text = std::string(transfer_prefix);
  valid_obs_text.push_back(static_cast<char>(0x80));
  valid_obs_text.append("\", chunked\r\n\r\n");
  result |=
      ExpectError(Send(backend, Options{}, valid_obs_text),
                  "rest_v1_invalid_transfer_coding_refused", 501, "request", "not_implemented");
  return result;
}

int TransferEncodingWithContentLengthRefused() {
  FakeBackend backend;
  int result = ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n"),
      "rest_v1_transfer_encoding_with_content_length_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding: gzip, chunked\r\nContent-Length: 0\r\n\r\n"),
      "rest_v1_transfer_encoding_with_content_length_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding:\r\nContent-Length: 0\r\n\r\n"),
      "rest_v1_transfer_encoding_with_content_length_refused", 400, "request", "malformed_request");
  return result;
}

int Http10TransferEncoding() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.0\r\nHost: loopback\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"),
                  "rest_v1_http10_transfer_encoding_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.0\r\nHost: loopback\r\n"
                       "Transfer-Encoding:\r\n\r\n"),
                  "rest_v1_http10_transfer_encoding_refused", 400, "request", "malformed_request");
  return result;
}

int ObsFoldRefused() {
  FakeBackend backend;
  int result = 0;
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Transfer-Encoding: gzip,\r\n chunked\r\n\r\n"),
      "rest_v1_obs_fold_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 0\r\nExpect:\r\n 100-continue\r\n\r\n"),
      "rest_v1_obs_fold_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\nHost: loop\r\n back\r\n\r\n"),
      "rest_v1_obs_fold_refused", 400, "request", "malformed_request");

  const auto folded_length =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616,\r\n 18446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, folded_length), "rest_v1_obs_fold_refused", 400,
                        "request", "malformed_request");
  result |= ExpectError(Send(backend, Options{},
                             "GET /v1/nope HTTP/1.1\r\nHost: loopback\r\nContent-Length: "
                             "18446744073709551616,\r\n 18446744073709551616\r\n\r\n"),
                        "rest_v1_obs_fold_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{},
           "PUT /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
           "Content-Length: 18446744073709551616,\r\n 18446744073709551616\r\n\r\n"),
      "rest_v1_obs_fold_refused", 400, "request", "malformed_request");

  result |= ExpectStatus(
      Send(backend, Options{}, "GET /v1/health HTTP/1.1\r\n \r\nHost: loopback\r\n\r\n"),
      "rest_v1_obs_fold_refused", 200);
  return result;
}

int Phase1PortsNotCalled() {
  const ScopedTempDirectory directory("phase1-composition-journal");
  {
    sitometron::journal::SegmentJournal journal(sitometron::journal::SystemFileSystem());
    const auto opened = journal.Open(directory.Path().string());
    if (!opened.ok) {
      return Check(false, "rest_v1_phase1_ports_not_called", "test Journal did not open");
    }
    const sitometron::core::Uuid id{std::string(k_job_id)};
    const sitometron::core::LogicalJobEvent event{
        1,
        1,
        sitometron::core::EventType::kJobCreated,
        sitometron::core::DiagnosticTimestamp{"2026-10-13T00:00:00Z"},
        id,
        sitometron::core::JobCreatedPayload{id}};
    if (journal.Commit(event) != sitometron::core::LogicalCommitResult::kCommitted) {
      return Check(false, "rest_v1_phase1_ports_not_called", "test Journal did not commit");
    }
  }
  const auto segment = directory.Path() / sitometron::journal::SegmentJournal::SegmentName(1);
  const auto bytes = [&] {
    std::ifstream input(segment, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto original = bytes();
  sitometron::daemon::StartupConfig config;
  config.journal_path = directory.Path().string();
  config.max_jobs = 1;
  config.http.listen_port = 0;
  sitometron::daemon::Phase1Composition composition(std::move(config));
  std::string error;
  int result = Check(composition.Start(error), "rest_v1_phase1_ports_not_called",
                     "Phase 1 composition failed to start: " + error);
  if (result == 0) {
    const auto readiness = composition.backend().GetReadiness();
    result |= Check(!readiness.ready, "rest_v1_phase1_ports_not_called",
                    "replayed unresolved Job did not close readiness");
    const auto response = SendToPort(composition.listen_port(), HealthRequest("GET", "/v1/ready"));
    result |= ExpectStatus(response, "rest_v1_phase1_ports_not_called", 503);
    if (response.status == 503) {
      try {
        const auto body = nlohmann::json::parse(response.body);
        result |= Check(body.at("ready") == false, "rest_v1_phase1_ports_not_called",
                        "replayed readiness body is ready");
        result |= Check(body.at("reasons").at(0).at("code") == "unresolved_jobs",
                        "rest_v1_phase1_ports_not_called", "replayed readiness reason mismatch");
        result |= Check(body.at("reasons").at(0).at("job_ids") ==
                            nlohmann::json::array({std::string(k_job_id)}),
                        "rest_v1_phase1_ports_not_called", "replayed readiness Job IDs mismatch");
      } catch (...) {
        result |= Check(false, "rest_v1_phase1_ports_not_called", "readiness body is not JSON");
      }
    }
    const std::string create_body = R"({"application_id":"app"})";
    const auto create =
        SendToPort(composition.listen_port(),
                   "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
                   "Content-Length: " +
                       std::to_string(create_body.size()) + "\r\n\r\n" + create_body);
    result |=
        ExpectError(create, "rest_v1_phase1_ports_not_called", 422, "job", "unknown_application");
    composition.Stop();
    result |= Check(bytes() == original, "rest_v1_phase1_ports_not_called",
                    "production empty-registry create changed the replayed Journal");
    result |= Check(composition.clock_reads() == 0, "rest_v1_phase1_ports_not_called",
                    "Phase 1 clock was read during replay/serve/shutdown");
    result |= Check(composition.identity_reads() == 0, "rest_v1_phase1_ports_not_called",
                    "Phase 1 identity source was read during replay/serve/shutdown");
  }
  return result;
}

struct ProcessResult {
  bool started = false;
  int exit_code = -1;
};

#ifdef _WIN32
std::string QuoteWindowsArgument(std::string_view value) {
  std::string quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back('"');
  std::size_t backslashes = 0;
  for (const char character : value) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

bool WaitForProcess(HANDLE process, std::chrono::milliseconds timeout) {
  return ::WaitForSingleObject(process, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
}

bool TerminateAndReap(HANDLE process) {
  (void)::TerminateProcess(process, 1);
  return WaitForProcess(process, k_process_timeout);
}
#else
bool WaitForChild(pid_t child, int& status, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) return true;
    if (waited < 0 && errno != EINTR) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    const auto wait_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), 25));
    (void)::poll(nullptr, 0, std::max(wait_ms, 1));
  }
}

bool TerminateAndReap(pid_t child, int& status) {
  (void)::kill(child, SIGTERM);
  if (WaitForChild(child, status, k_process_timeout)) return true;
  (void)::kill(child, SIGKILL);
  return WaitForChild(child, status, k_process_timeout);
}
#endif

ProcessResult RunAndWait(const std::string& executable, const std::vector<std::string>& arguments) {
  std::vector<std::string> values;
  values.reserve(arguments.size() + 1);
  values.push_back(executable);
  values.insert(values.end(), arguments.begin(), arguments.end());
#ifdef _WIN32
  std::string command_line;
  for (const auto& value : values) {
    if (!command_line.empty()) command_line.push_back(' ');
    command_line += QuoteWindowsArgument(value);
  }
  std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!::CreateProcessA(executable.c_str(), mutable_command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) {
    return {};
  }
  (void)::CloseHandle(process.hThread);
  bool reaped = WaitForProcess(process.hProcess, k_process_timeout);
  if (!reaped) reaped = TerminateAndReap(process.hProcess);
  if (!reaped) {
    (void)::CloseHandle(process.hProcess);
    return {true, -1};
  }
  DWORD exit_code = 1;
  const bool got_exit_code = ::GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  (void)::CloseHandle(process.hProcess);
  if (!got_exit_code) return {true, -1};
  return {true, static_cast<int>(exit_code)};
#else
  const auto child = ::fork();
  if (child < 0) return {};
  if (child == 0) {
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  bool reaped = WaitForChild(child, status, k_process_timeout);
  if (!reaped) reaped = TerminateAndReap(child, status);
  if (!reaped || !WIFEXITED(status)) return {true, -1};
  return {true, WEXITSTATUS(status)};
#endif
}

#ifndef _WIN32
struct RunningProcess {
  pid_t pid = -1;
  int output_fd = -1;
};

bool StartRunningProcess(const std::string& executable, const std::vector<std::string>& arguments,
                         RunningProcess& process) {
  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0) return false;
  std::vector<std::string> values;
  values.reserve(arguments.size() + 1);
  values.push_back(executable);
  values.insert(values.end(), arguments.begin(), arguments.end());
  const auto child = ::fork();
  if (child < 0) {
    (void)::close(descriptors[0]);
    (void)::close(descriptors[1]);
    return false;
  }
  if (child == 0) {
    (void)::close(descriptors[0]);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0 || ::dup2(descriptors[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    (void)::close(descriptors[1]);
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    _exit(127);
  }
  (void)::close(descriptors[1]);
  process = {child, descriptors[0]};
  return true;
}

bool ReadStartupLine(int descriptor, std::string& line) {
  const auto deadline = std::chrono::steady_clock::now() + k_process_timeout;
  std::array<char, 512> buffer{};
  for (;;) {
    const auto newline = line.find('\n');
    if (newline != std::string::npos) {
      line.resize(newline);
      return true;
    }
    if (line.size() > 4096) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    const auto wait_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));
    pollfd event{descriptor, POLLIN, 0};
    int ready = 0;
    do {
      ready = ::poll(&event, 1, std::max(wait_ms, 1));
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return false;
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count <= 0) return false;
    line.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

int StopRunningProcess(RunningProcess& process) {
  if (process.pid < 0) return -1;
  int status = 0;
  (void)::kill(process.pid, SIGTERM);
  bool reaped = WaitForChild(process.pid, status, k_process_timeout);
  if (!reaped) {
    (void)::kill(process.pid, SIGKILL);
    reaped = WaitForChild(process.pid, status, k_process_timeout);
  }
  (void)::close(process.output_fd);
  process = {};
  if (!reaped || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

std::uint16_t ListeningPort(std::string_view line) {
  constexpr std::string_view marker = "listening on 127.0.0.1:";
  const auto begin = line.find(marker);
  if (begin == std::string_view::npos) return 0;
  const auto port_begin = begin + marker.size();
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(line.data() + port_begin, line.data() + line.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() || value == 0 ||
      value > 65535) {
    return 0;
  }
  return static_cast<std::uint16_t>(value);
}
#endif

int SitometrondEntrypoint(const std::string& executable) {
  constexpr std::string_view name = "rest_v1_sitometrond_entrypoint";
  if (executable.empty()) return Check(false, name, "sitometrond executable path is missing");

  const ScopedTempDirectory directory("sitometrond-entrypoint-journal");
  int result = 0;
  const auto unknown = RunAndWait(executable, {"--unknown"});
  result |= Check(unknown.started && unknown.exit_code == 2, name,
                  "unknown option did not exit with status 2");
  const auto malformed =
      RunAndWait(executable, {"--journal", directory.Path().string(), "--body-limit", "nope"});
  result |= Check(malformed.started && malformed.exit_code == 2, name,
                  "malformed numeric option did not exit with status 2");
  const auto missing_journal =
      RunAndWait(executable, {"--listen", "127.0.0.1:0", "--max-jobs", "2"});
  result |= Check(missing_journal.started && missing_journal.exit_code == 2, name,
                  "missing required --journal did not exit with status 2");

#ifdef _WIN32
  // Windows has no portable console-signal path for this subprocess smoke.
  return result;
#else
  RunningProcess process;
  if (!StartRunningProcess(
          executable,
          {"--journal", directory.Path().string(), "--listen", "127.0.0.1:0", "--max-jobs", "2"},
          process)) {
    return result | Check(false, name, "could not start a valid sitometrond configuration");
  }
  std::string startup_line;
  const bool got_startup_line = ReadStartupLine(process.output_fd, startup_line);
  const auto port = got_startup_line ? ListeningPort(startup_line) : 0;
  result |= Check(port != 0, name, "startup output did not contain the bound ephemeral port");
  if (port != 0) {
    result |= ExpectStatus(SendToPort(port, HealthRequest()), name, 200);
    result |= ExpectStatus(SendToPort(port, HealthRequest("GET", "/v1/ready")), name, 200);
  }
  const auto exit_code = StopRunningProcess(process);
  result |= Check(exit_code == 0, name, "SIGTERM did not produce exit status 0");
  return result;
#endif
}

int SitometrondOptions() {
  int result = 0;
  const ScopedTempDirectory directory("sitometrond-options-journal");
  const auto journal_path = directory.Path().string();
  const auto valid = sitometron::daemon::ParseStartupOptions(
      {"--journal", journal_path, "--listen", "127.0.0.1:0", "--cancel-principal", "operator",
       "--max-jobs", "7", "--target-limit", "4096", "--header-limit", "8192", "--body-limit",
       "4096", "--header-timeout-ms", "17", "--body-timeout-ms", "19"});
  result |= Check(valid.ok, "rest_v1_sitometrond_options",
                  "documented valid configuration was refused: " + valid.error);
  if (valid.ok) {
    result |= Check(valid.config.http.listen_address == "127.0.0.1", "rest_v1_sitometrond_options",
                    "listen address mismatch");
    result |= Check(valid.config.http.listen_port == 0, "rest_v1_sitometrond_options",
                    "listen port mismatch");
    result |= Check(valid.config.max_jobs == 7, "rest_v1_sitometrond_options", "max-jobs mismatch");
    result |= Check(valid.config.http.combined_request_limit_bytes == 16384,
                    "rest_v1_sitometrond_options", "combined request bound mismatch");
  }

  const auto defaults = sitometron::daemon::ParseStartupOptions({"--journal", journal_path});
  result |= Check(defaults.ok, "rest_v1_sitometrond_options",
                  "documented defaults were refused: " + defaults.error);
  if (defaults.ok) {
    result |= Check(defaults.config.http.listen_address == "127.0.0.1",
                    "rest_v1_sitometrond_options", "default listen address mismatch");
    result |= Check(defaults.config.http.listen_port == 8080, "rest_v1_sitometrond_options",
                    "default listen port mismatch");
    result |= Check(defaults.config.http.cancel_principal == "local-operator",
                    "rest_v1_sitometrond_options", "default cancel principal mismatch");
    result |= Check(defaults.config.max_jobs == 32, "rest_v1_sitometrond_options",
                    "default max-jobs mismatch");
    result |= Check(defaults.config.http.target_limit_bytes == 8192, "rest_v1_sitometrond_options",
                    "default target limit mismatch");
    result |= Check(defaults.config.http.header_limit_bytes == 16384, "rest_v1_sitometrond_options",
                    "default header limit mismatch");
    result |= Check(defaults.config.http.body_limit_bytes == 65536, "rest_v1_sitometrond_options",
                    "default body limit mismatch");
    result |= Check(defaults.config.http.header_timeout_ms == 5000, "rest_v1_sitometrond_options",
                    "default header timeout mismatch");
    result |= Check(defaults.config.http.body_timeout_ms == 5000, "rest_v1_sitometrond_options",
                    "default body timeout mismatch");
    result |= Check(defaults.config.http.combined_request_limit_bytes == 86016,
                    "rest_v1_sitometrond_options", "default combined request bound mismatch");
  }

  const auto expect_rejected = [&](const std::vector<std::string>& args,
                                   std::string_view case_name) {
    const auto parsed = sitometron::daemon::ParseStartupOptions(args);
    return Check(!parsed.ok, "rest_v1_sitometrond_options",
                 std::string(case_name) + " command line was accepted");
  };
  const auto expect_accepted = [&](const std::vector<std::string>& args,
                                   std::string_view case_name) {
    const auto parsed = sitometron::daemon::ParseStartupOptions(args);
    return Check(parsed.ok, "rest_v1_sitometrond_options",
                 std::string(case_name) + " command line was refused: " + parsed.error);
  };
  result |= expect_rejected({}, "missing required --journal");
  result |= expect_rejected({"--journal", ""}, "empty --journal");
  result |= expect_rejected({"--journal", journal_path, "--unknown", "x"}, "unknown");
  result |= expect_rejected({"--journal", journal_path, "--journal", "other"}, "repeated");
  result |= expect_rejected({"--journal", journal_path, "--max-jobs"}, "missing value");
  result |= expect_rejected({"--journal", journal_path, "--body-limit", "nope"}, "malformed");
  result |=
      expect_rejected({"--journal", journal_path, "--listen", "127.0.0.1"}, "malformed listen");
  result |= expect_rejected({"--journal", journal_path, "--listen", "127.0.0.1:65536"},
                            "listen port out-of-range");
  result |= expect_rejected({"--journal", journal_path, "--cancel-principal", ""},
                            "cancel-principal below minimum");
  result |=
      expect_rejected({"--journal", journal_path, "--cancel-principal", std::string(257, 'x')},
                      "cancel-principal above maximum");
  result |= expect_rejected({"--journal", journal_path, "--cancel-principal", "operator\n"},
                            "cancel-principal non-visible");
  result |=
      expect_rejected({"--journal", journal_path, "--max-jobs", "0"}, "max-jobs below minimum");
  result |=
      expect_rejected({"--journal", journal_path, "--max-jobs", "1025"}, "max-jobs above maximum");
  result |= expect_rejected({"--journal", journal_path, "--target-limit", "2047"},
                            "target-limit below minimum");
  result |= expect_rejected({"--journal", journal_path, "--target-limit", "65537"},
                            "target-limit above maximum");
  result |= expect_rejected({"--journal", journal_path, "--header-limit", "8191"},
                            "header-limit below minimum");
  result |= expect_rejected({"--journal", journal_path, "--header-limit", "262145"},
                            "header-limit above maximum");
  result |= expect_rejected({"--journal", journal_path, "--body-limit", "4095"},
                            "body-limit below minimum");
  result |= expect_rejected({"--journal", journal_path, "--body-limit", "1048577"},
                            "body-limit above maximum");
  result |= expect_rejected({"--journal", journal_path, "--header-timeout-ms", "0"},
                            "header-timeout below minimum");
  result |= expect_rejected({"--journal", journal_path, "--header-timeout-ms", "60001"},
                            "header-timeout above maximum");
  result |= expect_rejected({"--journal", journal_path, "--body-timeout-ms", "0"},
                            "body-timeout below minimum");
  result |= expect_rejected({"--journal", journal_path, "--body-timeout-ms", "60001"},
                            "body-timeout above maximum");

  result |= expect_accepted({"--journal", journal_path, "--listen", "127.0.0.1:65535"},
                            "listen port maximum");
  result |= expect_accepted({"--journal", journal_path, "--cancel-principal", "x"},
                            "cancel-principal minimum");
  result |=
      expect_accepted({"--journal", journal_path, "--cancel-principal", std::string(256, 'x')},
                      "cancel-principal maximum");
  result |= expect_accepted({"--journal", journal_path, "--max-jobs", "1"}, "max-jobs minimum");
  result |= expect_accepted({"--journal", journal_path, "--max-jobs", "1024"}, "max-jobs maximum");
  result |= expect_accepted({"--journal", journal_path, "--target-limit", "2048"},
                            "target-limit minimum");
  result |= expect_accepted({"--journal", journal_path, "--target-limit", "65536"},
                            "target-limit maximum");
  result |= expect_accepted({"--journal", journal_path, "--header-limit", "8192"},
                            "header-limit minimum");
  result |= expect_accepted({"--journal", journal_path, "--header-limit", "262144"},
                            "header-limit maximum");
  result |=
      expect_accepted({"--journal", journal_path, "--body-limit", "4096"}, "body-limit minimum");
  result |=
      expect_accepted({"--journal", journal_path, "--body-limit", "1048576"}, "body-limit maximum");
  result |= expect_accepted({"--journal", journal_path, "--header-timeout-ms", "1"},
                            "header-timeout minimum");
  result |= expect_accepted({"--journal", journal_path, "--header-timeout-ms", "60000"},
                            "header-timeout maximum");
  result |= expect_accepted({"--journal", journal_path, "--body-timeout-ms", "1"},
                            "body-timeout minimum");
  result |= expect_accepted({"--journal", journal_path, "--body-timeout-ms", "60000"},
                            "body-timeout maximum");

  sitometron::daemon::StartupConfig config;
  config.journal_path = journal_path;
  config.max_jobs = 2;
  config.http.listen_port = 0;
  sitometron::daemon::Phase1Composition composition(std::move(config));
  std::string error;
  result |= Check(composition.Start(error), "rest_v1_sitometrond_options",
                  "composed daemon failed to start: " + error);
  if (error.empty() && composition.listen_port() != 0) {
    result |= ExpectStatus(SendToPort(composition.listen_port(), HealthRequest()),
                           "rest_v1_sitometrond_options", 200);
    result |= ExpectStatus(SendToPort(composition.listen_port(), HealthRequest("GET", "/v1/ready")),
                           "rest_v1_sitometrond_options", 200);
    result |= ExpectStatus(SendToPort(composition.listen_port(), HealthRequest("GET", "/v1/jobs")),
                           "rest_v1_sitometrond_options", 200);
  }
  composition.Stop();
  return result;
}

namespace job_routes {

namespace core = sitometron::core;
namespace internal = core::internal;
using Json = nlohmann::json;

constexpr std::string_view k_application = "app";
constexpr std::string_view k_principal = "configured-operator";
constexpr std::string_view k_unknown_id = "00000000-0000-7000-8000-000000000000";

Json ReadJsonFile(std::string_view relative) {
  std::ifstream input(std::filesystem::path(SITOMETRON_TEST_SOURCE_DIR) / relative);
  if (!input) throw std::runtime_error("test contract file could not be opened");
  return Json::parse(input);
}

core::Snapshot StateFixture(std::string_view state) {
  static const auto vectors = ReadJsonFile("schemas/core/v1/job-reducer-vectors.json");
  for (const auto& vector : vectors.at("case_vectors")) {
    const auto& snapshot = vector.at("initial_snapshot");
    if (snapshot.is_object() && snapshot.at("state") == std::string(state))
      return sitometron::test::SnapshotFrom(snapshot);
  }
  throw std::runtime_error("state fixture is missing");
}

std::string GeneratedId(std::size_t index) {
  constexpr std::array suffixes = {"f0", "01", "b0", "02", "a0", "03", "90", "04"};
  auto id = std::string(k_job_id);
  id.replace(id.size() - 2, 2, suffixes.at(index));
  return id;
}

std::string JobPath(std::string_view id = k_unknown_id, bool cancel = false) {
  auto path = std::string("/v1/jobs/");
  path.append(id);
  if (cancel) path.append("/cancel");
  return path;
}

std::string CreateBody(std::string_view id = k_application) {
  return Json{{"application_id", id}}.dump();
}

std::string Request(std::string_view method, std::string_view target,
                    std::optional<std::string> body = std::nullopt,
                    std::optional<std::string> media = "application/json",
                    std::string_view extra_headers = {}) {
  std::string request;
  request.append(method).append(" ").append(target).append(" HTTP/1.1\r\nHost: loopback\r\n");
  if (body) {
    if (media) request.append("Content-Type: ").append(*media).append("\r\n");
    request.append("Content-Length: ").append(std::to_string(body->size())).append("\r\n");
  }
  request.append(extra_headers).append("\r\n");
  if (body) request.append(*body);
  return request;
}

int RestError(const Response& response, std::string_view name, int status, std::string_view domain,
              const char* code) {
  int result = ExpectError(response, name, status, domain, code);
  const auto media = response.headers.find("content-type");
  result |= Check(media != response.headers.end() && media->second == "application/json", name,
                  "error Content-Type is not application/json");
  result |= Check(response.headers.find("location") == response.headers.end(), name,
                  "error response has Location");
  result |= Check(response.version == "HTTP/1.1", name, "error response version mismatch");
  try {
    const auto parsed = Json::parse(response.body);
    const auto& error = parsed.at("error");
    result |= Check(error.at("domain").is_string() && error.at("code").is_string() &&
                        error.at("message").is_string() && error.at("details") == Json::object(),
                    name, "error envelope field types or empty details mismatch");
  } catch (...) {
    result |= Check(false, name, "response is not a complete JSON error envelope");
  }
  return result;
}

int JobResponse(const Response& response, std::string_view name, int status,
                const core::Snapshot& snapshot) {
  int result = ExpectStatus(response, name, status);
  if (response.status != status) return result;
  const auto state = std::string(core::ToString(snapshot.state));
  const bool terminal = core::IsTerminalState(snapshot.state);
  try {
    const auto body = Json::parse(response.body);
    result |= Check(body.at("job_id") == snapshot.job_id.value && body.at("state") == state &&
                        body.at("terminal") == terminal &&
                        body.at("outcome") == (terminal ? Json(state) : Json(nullptr)),
                    name, "Job differs from the committed snapshot");
    result |= Check(response.headers.at("content-type") == "application/json", name,
                    "Job Content-Type mismatch");
  } catch (...) {
    result |= Check(false, name, "Job resource is missing or has wrong types");
  }
  return result;
}

// Metadata for the test-only registry, not an application runner or a daemon registration option.
struct TestRegistry {
  explicit TestRegistry(std::vector<std::string> values) : identifiers(std::move(values)) {
    for (const auto& id : identifiers) {
      const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      };
      if (id.empty() || id.size() > 128 || !alnum(static_cast<unsigned char>(id.front())) ||
          !std::ranges::all_of(id, [&](unsigned char c) {
            return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
          }))
        throw std::invalid_argument("invalid test Application identifier");
    }
  }
  std::vector<std::string> identifiers;
};

class Signal final {
 public:
  enum class Port : std::uint8_t { kNone, kIdentity, kCommit };
  void Hold(Port port) {
    std::lock_guard lock(mutex_);
    port_ = port;
    entered_ = false;
    released_ = false;
    responses_ = 0;
  }
  bool Enter(Port port) {
    std::unique_lock lock(mutex_);
    if (port != port_) return true;
    entered_ = true;
    cv_.notify_all();
    return cv_.wait_for(lock, k_network_timeout, [&] { return released_; });
  }
  void ResponseFinished() {
    std::lock_guard lock(mutex_);
    ++responses_;
    cv_.notify_all();
  }
  bool WaitForEntryOrResponse() {
    std::unique_lock lock(mutex_);
    const bool woke =
        cv_.wait_for(lock, k_network_timeout, [&] { return entered_ || responses_ != 0; });
    return woke && entered_ && responses_ == 0;
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    port_ = Port::kNone;
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  Port port_ = Port::kNone;
  bool entered_ = false;
  bool released_ = false;
  std::size_t responses_ = 0;
};

class Identity final : public core::IdentitySourcePort {
 public:
  explicit Identity(Signal& signal) : signal_(signal), fake_(Jobs(), Workers(), Launches()) {}
  core::JobSessionIdentityResult GenerateJobSessionIdentity() override {
    ++calls;
    const auto active = ++active_;
    auto maximum = max_active.load();
    while (maximum < active && !max_active.compare_exchange_weak(maximum, active)) {
    }
    if (!signal_.Enter(Signal::Port::kIdentity)) return core::IdentitySourceExhausted{};
    std::lock_guard lock(mutex_);
    return fake_.GenerateJobSessionIdentity();
  }
  core::WorkerIdentityResult GenerateWorkerIdentity() override {
    ++calls;
    std::lock_guard lock(mutex_);
    return fake_.GenerateWorkerIdentity();
  }
  core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    ++calls;
    --active_;
    std::lock_guard lock(mutex_);
    return fake_.GenerateLaunchOperationIdentity();
  }
  std::atomic_size_t calls{0};
  std::atomic_size_t max_active{0};

 private:
  static std::vector<core::JobSessionIdentityResult> Jobs() {
    std::vector<core::JobSessionIdentityResult> result;
    for (std::size_t index = 0; index < 8; ++index)
      result.emplace_back(core::GeneratedJobSessionIdentity{core::Uuid{GeneratedId(index)}});
    return result;
  }
  static std::vector<core::WorkerIdentityResult> Workers() {
    std::vector<core::WorkerIdentityResult> result;
    for (std::size_t index = 0; index < 8; ++index) {
      auto id = std::string("550e8400-e29b-41d4-a716-446655440000");
      id.back() = static_cast<char>('0' + index);
      result.emplace_back(core::GeneratedWorkerIdentity{core::Uuid{std::move(id)}});
    }
    return result;
  }
  static std::vector<core::LaunchOperationIdentityResult> Launches() {
    std::vector<core::LaunchOperationIdentityResult> result;
    for (std::size_t index = 0; index < 8; ++index)
      result.emplace_back(core::GeneratedLaunchOperationIdentity{
          core::StableId{std::string("launch-op-").append(std::to_string(index))}});
    return result;
  }
  Signal& signal_;
  std::mutex mutex_;
  sitometron::test::FakeIdentitySource fake_;
  std::atomic_size_t active_{0};
};

// A passive, gated port: records attempts, supplies a result, and never re-enters ingress.
class Journal final : public core::JobJournalPort {
 public:
  explicit Journal(Signal& signal) : signal_(signal) { attempts_.reserve(128); }
  core::LogicalCommitResult Commit(const core::LogicalJobEvent& event) noexcept override {
    try {
      if (!signal_.Enter(Signal::Port::kCommit)) return core::LogicalCommitResult::kDefiniteFailure;
      std::lock_guard lock(mutex_);
      attempts_.push_back(event);
      return result_;
    } catch (...) {
      return core::LogicalCommitResult::kDefiniteFailure;
    }
  }
  void Result(core::LogicalCommitResult result) {
    std::lock_guard lock(mutex_);
    result_ = result;
  }
  std::vector<core::LogicalJobEvent> Attempts() const {
    std::lock_guard lock(mutex_);
    return attempts_;
  }

 private:
  Signal& signal_;
  mutable std::mutex mutex_;
  std::vector<core::LogicalJobEvent> attempts_;
  core::LogicalCommitResult result_ = core::LogicalCommitResult::kCommitted;
};

class Effects final : public core::ApplicationRunnerPort, public core::SessionRetainerPort {
 public:
  void HandoffLaunch(core::ApplicationLaunchRequest&&) noexcept override { ++launches; }
  void HandoffCooperativeStop(core::ApplicationStopRequest&&) noexcept override { ++stops; }
  void HandoffForcedStop(core::ApplicationStopRequest&&) noexcept override { ++stops; }
  void HandoffRetainSameIdentity(core::SessionRetainRequest&&) noexcept override { ++retains; }
  std::atomic_size_t launches{0};
  std::atomic_size_t stops{0};
  std::atomic_size_t retains{0};
};

struct ScenarioOptions {
  std::vector<core::Snapshot> seeds;
  std::vector<std::string> applications{std::string(k_application)};
  std::size_t max_jobs = 4;
  bool unresolved = false;
};

class Scenario final {
 public:
  explicit Scenario(const ScenarioOptions& options = {})
      : identity(signal), journal(signal), registry(options.applications) {
    internal::Config config;
    config.max_jobs = options.max_jobs;
    config.normal_capacity = 2;
    config.trace_capacity = 1024;
    config.completion_capacity = 128;
    config.handoff_capacity = 64;
    config.ack_capacity = 128;
    config.callback_registration_capacity = 16;
    config.replayed_jobs = options.seeds;
    if (options.unresolved)
      for (const auto& snapshot : options.seeds) config.unresolved_jobs.push_back(snapshot.job_id);
    config.ports = {&clock, &journal, &effects, &effects, &identity};
    writer = std::make_unique<internal::JobOrchestrator>(config);
    backend = sitometron::daemon::MakeJobBackend(*writer, options.max_jobs, options.seeds,
                                                 registry.identifiers);
    http.listen_port = 0;
    http.cancel_principal = k_principal;
  }
  ~Scenario() { signal.Release(); }
  Scenario(const Scenario&) = delete;
  Scenario& operator=(const Scenario&) = delete;
  Response Exchange(const std::string& request) { return Send(*backend, http, request); }
  int NoSubmission(std::string_view name, std::size_t count = 0) const {
    return Check(writer->CopyIngressSequences().size() == count && journal.Attempts().empty() &&
                     identity.calls.load() == 0,
                 name, "validation or registry refusal reached identity/ingress/Journal");
  }
  Signal signal;
  sitometron::test::FakeClock clock{core::ClockReading{{"2026-10-23T00:00:00Z"}, {0}}};
  Identity identity;
  Journal journal;
  Effects effects;
  TestRegistry registry;
  std::unique_ptr<internal::JobOrchestrator> writer;
  std::unique_ptr<Backend> backend;
  Options http;
};

class PendingResponse final {
 public:
  PendingResponse(Signal& signal, std::function<Response()> operation)
      : signal_(signal), thread_([this, operation = std::move(operation)] {
          try {
            response_ = operation();
          } catch (...) {
            response_ = {};
          }
          signal_.ResponseFinished();
        }) {}
  ~PendingResponse() {
    signal_.Release();
    if (thread_.joinable()) thread_.join();
  }
  PendingResponse(const PendingResponse&) = delete;
  PendingResponse& operator=(const PendingResponse&) = delete;
  const Response& Finish() {
    if (thread_.joinable()) thread_.join();
    return response_;
  }

 private:
  Signal& signal_;
  Response response_;
  std::thread thread_;
};

class WriterQueueHold final {
 public:
  explicit WriterQueueHold(internal::JobOrchestrator& writer) : writer_(writer) {
    writer_.ArmBarrier(internal::WriterPhase::kBeforeDequeue);
    first_ = writer_.SubmitCommand(core::Command{1, core::CommandType::kCancel,
                                                 core::Uuid{std::string(k_unknown_id)}, "test"});
    if (first_.code != internal::IngressCode::kAdmitted ||
        !writer_.WaitUntil(first_.ingress_sequence, internal::WriterPhase::kBeforeDequeue))
      throw std::runtime_error("could not hold the direct core queue control");
    second_ = writer_.SubmitCommand(core::Command{1, core::CommandType::kCancel,
                                                  core::Uuid{std::string(k_unknown_id)}, "test"});
    if (second_.code != internal::IngressCode::kAdmitted)
      throw std::runtime_error("could not fill the direct core queue control");
  }
  ~WriterQueueHold() {
    (void)writer_.Release(first_.ingress_sequence, internal::WriterPhase::kBeforeDequeue);
    (void)writer_.AwaitCompletion(first_.ingress_sequence);
    (void)writer_.AwaitCompletion(second_.ingress_sequence);
  }
  WriterQueueHold(const WriterQueueHold&) = delete;
  WriterQueueHold& operator=(const WriterQueueHold&) = delete;

 private:
  internal::JobOrchestrator& writer_;
  internal::IngressResult first_;
  internal::IngressResult second_;
};

class LaterInputHold final {
 public:
  LaterInputHold(internal::JobOrchestrator& writer, Signal& signal)
      : writer_(writer), signal_(signal) {
    writer_.ArmBarrier(internal::WriterPhase::kBeforeDequeue);
    input_ = writer_.SubmitCommand(core::Command{1, core::CommandType::kCancel,
                                                 core::Uuid{std::string(k_unknown_id)}, "later"});
    if (input_.code != internal::IngressCode::kAdmitted)
      throw std::runtime_error("later direct input was not admitted");
  }
  ~LaterInputHold() {
    signal_.Release();
    (void)writer_.WaitUntil(input_.ingress_sequence, internal::WriterPhase::kBeforeDequeue);
    (void)writer_.Release(input_.ingress_sequence, internal::WriterPhase::kBeforeDequeue);
    (void)writer_.AwaitCompletion(input_.ingress_sequence);
  }
  LaterInputHold(const LaterInputHold&) = delete;
  LaterInputHold& operator=(const LaterInputHold&) = delete;

 private:
  internal::JobOrchestrator& writer_;
  Signal& signal_;
  internal::IngressResult input_;
};

int Created(Scenario& fixture, const Response& response, std::string_view name,
            std::size_t index = 0) {
  auto expected =
      core::InitialSnapshot(core::Uuid{GeneratedId(index)}, core::Uuid{GeneratedId(index)});
  expected.entity_exists = true;
  expected.state = core::JobState::kAdmitted;
  int result = JobResponse(response, name, 202, expected);
  const auto location = response.headers.find("location");
  result |=
      Check(location != response.headers.end() && location->second == JobPath(GeneratedId(index)),
            name, "create Location does not identify the returned committed Job");
  if (response.status == 202)
    result |= JobResponse(fixture.Exchange(Request("GET", JobPath(GeneratedId(index)))), name, 200,
                          expected);
  return result;
}

int CreateCommitsBefore202() {
  constexpr std::string_view name = "rest_v1_create_commits_before_202";
  Scenario fixture;
  fixture.signal.Hold(Signal::Port::kCommit);
  Server server(fixture.http, *fixture.backend);
  std::string error;
  if (!server.Start(error)) return Check(false, name, "listener did not start");
  PendingResponse pending(fixture.signal, [&] {
    return SendToPort(server.port(), Request("POST", "/v1/jobs", CreateBody()));
  });
  int result = Check(fixture.signal.WaitForEntryOrResponse(), name,
                     "expected writer commit gate before the final response");
  const auto before = fixture.writer->SnapshotFor(core::Uuid{GeneratedId(0)});
  result |= Check(!before || !before->entity_exists, name, "creation was visible before commit");
  fixture.signal.Release();
  result |= Created(fixture, pending.Finish(), name);
  result |= Check(fixture.journal.Attempts().size() == 1 && fixture.effects.launches.load() == 0,
                  name, "create did not commit exactly once without execution");
  for (const auto outcome :
       {core::LogicalCommitResult::kDefiniteFailure, core::LogicalCommitResult::kOutcomeUnknown}) {
    Scenario failed;
    failed.journal.Result(outcome);
    result |= RestError(failed.Exchange(Request("POST", "/v1/jobs", CreateBody())), name, 503,
                        "service", "service_failed");
    const auto snapshot = failed.writer->SnapshotFor(core::Uuid{GeneratedId(0)});
    result |= Check(!snapshot || !snapshot->entity_exists, name,
                    "failed commit published a speculative Job");
  }
  return result;
}

int CreateRejectsUnknownApplication() {
  constexpr std::string_view name = "rest_v1_create_rejects_unknown_application";
  Scenario fixture;
  const std::vector<std::string> unknown = {"unknown",
                                            "APP",
                                            ".app",
                                            "a/b",
                                            "a b",
                                            "\xc3\xa9",
                                            std::string(1, '\0'),
                                            std::string("app\0hidden", 10),
                                            "app ",
                                            std::string(128, 'z')};
  int result = 0;
  for (std::size_t index = 0; index < unknown.size(); ++index) {
    const auto label =
        std::string(name).append(" [unregistered-").append(std::to_string(index)).append("]");
    result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", CreateBody(unknown[index]))),
                        label, 422, "job", "unknown_application");
  }
  result |= fixture.NoSubmission(name);
  return result;
}

int CreateNeverWaits() {
  constexpr std::string_view name = "rest_v1_create_never_waits";
  int result = 0;
  for (const bool resident_full : {false, true}) {
    ScenarioOptions options;
    if (resident_full) {
      options.seeds = {StateFixture("admitted")};
      options.max_jobs = 1;
    }
    Scenario fixture(options);
    WriterQueueHold hold(*fixture.writer);
    const auto before = fixture.writer->CopyIngressSequences().size();
    const auto response = fixture.Exchange(Request("POST", "/v1/jobs", CreateBody()));
    result |=
        RestError(response, name, 503, "service", resident_full ? "capacity_exhausted" : "busy");
    result |= Check(fixture.writer->CopyIngressSequences().size() == before &&
                        fixture.journal.Attempts().empty(),
                    name, "capacity refusal admitted work or waited for the held queue to drain");
  }
  Scenario fixture;
  fixture.signal.Hold(Signal::Port::kCommit);
  Server server(fixture.http, *fixture.backend);
  std::string error;
  if (!server.Start(error)) return result | Check(false, name, "listener did not start");
  PendingResponse pending(fixture.signal, [&] {
    return SendToPort(server.port(), Request("POST", "/v1/jobs", CreateBody()));
  });
  const bool reached_commit = fixture.signal.WaitForEntryOrResponse();
  result |= Check(reached_commit, name, "create did not reach its own controlled commit");
  if (reached_commit) {
    LaterInputHold later(*fixture.writer, fixture.signal);
    fixture.signal.Release();
    // The later writer turn remains held until after this response and read are checked. This
    // tests per-sequence completion, not the listener's pending I/O-thread design disposition.
    result |= Created(fixture, pending.Finish(), name);
  } else {
    fixture.signal.Release();
    result |= Created(fixture, pending.Finish(), name);
  }
  return result;
}

int JobResourceFromCommittedState() {
  constexpr std::string_view name = "rest_v1_job_resource_from_committed_state";
  int result = 0;
  for (const auto state : {"admitted", "preparing", "running", "stopping", "finalizing",
                           "succeeded", "failed", "cancelled", "terminated", "timed_out"}) {
    const auto snapshot = StateFixture(state);
    ScenarioOptions options;
    options.seeds = {snapshot};
    Scenario fixture(options);
    result |= JobResponse(fixture.Exchange(Request("GET", JobPath(snapshot.job_id.value))),
                          std::string(name).append(" [").append(state).append("]"), 200, snapshot);
    result |= Check(fixture.journal.Attempts().empty(), name, "read appended a Journal record");
  }
  Scenario fixture;
  fixture.signal.Hold(Signal::Port::kCommit);
  const auto created = fixture.writer->CreateJob();
  int control = Check(created.ingress.code == internal::IngressCode::kAdmitted &&
                          fixture.signal.WaitForEntryOrResponse(),
                      name, "direct creation control failed");
  result |= control;
  if (control == 0) {
    result |= RestError(fixture.Exchange(Request("GET", JobPath(GeneratedId(0)))), name, 404, "job",
                        "job_not_found");
    const auto list = fixture.Exchange(Request("GET", "/v1/jobs"));
    result |= ExpectStatus(list, name, 200);
    if (list.status == 200)
      result |= Check(Json::parse(list.body).at("jobs").empty(), name,
                      "provisional Job appeared in list");
  }
  fixture.signal.Release();
  if (created.ingress.code != internal::IngressCode::kAdmitted) return result;
  const auto done = fixture.writer->AwaitCompletion(created.ingress.ingress_sequence);
  result |= Check(done && done->code == internal::Completion::Code::kSuccess, name,
                  "direct creation did not commit");
  const auto committed = fixture.writer->SnapshotFor(core::Uuid{GeneratedId(0)});
  if (committed)
    result |= JobResponse(fixture.Exchange(Request("GET", JobPath(GeneratedId(0)))), name, 200,
                          *committed);
  fixture.signal.Hold(Signal::Port::kCommit);
  const auto cancel = fixture.writer->SubmitCommand(
      core::Command{1, core::CommandType::kCancel, core::Uuid{GeneratedId(0)}, "direct-control"});
  if (cancel.code == internal::IngressCode::kAdmitted && fixture.signal.WaitForEntryOrResponse()) {
    if (committed)
      result |= JobResponse(fixture.Exchange(Request("GET", JobPath(GeneratedId(0)))), name, 200,
                            *committed);
  } else {
    result |= Check(false, name, "direct cancel control did not reach commit");
  }
  fixture.signal.Release();
  if (cancel.code == internal::IngressCode::kAdmitted)
    (void)fixture.writer->AwaitCompletion(cancel.ingress_sequence);
  const auto after = fixture.writer->SnapshotFor(core::Uuid{GeneratedId(0)});
  if (after)
    result |=
        JobResponse(fixture.Exchange(Request("GET", JobPath(GeneratedId(0)))), name, 200, *after);
  return result;
}

int ListResidentInCreationOrder() {
  constexpr std::string_view name = "rest_v1_list_resident_in_creation_order";
  ScenarioOptions options;
  auto first = StateFixture("cancelled");
  auto second = StateFixture("admitted");
  first.job_id.value = GeneratedId(6);
  first.session_id = first.job_id;
  second.job_id.value = GeneratedId(7);
  second.session_id = second.job_id;
  options.seeds = {first, second};
  Scenario fixture(options);
  Scenario empty;
  auto response = empty.Exchange(Request("GET", "/v1/jobs"));
  int result = ExpectStatus(response, name, 200);
  if (response.status == 200)
    result |= Check(Json::parse(response.body).at("jobs").empty(), name, "fresh list is not empty");
  for (std::size_t index = 0; index < 2; ++index)
    result |=
        Created(fixture, fixture.Exchange(Request("POST", "/v1/jobs", CreateBody())), name, index);
  const auto cancelled = fixture.Exchange(Request("POST", JobPath(GeneratedId(0), true)));
  result |= ExpectStatus(cancelled, name, 202);
  response = fixture.Exchange(Request("GET", "/v1/jobs"));
  result |= ExpectStatus(response, name, 200);
  if (response.status == 200) {
    const auto jobs = Json::parse(response.body).at("jobs");
    const std::vector<std::string> order = {first.job_id.value, second.job_id.value, GeneratedId(0),
                                            GeneratedId(1)};
    result |= Check(jobs.size() == order.size(), name, "resident list omitted or added a Job");
    for (std::size_t index = 0; index < std::min(jobs.size(), order.size()); ++index) {
      result |=
          Check(jobs.at(index).at("job_id") == order[index], name, "list is not in creation order");
      const auto current = fixture.writer->SnapshotFor(core::Uuid{order[index]});
      result |= Check(
          current && jobs.at(index).at("state") == std::string(core::ToString(current->state)),
          name, "list used a stale or speculative state");
    }
  }
  ScenarioOptions collision_options;
  collision_options.max_jobs = 2;
  auto replayed = StateFixture("cancelled");
  replayed.job_id.value = GeneratedId(0);
  replayed.session_id = replayed.job_id;
  collision_options.seeds = {replayed};
  const auto collision_name = std::string(name).append(" [replayed-id-collision]");
  {
    // A new source instance may issue an ID from an earlier instance's replayed history.
    Scenario control(collision_options);
    const auto duplicate = control.writer->CreateJob();
    const auto completion = control.writer->AwaitCompletion(duplicate.ingress.ingress_sequence);
    result |= Check(duplicate.ingress.code == internal::IngressCode::kAdmitted && completion &&
                        completion->code == internal::Completion::Code::kReducerRejection &&
                        completion->rejection &&
                        completion->rejection->reason == core::RejectionReason::kJobAlreadyExists &&
                        control.journal.Attempts().empty() && control.writer->resident_count() == 1,
                    collision_name, "core did not reject the replay collision without a commit");
  }
  Scenario collision(collision_options);
  result |= RestError(collision.Exchange(Request("POST", "/v1/jobs", CreateBody())), collision_name,
                      500, "service", "internal");
  result |= Check(collision.writer->CopyIngressSequences().size() == 1 &&
                      collision.writer->writer_turn_count() == 1 &&
                      collision.writer->resident_count() == 1 &&
                      collision.journal.Attempts().empty() && !collision.writer->failed(),
                  collision_name, "rejected creation changed the resident or Journal population");
  response = collision.Exchange(Request("GET", "/v1/jobs"));
  result |= ExpectStatus(response, collision_name, 200);
  if (response.status == 200) {
    const auto jobs = Json::parse(response.body).at("jobs");
    result |= Check(jobs.size() == 1 && jobs.at(0).at("job_id") == replayed.job_id.value,
                    collision_name, "rejected creation duplicated the replayed resident");
  }
  result |= Created(collision, collision.Exchange(Request("POST", "/v1/jobs", CreateBody())),
                    collision_name, 1);
  (void)collision.writer->WaitForWriterIdle();
  response = collision.Exchange(Request("GET", "/v1/jobs"));
  result |= ExpectStatus(response, collision_name, 200);
  if (response.status == 200) {
    const auto jobs = Json::parse(response.body).at("jobs");
    result |= Check(jobs.size() == 2 && jobs.at(0).at("job_id") == replayed.job_id.value &&
                        jobs.at(1).at("job_id") == GeneratedId(1),
                    collision_name, "rejected creation consumed the following valid index slot");
  }
  result |=
      Check(collision.writer->resident_count() == 2 && collision.journal.Attempts().size() == 1,
            collision_name, "following valid creation did not use the remaining resident slot");
  return result;
}

int CancelStatusMapping() {
  constexpr std::string_view name = "rest_v1_cancel_status_mapping";
  int result = 0;
  Scenario absent;
  const auto before = absent.writer->writer_turn_count();
  const auto started = std::chrono::steady_clock::now();
  const auto unknown = absent.Exchange(Request("POST", JobPath(k_unknown_id, true)));
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  result |= RestError(unknown, name, 404, "job", "job_not_found");
  result |=
      Check(absent.writer->writer_turn_count() == before + 1 && absent.journal.Attempts().empty(),
            name, "unknown cancel did not use exactly one writer rejection turn");
  std::cout << name << ": adapter-fixture unknown cancel elapsed_us=" << elapsed
            << " writer_turns=" << absent.writer->writer_turn_count() - before << '\n';
  {
    // Informational Q2 control: measure the existing production core API directly as well as the
    // HTTP path. No duration is asserted; RED's interim HTTP response is not a writer-turn sample.
    Scenario control;
    const auto submit_started = std::chrono::steady_clock::now();
    const auto input = control.writer->SubmitCommand(
        core::Command{1, core::CommandType::kCancel, core::Uuid{std::string(k_unknown_id)},
                      std::string(k_principal)});
    const auto await_started = std::chrono::steady_clock::now();
    const auto completion = control.writer->AwaitCompletion(input.ingress_sequence);
    const auto finished = std::chrono::steady_clock::now();
    result |=
        Check(input.code == internal::IngressCode::kAdmitted && completion &&
                  completion->code == internal::Completion::Code::kReducerRejection &&
                  completion->rejection &&
                  completion->rejection->reason == core::RejectionReason::kJobNotFound &&
                  control.writer->writer_turn_count() == 1 && control.journal.Attempts().empty(),
              name, "direct unknown-cancel control did not reject in one writer turn");
    std::cout
        << name << ": direct-core unknown cancel elapsed_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(finished - submit_started).count()
        << " await_only_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(finished - await_started).count()
        << " writer_turns=" << control.writer->writer_turn_count() << '\n';
  }
  for (const auto state : {"admitted", "preparing", "running", "stopping", "finalizing",
                           "succeeded", "failed", "cancelled", "terminated", "timed_out"}) {
    const auto snapshot = StateFixture(state);
    ScenarioOptions options;
    options.seeds = {snapshot};
    Scenario fixture(options);
    const auto path = JobPath(snapshot.job_id.value, true);
    const auto label = std::string(name).append(" [").append(state).append("]");
    const bool accepted = snapshot.state == core::JobState::kAdmitted ||
                          snapshot.state == core::JobState::kPreparing ||
                          snapshot.state == core::JobState::kRunning;
    const auto response = fixture.Exchange(Request("POST", path));
    if (accepted) {
      const auto decision = core::DecideCommand(
          snapshot,
          core::Command{1, core::CommandType::kCancel, snapshot.job_id, std::string(k_principal)});
      const auto& proposal = std::get<core::PreEnvelopeProposal>(decision.value);
      const auto expected = core::Apply(snapshot, proposal).snapshot;
      result |= JobResponse(response, label, 202, expected);
      result |= Check(response.headers.find("location") == response.headers.end(), label,
                      "cancel response has create Location");
      const auto repeat = fixture.Exchange(Request("POST", path));
      result |=
          RestError(repeat, label, 409, "job",
                    expected.state == core::JobState::kStopping ? "stop_cause_already_latched"
                                                                : "command_not_allowed_in_state");
      result |= Check(fixture.journal.Attempts().size() == 1, label,
                      "repeat cancel did not retain exactly one accepted-cancel record");
    } else {
      result |=
          RestError(response, label, 409, "job",
                    snapshot.state == core::JobState::kStopping ? "stop_cause_already_latched"
                                                                : "command_not_allowed_in_state");
      result |=
          Check(fixture.journal.Attempts().empty(), label, "rejected cancel appended a record");
    }
  }
  Scenario full;
  WriterQueueHold hold(*full.writer);
  result |= RestError(full.Exchange(Request("POST", JobPath(k_unknown_id, true))), name, 503,
                      "service", "busy");
  return result;
}

int CancelRecordsConfiguredPrincipal() {
  constexpr std::string_view name = "rest_v1_cancel_records_configured_principal";
  ScenarioOptions options;
  options.seeds = {StateFixture("admitted")};
  Scenario fixture(options);
  fixture.signal.Hold(Signal::Port::kCommit);
  Server server(fixture.http, *fixture.backend);
  std::string error;
  if (!server.Start(error)) return Check(false, name, "listener did not start");
  PendingResponse pending(fixture.signal, [&] {
    return SendToPort(
        server.port(),
        Request(
            "POST", JobPath(options.seeds.front().job_id.value, true), "{}", "application/json",
            "X-Principal: forged\r\nX-Authenticated-User: forged\r\nAuthorization: forged\r\n"));
  });
  int result = Check(fixture.signal.WaitForEntryOrResponse(), name,
                     "cancel answered before its controlled commit");
  const auto before = fixture.writer->SnapshotFor(options.seeds.front().job_id);
  result |= Check(before && before->state == options.seeds.front().state, name,
                  "cancel changed the published state before commit");
  fixture.signal.Release();
  const auto& response = pending.Finish();
  result |= ExpectStatus(response, name, 202);
  const auto attempts = fixture.journal.Attempts();
  result |= Check(attempts.size() == 1, name, "configured cancel did not append exactly once");
  if (!attempts.empty()) {
    const auto* principal = std::get_if<core::PrincipalPayload>(&attempts.front().payload);
    result |= Check(attempts.front().event_type == core::EventType::kCancelAccepted && principal &&
                        principal->principal_subject == k_principal,
                    name, "recorded principal was not the startup-configured principal");
  }
  return result;
}

struct BodyVector {
  std::string name;
  std::string body;
  int status = 400;
  const char* code = "malformed_json";
};

int BodyVectors(std::string_view name, const std::vector<BodyVector>& vectors) {
  Scenario fixture;
  const std::array endpoints = {
      std::pair{"POST", std::string("/v1/jobs")},  std::pair{"POST", JobPath(k_unknown_id, true)},
      std::pair{"GET", std::string("/v1/jobs")},   std::pair{"GET", JobPath()},
      std::pair{"GET", std::string("/v1/health")}, std::pair{"GET", std::string("/v1/ready")}};
  int result = 0;
  std::size_t passed = 0;
  for (const auto& vector : vectors) {
    for (const auto& [method, target] : endpoints) {
      const auto label = std::string(name)
                             .append(" [")
                             .append(vector.name)
                             .append(" ")
                             .append(method)
                             .append(" ")
                             .append(target)
                             .append("]");
      const bool create = std::string_view(method) == "POST" && target == "/v1/jobs";
      const bool unknown_application =
          create && std::string_view(vector.code) == "unknown_application";
      const int status = unknown_application ? 422 : vector.status;
      const char* code = !create && std::string_view(vector.code) == "unknown_application"
                             ? "validation_failed"
                             : vector.code;
      const auto check = RestError(fixture.Exchange(Request(method, target, vector.body)), label,
                                   status, unknown_application ? "job" : "request", code);
      if (check == 0) ++passed;
      result |= check;
    }
  }
  result |= fixture.NoSubmission(name);
  std::cout << name << ": vector outcomes passed=" << passed << '/'
            << vectors.size() * endpoints.size() << '\n';
  return result;
}

int JsonRejectsRawNul() {
  const auto nul = std::string(1, '\0');
  return BodyVectors(
      "rest_v1_json_rejects_raw_nul",
      {{"prefix", nul + "{}"},
       {"string", std::string("{\"application_id\":\"") + nul + "\"}"},
       {"key", std::string("{\"") + nul + "\":1}"},
       {"token", std::string("{\"x\":") + nul + "1}"},
       {"suffix", "{}" + nul},
       {"suffix-garbage", "{}" + nul + "garbage"},
       {"suffix-second-value", "{}" + nul + "{}"},
       {"after-whitespace", "{} \n" + nul + "x"},
       {"escaped-control", R"({"application_id":"\u0000"})", 422, "unknown_application"}});
}

int JsonRejectsInvalidUtf8() {
  std::vector<BodyVector> vectors;
  const std::vector<std::string> invalid = {"\xff",     "\xc0\xaf", "\xed\xa0\x80",
                                            "\xe2\x82", "\x80",     "\xf4\x90\x80\x80"};
  for (std::size_t index = 0; index < invalid.size(); ++index)
    vectors.push_back({std::string("invalid-").append(std::to_string(index)),
                       std::string("{\"application_id\":\"") + invalid[index] + "\"}"});
  vectors.push_back({"non-json-unicode-whitespace", "{}\xc2\xa0"});
  for (const auto valid : {"\xc3\xa9", "\xe3\x81\x82", "\xf0\x9f\x98\x80"})
    vectors.push_back({"valid-utf8", CreateBody(valid), 422, "unknown_application"});
  return BodyVectors("rest_v1_json_rejects_invalid_utf8", vectors);
}

int JsonRejectsTrailingData() {
  std::vector<BodyVector> vectors;
  for (const auto suffix : {"{}", "1", "x", "/*comment*/", "\v", "\f", "\xc2\xa0"})
    vectors.push_back({"non-whitespace-suffix", std::string("{}") + suffix});
  for (const auto whitespace : {" ", "\t", "\r", "\n", " \t\r\n"})
    vectors.push_back({"json-whitespace-control",
                       std::string(whitespace) + "{\"x\":1}" + whitespace, 422,
                       "validation_failed"});
  return BodyVectors("rest_v1_json_rejects_trailing_data", vectors);
}

int JsonRejectsDuplicateKeysAtAnyDepth() {
  return BodyVectors(
      "rest_v1_json_rejects_duplicate_keys_at_any_depth",
      {{"root-unequal", R"({"application_id":"app","application_id":"unknown"})"},
       {"root-equal", R"({"application_id":"app","application_id":"app"})"},
       {"nested", R"({"outer":{"a":1,"a":2}})"},
       {"array", R"([{"a":1,"a":2}])"},
       {"decoded-key", R"({"a":1,"\u0061":2})"},
       {"empty-key", R"({"":1,"":2})"},
       {"unicode-key", std::string("{\"\\u00e9\":1,\"") + "\xc3\xa9" + "\":2}"},
       {"distinct-normalization-control", R"({"\u00e9":1,"e\u0301":2})", 422, "validation_failed"},
       {"siblings-control", R"({"x":{"a":1},"y":{"a":2}})", 422, "validation_failed"},
       {"depth-control", R"({"a":{"a":1}})", 422, "validation_failed"},
       {"array-siblings-control", R"([{"a":1},{"a":2}])", 422, "validation_failed"}});
}

int JsonRejectsLeadingBom() {
  const std::string bom = "\xef\xbb\xbf";
  return BodyVectors("rest_v1_json_rejects_leading_bom",
                     {{"leading", bom + "{}"},
                      {"only", bom},
                      {"after-space", " " + bom + "{}"},
                      {"repeated", bom + bom + "{}"},
                      {"inside-string-control", CreateBody(bom), 422, "unknown_application"}});
}

int JsonRejectsLoneSurrogate() {
  return BodyVectors(
      "rest_v1_json_rejects_lone_surrogate",
      {{"high", R"({"application_id":"\ud800"})"},
       {"low", R"({"application_id":"\udc00"})"},
       {"high-text", R"({"application_id":"\ud800x"})"},
       {"reversed", R"({"application_id":"\udc00\ud800"})"},
       {"pair-control", R"({"application_id":"\ud83d\ude00"})", 422, "unknown_application"},
       {"literal-control", R"({"application_id":"\\ud800"})", 422, "unknown_application"}});
}

int ValidationBeforeSubmission() {
  constexpr std::string_view name = "rest_v1_validation_before_submission";
  int result = BodyVectors(
      name,
      {{"truncated", "{"},
       {"trailing-comma", "{\"x\":1,}"},
       {"comment", "/*comment*/{}"},
       {"leading-zero", "01"},
       {"nan", "NaN"},
       {"library-number-overflow-406", "1e1000"},
       {"array", "[]", 422, "validation_failed"},
       {"null", "null", 422, "validation_failed"},
       {"boolean", "true", 422, "validation_failed"},
       {"number", "1", 422, "validation_failed"},
       {"string", "\"x\"", 422, "validation_failed"},
       {"wrong-type", R"({"application_id":1})", 422, "validation_failed"},
       {"extra-job", R"({"application_id":"app","job_id":"client"})", 422, "validation_failed"},
       {"extra-path", R"({"application_id":"app","executable":"client"})", 422,
        "validation_failed"},
       {"extra-args", R"({"application_id":"app","args":[]})", 422, "validation_failed"},
       {"extra-principal", R"({"application_id":"app","principal":"forged"})", 422,
        "validation_failed"},
       {"unknown-with-extra", R"({"application_id":"unknown","extra":1})", 422,
        "validation_failed"},
       {"empty-id", CreateBody(""), 422, "validation_failed"},
       {"long-id", CreateBody(std::string(129, 'a')), 422, "validation_failed"}});
  Scenario fixture;
  result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", "{}")), name, 422, "request",
                      "validation_failed");
  std::vector<std::string> invalid = {"short",
                                      std::string(k_job_id) + "0",
                                      "%7B" + std::string(k_job_id) + "%7D",
                                      "urn:uuid:" + std::string(k_job_id),
                                      "01890F30-7b54-7cc3-98c4-dc0c0c07398f",
                                      "01890f30_7b54-7cc3-98c4-dc0c0c07398f",
                                      "01890f30-7b54-4cc3-98c4-dc0c0c07398f",
                                      "01890f30-7b54-7cc3-78c4-dc0c0c07398f",
                                      "01890f30-7b54-7cc3-c8c4-dc0c0c07398f",
                                      "01890f30-7b54-7cc3-g8c4-dc0c0c07398f",
                                      "01890f30%2d7b54-7cc3-98c4-dc0c0c07398f"};
  for (const auto& id : invalid) {
    for (const bool cancel : {false, true})
      result |= RestError(fixture.Exchange(Request(cancel ? "POST" : "GET", JobPath(id, cancel))),
                          name, 400, "request", "invalid_job_id");
  }
  for (const char variant : {'8', '9', 'a', 'b'}) {
    auto id = std::string(k_unknown_id);
    id[19] = variant;
    result |=
        RestError(fixture.Exchange(Request("GET", JobPath(id))), name, 404, "job", "job_not_found");
  }
  result |= RestError(fixture.Exchange(Request("POST", JobPath("invalid", true), "{")), name, 400,
                      "request", "malformed_json");
  result |=
      RestError(fixture.Exchange(Request("POST", JobPath("invalid", true), "{", "text/plain")),
                name, 415, "request", "unsupported_media_type");
  result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs?x=1", "{", "text/plain")), name,
                      404, "request", "route_not_found");
  result |= RestError(fixture.Exchange(Request("PUT", "/v1/jobs", "{", "text/plain")), name, 405,
                      "request", "method_not_allowed");
  result |= RestError(fixture.Exchange(Request("GET", "/v1/jobs/")), name, 404, "request",
                      "route_not_found");
  result |= RestError(fixture.Exchange(Request("POST", JobPath(k_unknown_id, true) + "/extra")),
                      name, 404, "request", "route_not_found");
  result |= fixture.NoSubmission(name);
  return result;
}

int CreateWithoutBody() {
  constexpr std::string_view name = "rest_v1_create_without_body";
  Scenario fixture;
  int result = 0;
  for (const auto& media :
       {std::optional<std::string>{}, std::optional<std::string>{"application/json"},
        std::optional<std::string>{"text/plain"}}) {
    result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", "", media)), name, 400,
                        "request", "malformed_json");
    const auto header = media ? std::string("Content-Type: ").append(*media).append("\r\n") : "";
    result |=
        RestError(fixture.Exchange(Request("POST", "/v1/jobs", std::nullopt, std::nullopt, header)),
                  name, 400, "request", "malformed_json");
  }
  result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", " \t\r\n")), name, 400,
                      "request", "malformed_json");
  result |= RestError(
      fixture.Exchange(
          "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\n"
          "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
      name, 400, "request", "malformed_json");
  result |= fixture.NoSubmission(name);
  std::string spaced_body(" \t\r\n");
  spaced_body.append(CreateBody()).append(" \t\r\n");
  const std::array<std::string, 3> valid_bodies = {std::move(spaced_body),
                                                   R"({"\u0061pplication_id":"app"})",
                                                   R"({"application_id":"\u0061pp"})"};
  for (std::size_t index = 0; index < valid_bodies.size(); ++index)
    result |= Created(fixture,
                      fixture.Exchange(Request("POST", "/v1/jobs", valid_bodies[index],
                                               "Application/JSON; charset=utf-8")),
                      name, index);
  return result;
}

int RegisteredApplicationIdForm() {
  constexpr std::string_view name = "rest_v1_registered_application_id_form";
  const std::vector<std::string> valid = {"a", "0", "a0z9", "a._:-", std::string(128, 'a')};
  int result = 0;
  std::size_t setup_passed = 0;
  for (const auto& id : valid) {
    ScenarioOptions options;
    options.applications = {id};
    Scenario fixture(options);
    ++setup_passed;
    result |= Created(fixture, fixture.Exchange(Request("POST", "/v1/jobs", CreateBody(id))), name);
  }
  const std::vector<std::string> invalid = {
      "",    std::string(129, 'a'), ".a",      "_a", ":a", "-a", "A", "a/b", "a b",
      "a\n", std::string(1, '\0'),  "\xc3\xa9"};
  for (const auto& id : invalid) {
    bool rejected = false;
    try {
      const TestRegistry registry({id});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    if (rejected) ++setup_passed;
    result |= Check(rejected, name, "test registry accepted an invalid identifier");
  }
  Scenario fixture;
  for (const auto character : {"\xc3\xa9", "\xf0\x9f\x98\x80"}) {
    for (const std::size_t count : {std::size_t{1}, std::size_t{128}, std::size_t{129}}) {
      std::string unicode;
      for (std::size_t index = 0; index < count; ++index) unicode.append(character);
      result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", CreateBody(unicode))), name,
                          422, count <= 128 ? "job" : "request",
                          count <= 128 ? "unknown_application" : "validation_failed");
    }
  }
  result |= fixture.NoSubmission(name);
  std::cout << name << ": test-only registry setup controls passed=" << setup_passed << '/'
            << valid.size() + invalid.size() << '\n';
  return result;
}

int CancelBody() {
  constexpr std::string_view name = "rest_v1_cancel_body";
  int result = 0;
  for (const auto& body :
       {std::optional<std::string>{}, std::optional<std::string>{""},
        std::optional<std::string>{"{}"}, std::optional<std::string>{" \t{ }\r\n"}}) {
    ScenarioOptions options;
    options.seeds = {StateFixture("admitted")};
    Scenario fixture(options);
    result |= ExpectStatus(
        fixture.Exchange(Request("POST", JobPath(options.seeds[0].job_id.value, true), body)), name,
        202);
  }
  Scenario fixture;
  for (const auto& body :
       {"{}", "{", "[]", "null", " ", "{\"reason\":\"x\"}", "{\"principal\":\"forged\"}"}) {
    for (const auto& media :
         {std::optional<std::string>{}, std::optional<std::string>{"text/plain"},
          std::optional<std::string>{"application/json"}}) {
      const bool malformed = std::string_view(body) == "{" || std::string_view(body) == " ";
      const bool accepted = std::string_view(body) == "{}";
      const int status = !media || *media != "application/json" ? 415
                         : malformed                            ? 400
                         : accepted                             ? 404
                                                                : 422;
      const char* code = status == 415   ? "unsupported_media_type"
                         : status == 400 ? "malformed_json"
                         : status == 404 ? "job_not_found"
                                         : "validation_failed";
      result |=
          RestError(fixture.Exchange(Request("POST", JobPath(k_unknown_id, true), body, media)),
                    name, status, status == 404 ? "job" : "request", code);
    }
  }
  for (const auto media : {"text/plain", "application/json"})
    result |= RestError(fixture.Exchange(Request("POST", JobPath(k_unknown_id, true), "", media)),
                        name, 404, "job", "job_not_found");
  const auto header = std::string("POST ")
                          .append(JobPath(k_unknown_id, true))
                          .append(
                              " HTTP/1.1\r\nHost: loopback\r\nContent-Type: "
                              "application/json\r\nTransfer-Encoding: chunked\r\n\r\n");
  result |= RestError(SendChunkedParts(*fixture.backend, fixture.http, header,
                                       {"1\r\n{\r\n", "1\r\n}\r\n0\r\n\r\n"}),
                      name, 404, "job", "job_not_found");
  // ParseHeader treats an explicit zero Content-Length as no body, unlike chunked framing.
  for (const auto& media : {std::optional<std::string>{}, std::optional<std::string>{"text/plain"},
                            std::optional<std::string>{"application/json"}}) {
    const auto media_header =
        media ? std::string("Content-Type: ").append(*media).append("\r\n") : std::string{};
    const auto empty_chunked =
        Request("POST", JobPath(k_unknown_id, true), std::nullopt, std::nullopt,
                media_header + "Transfer-Encoding: chunked\r\n") +
        "0\r\n\r\n";
    const auto label = std::string(name)
                           .append(" [empty-chunked, ")
                           .append(media.value_or("no media"))
                           .append("]");
    Scenario empty_body;
    const bool json = media && *media == "application/json";
    result |= RestError(empty_body.Exchange(empty_chunked), label, json ? 400 : 415, "request",
                        json ? "malformed_json" : "unsupported_media_type");
    result |= empty_body.NoSubmission(label);
    result |=
        RestError(empty_body.Exchange(Request("POST", JobPath(k_unknown_id, true), "", media)),
                  std::string(name).append(" [Content-Length: 0]"), 404, "job", "job_not_found");
  }
  return result;
}

int NotReadyRefusesWrites() {
  constexpr std::string_view name = "rest_v1_not_ready_refuses_writes";
  int result = 0;
  std::size_t controls = 0;
  for (const auto mode : {"unresolved", "shutdown", "failed"}) {
    ScenarioOptions options;
    options.seeds = {StateFixture("admitted")};
    options.unresolved = std::string_view(mode) == "unresolved";
    Scenario fixture(options);
    if (std::string_view(mode) == "shutdown") {
      const auto marker = fixture.writer->SubmitShutdown();
      (void)fixture.writer->AwaitCompletion(marker.ingress_sequence);
    } else if (std::string_view(mode) == "failed") {
      fixture.writer->LatchReadinessFailure();
    }
    const auto before = fixture.writer->CopyIngressSequences().size();
    const auto label = std::string(name).append(" [").append(mode).append("]");
    result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", CreateBody("not-registered"))),
                        label + " unregistered-before-ingress", 422, "job", "unknown_application");
    const char* code = std::string_view(mode) == "failed" ? "service_failed" : "not_ready";
    const auto registered = RestError(fixture.Exchange(Request("POST", "/v1/jobs", CreateBody())),
                                      label + " registered-to-ingress", 503, "service", code);
    if (registered == 0) ++controls;
    result |= registered;
    for (const auto& id : {options.seeds[0].job_id.value, std::string(k_unknown_id)})
      result |= RestError(fixture.Exchange(Request("POST", JobPath(id, true))), label, 503,
                          "service", code);
    result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", "{")), label, 400, "request",
                        "malformed_json");
    result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", "{}")), label, 422, "request",
                        "validation_failed");
    result |= JobResponse(fixture.Exchange(Request("GET", JobPath(options.seeds[0].job_id.value))),
                          label, 200, options.seeds[0]);
    result |= ExpectStatus(fixture.Exchange(Request("GET", "/v1/jobs")), label, 200);
    result |= ExpectStatus(fixture.Exchange(Request("GET", "/v1/ready")), label, 503);
    result |= fixture.NoSubmission(label, before);
  }
  // The sticky failure result must win even when unresolved replay also closes admission.
  ScenarioOptions options;
  options.seeds = {StateFixture("admitted")};
  options.unresolved = true;
  Scenario failed_closed(options);
  failed_closed.writer->LatchReadinessFailure();
  result |= RestError(failed_closed.Exchange(Request("POST", "/v1/jobs", CreateBody())), name, 503,
                      "service", "service_failed");
  std::cout << name << ": registered closed-admission controls passed=" << controls << "/3\n";
  return result;
}

int CreateSerialized() {
  constexpr std::string_view name = "rest_v1_create_serialized";
  Scenario fixture;
  fixture.signal.Hold(Signal::Port::kIdentity);
  Server server(fixture.http, *fixture.backend);
  std::string error;
  if (!server.Start(error)) return Check(false, name, "listener did not start");
  PendingResponse first(fixture.signal, [&] {
    return SendToPort(server.port(), Request("POST", "/v1/jobs", CreateBody()));
  });
  int result = Check(fixture.signal.WaitForEntryOrResponse(), name,
                     "first create did not reach the controlled identity source");
  PendingResponse second(fixture.signal, [&] {
    return SendToPort(server.port(), Request("POST", "/v1/jobs", CreateBody()));
  });
  fixture.signal.Release();
  const auto& one = first.Finish();
  const auto& two = second.Finish();
  result |= Created(fixture, one, name, 0);
  result |= Created(fixture, two, name, 1);
  result |= Check(fixture.identity.max_active.load() == 1 && fixture.journal.Attempts().size() == 2,
                  name, "creates overlapped identity generation or lost their ordered commits");
  return result;
}

int AlreadyPendingIsBusy() {
  constexpr std::string_view name = "rest_v1_already_pending_is_busy";
  Scenario fixture;
  fixture.signal.Hold(Signal::Port::kIdentity);
  PendingResponse outside(fixture.signal, [&] {
    const auto created = fixture.writer->CreateJob();
    if (created.ingress.code == internal::IngressCode::kAdmitted)
      (void)fixture.writer->AwaitCompletion(created.ingress.ingress_sequence);
    return Response{};
  });
  int result = Check(fixture.signal.WaitForEntryOrResponse(), name,
                     "external creation did not hold identity generation");
  result |= RestError(fixture.Exchange(Request("POST", "/v1/jobs", CreateBody())), name, 503,
                      "service", "busy");
  result |=
      Check(fixture.writer->CopyIngressSequences().empty() && fixture.journal.Attempts().empty(),
            name, "already_pending acquired a sequence or Journal record");
  fixture.signal.Release();
  (void)outside.Finish();
  result |= Created(fixture, fixture.Exchange(Request("POST", "/v1/jobs", CreateBody())), name, 1);
  return result;
}

struct ErrorVector {
  std::string name;
  std::string target;
  std::string request;
  Options options;
  int status;
  std::string domain;
  const char* code;
  bool before_method = false;
};

std::vector<ErrorVector> ErrorVectors() {
  Options timeout;
  timeout.body_timeout_ms = 25;
  std::vector<ErrorVector> vectors = {
      {"http-syntax",
       "/v1/jobs",
       "GET /v1/jobs HTTP/1.1\r\nHost: loopback\r\nBrokenField\r\n\r\n",
       {},
       400,
       "request",
       "malformed_request",
       true},
      {"json-syntax",
       "/v1/jobs",
       Request("GET", "/v1/jobs", "{"),
       {},
       400,
       "request",
       "malformed_json"},
      {"invalid-id",
       JobPath("invalid"),
       Request("GET", JobPath("invalid")),
       {},
       400,
       "request",
       "invalid_job_id"},
      {"query",
       "/v1/jobs?q",
       Request("GET", "/v1/jobs?q"),
       {},
       404,
       "request",
       "route_not_found",
       true},
      {"unknown-job", JobPath(), Request("GET", JobPath()), {}, 404, "job", "job_not_found"},
      {"method", "/v1/jobs", Request("PUT", "/v1/jobs"), {}, 405, "request", "method_not_allowed"},
      {"body-timeout", "/v1/jobs",
       "GET /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: "
       "application/json\r\nContent-Length: 10\r\n\r\n{",
       timeout, 408, "request", "request_timeout"},
      {"body-bound",
       "/v1/jobs",
       "GET /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: "
       "application/json\r\nContent-Length: 1048577\r\n\r\n",
       {},
       413,
       "request",
       "payload_too_large"},
      {"media",
       "/v1/jobs",
       Request("GET", "/v1/jobs", "{}", "text/plain"),
       {},
       415,
       "request",
       "unsupported_media_type"},
      {"validation",
       "/v1/jobs",
       Request("GET", "/v1/jobs", "{}"),
       {},
       422,
       "request",
       "validation_failed"},
      {"unknown-application",
       "/v1/jobs",
       Request("POST", "/v1/jobs", CreateBody("unknown")),
       {},
       422,
       "job",
       "unknown_application"},
      {"transfer",
       "/v1/jobs",
       "GET /v1/jobs HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
       {},
       501,
       "request",
       "not_implemented",
       true}};
  Options target_bound;
  target_bound.target_limit_bytes = 2048;
  const auto target = std::string("/").append(3000, 'a');
  vectors.push_back({"target-bound", target, Request("GET", target), target_bound, 414, "request",
                     "target_too_long", true});
  const auto header = std::string("GET /v1/jobs HTTP/1.1\r\nHost: loopback\r\nX-Fill: ")
                          .append(20000, 'a')
                          .append("\r\n\r\n");
  vectors.push_back(
      {"header-bound", "/v1/jobs", header, {}, 431, "request", "headers_too_large", true});
  return vectors;
}

class ThrowingBackend final : public FakeBackend {
 public:
  Readiness GetReadiness() override {
    throw std::runtime_error(
        "/tmp/issue106-private-path OS-error-marker library-error-marker journal-record-marker "
        "synthetic-secret-marker");
  }
};

int ErrorEnvelopeAndCodes() {
  constexpr std::string_view name = "rest_v1_error_envelope_and_codes";
  Scenario fixture;
  int result = 0;
  for (const auto& vector : ErrorVectors()) {
    const auto label = std::string(name).append(" [").append(vector.name).append("]");
    result |= RestError(Send(*fixture.backend, vector.options, vector.request), label,
                        vector.status, vector.domain, vector.code);
    auto head = vector.request;
    head.replace(0, head.find(' '), "HEAD");
    const auto head_status = vector.before_method ? vector.status : 405;
    const auto reference = vector.before_method ? std::string{} : Request("PUT", vector.target);
    result |= CheckHeadRefusalVector(
        *fixture.backend,
        HeadRefusalVector{label, vector.options, vector.request, head, reference, false,
                          vector.status, head_status, vector.domain, vector.code});
  }
  struct BodylessRoute {
    std::string target;
    std::string_view allow;
    int bodyless_status;
  };
  const std::array routes = {
      BodylessRoute{"/v1/health", "GET", 200}, BodylessRoute{"/v1/ready", "GET", 200},
      BodylessRoute{"/v1/jobs", "GET, POST", 200}, BodylessRoute{JobPath(), "GET", 404}};
  for (const auto& route : routes) {
    for (const auto& media :
         {std::optional<std::string>{}, std::optional<std::string>{"text/plain"},
          std::optional<std::string>{"application/json"}}) {
      const auto label = std::string(name)
                             .append(" [empty-chunked, ")
                             .append(route.target)
                             .append(", ")
                             .append(media.value_or("no media"))
                             .append("]");
      const auto headers =
          media ? std::string("Content-Type: ").append(*media).append("\r\n") : std::string{};
      auto get = Request("GET", route.target, std::nullopt, std::nullopt,
                         headers + "Transfer-Encoding: chunked\r\n") +
                 "0\r\n\r\n";
      const bool json = media && *media == "application/json";
      result |= RestError(fixture.Exchange(get), label, json ? 400 : 415, "request",
                          json ? "malformed_json" : "unsupported_media_type");
      auto head = get;
      head.replace(0, 3, "HEAD");
      const auto response = fixture.Exchange(head);
      result |= ExpectStatus(response, label, 405);
      const auto allow = response.headers.find("allow");
      result |= Check(allow != response.headers.end() && allow->second == route.allow, label,
                      "HEAD Allow mismatch");
      result |= Check(response.body.empty(), label, "HEAD response carried content");
      result |= ExpectStatus(fixture.Exchange(Request("GET", route.target, "", media)), label,
                             route.bodyless_status);
      result |= fixture.NoSubmission(label);
    }
  }
  ThrowingBackend throwing;
  result |= RestError(Send(throwing, Options{}, Request("GET", "/v1/ready")), name, 500, "service",
                      "internal");
  result |= CancelStatusMapping();
  result |= NotReadyRefusesWrites();
  result |= CreateNeverWaits();
  return result;
}

int ErrorsLeakNothing() {
  constexpr std::string_view name = "rest_v1_errors_leak_nothing";
  Scenario fixture;
  std::vector<Response> responses;
  responses.push_back(fixture.Exchange(Request(
      "POST", "/v1/jobs",
      R"({"application_id":"/tmp/issue106-private-path synthetic-secret-marker journal-record-marker"})")));
  responses.push_back(fixture.Exchange(Request("POST", "/v1/jobs", "{\"library-error-marker\":")));
  responses.push_back(fixture.Exchange(
      Request("POST", JobPath(k_unknown_id, true),
              R"({"principal":"synthetic-secret-marker","path":"/tmp/issue106-private-path"})")));
  ThrowingBackend throwing;
  responses.push_back(Send(throwing, Options{}, Request("GET", "/v1/ready")));
  int result = 0;
  for (std::size_t index = 0; index < responses.size(); ++index) {
    const auto& response = responses[index];
    result |= RestError(response, name,
                        index == 0 || index == 2 ? 422
                        : index == 1             ? 400
                                                 : 500,
                        index == 0   ? "job"
                        : index == 3 ? "service"
                                     : "request",
                        index == 0   ? "unknown_application"
                        : index == 2 ? "validation_failed"
                        : index == 1 ? "malformed_json"
                                     : "internal");
    for (const auto marker :
         {"/tmp/issue106-private-path", "OS-error-marker", "library-error-marker",
          "journal-record-marker", "synthetic-secret-marker"}) {
      result |= Check(response.body.find(marker) == std::string::npos, name,
                      "response leaked a private marker");
      for (const auto& [key, value] : response.headers) {
        result |=
            Check(key.find(marker) == std::string::npos && value.find(marker) == std::string::npos,
                  name, "header leaked a private marker");
      }
    }
  }
  return result;
}

std::size_t CharacterCount(std::string_view text) {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](unsigned char c) { return (c & 0xc0U) != 0x80U; }));
}

bool SchemaMatches(const Json& document, const Json& schema, const Json& value) {
  if (schema.is_boolean()) return schema.get<bool>();
  static const std::set<std::string, std::less<>> supported = {
      "$ref",    "type",      "enum",      "required",      "properties",           "items",
      "pattern", "minLength", "maxLength", "maxProperties", "additionalProperties", "description",
      "examples"};
  for (const auto& item : schema.items())
    if (!supported.contains(item.key()))
      throw std::runtime_error("unsupported OpenAPI schema keyword");
  if (schema.contains("$ref")) {
    const auto reference = schema.at("$ref").get<std::string>();
    if (!reference.starts_with("#/")) throw std::runtime_error("OpenAPI reference is not local");
    if (!SchemaMatches(document, document.at(Json::json_pointer(reference.substr(1))), value))
      return false;
  }
  const auto type_matches = [&](const Json& type) {
    return (type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
           (type == "string" && value.is_string()) || (type == "boolean" && value.is_boolean()) ||
           (type == "null" && value.is_null());
  };
  if (schema.contains("type")) {
    const auto& types = schema.at("type");
    if (types.is_array()) {
      if (!std::ranges::any_of(types, type_matches)) return false;
    } else if (!type_matches(types))
      return false;
  }
  if (schema.contains("enum") &&
      !std::ranges::any_of(schema.at("enum"), [&](const Json& item) { return item == value; }))
    return false;
  if (value.is_object()) {
    if (schema.contains("required"))
      for (const auto& key : schema.at("required"))
        if (!value.contains(key.get<std::string>())) return false;
    if (schema.contains("maxProperties") &&
        value.size() > schema.at("maxProperties").get<std::size_t>())
      return false;
    const auto properties = schema.value("properties", Json::object());
    for (const auto& item : value.items()) {
      if (properties.contains(item.key())) {
        if (!SchemaMatches(document, properties.at(item.key()), item.value())) return false;
      } else if (schema.contains("additionalProperties") &&
                 !SchemaMatches(document, schema.at("additionalProperties"), item.value()))
        return false;
    }
  }
  if (value.is_array() && schema.contains("items"))
    for (const auto& item : value)
      if (!SchemaMatches(document, schema.at("items"), item)) return false;
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    const auto length = CharacterCount(text);
    if (schema.contains("minLength") && length < schema.at("minLength").get<std::size_t>())
      return false;
    if (schema.contains("maxLength") && length > schema.at("maxLength").get<std::size_t>())
      return false;
    if (schema.contains("pattern") &&
        !std::regex_search(text, std::regex(schema.at("pattern").get<std::string>())))
      return false;
  }
  return true;
}

bool CrossFieldsMatch(const Json& value) {
  if (value.is_object() && value.contains("job_id") && value.contains("state")) {
    const auto state = value.at("state").get<std::string>();
    const bool terminal = state == "succeeded" || state == "failed" || state == "cancelled" ||
                          state == "terminated" || state == "timed_out";
    if (value.at("terminal") != terminal ||
        value.at("outcome") != (terminal ? Json(state) : Json(nullptr)))
      return false;
  }
  if (value.is_object() && value.contains("ready") && value.contains("reasons"))
    if (value.at("ready") != value.at("reasons").empty()) return false;
  if (value.is_object() && value.contains("error"))
    if (value.at("error").at("details") != Json::object()) return false;
  if (value.is_structured())
    for (const auto& item : value)
      if (!CrossFieldsMatch(item)) return false;
  return true;
}

int ContractResponse(const Json& document, std::string_view path, std::string_view method,
                     const Response& response, int expected_status) {
  constexpr std::string_view name = "rest_v1_openapi_contract";
  int result = ExpectStatus(response, name, expected_status);
  if (response.status != expected_status) return result;
  const auto& operation = document.at("paths").at(std::string(path)).at(std::string(method));
  const auto& declared = operation.at("responses").at(std::to_string(response.status));
  const auto& content = declared.at("content").at("application/json");
  result |= Check(response.headers.contains("content-type") &&
                      response.headers.at("content-type") == "application/json",
                  name, "wire Content-Type differs from OpenAPI");
  const auto body = Json::parse(response.body);
  result |= Check(SchemaMatches(document, content.at("schema"), body) && CrossFieldsMatch(body),
                  name, "actual response violates the OpenAPI schema or ADR cross-field rules");
  if (declared.contains("headers")) {
    for (const auto& header : declared.at("headers").items()) {
      const auto found = response.headers.find(Lower(header.key()));
      result |= Check(found != response.headers.end(), name,
                      "OpenAPI required response header is missing");
      if (found != response.headers.end())
        result |= Check(SchemaMatches(document, header.value().at("schema"), found->second), name,
                        "response header violates the OpenAPI schema");
    }
  }
  if (response.headers.contains("location"))
    result |=
        Check(response.status == 202 && path == "/v1/jobs" && method == "post" &&
                  response.headers.at("location") == JobPath(body.at("job_id").get<std::string>()),
              name, "Location does not identify the created resource");
  if (body.contains("error") && content.contains("examples")) {
    result |=
        Check(std::ranges::any_of(content.at("examples"),
                                  [&](const Json& example) {
                                    const auto& expected = example.at("value").at("error");
                                    return body.at("error").at("domain") == expected.at("domain") &&
                                           body.at("error").at("code") == expected.at("code");
                                  }),
              name, "status/domain/code pair is not documented by the operation");
  }
  return result;
}

int OpenApiContract() {
  constexpr std::string_view name = "rest_v1_openapi_contract";
  const auto document = ReadJsonFile("schemas/openapi/rest-v1.openapi.json");
  int result = Check(document.at("openapi") == "3.1.0", name, "wrong OpenAPI version");
  std::size_t operations = 0;
  for (const auto& item : document.at("paths"))
    for (const auto& value : item)
      if (value.is_object() && value.contains("operationId")) ++operations;
  result |= Check(operations == 6, name, "OpenAPI operation inventory changed");
  Scenario fresh;
  result |= ContractResponse(document, "/v1/jobs", "post",
                             fresh.Exchange(Request("POST", "/v1/jobs", CreateBody())), 202);
  result |= ContractResponse(document, "/v1/jobs", "get",
                             fresh.Exchange(Request("GET", "/v1/jobs")), 200);
  result |= ContractResponse(document, "/v1/health", "get",
                             fresh.Exchange(Request("GET", "/v1/health")), 200);
  result |= ContractResponse(document, "/v1/ready", "get",
                             fresh.Exchange(Request("GET", "/v1/ready")), 200);
  for (const auto state : {"admitted", "stopping", "finalizing", "succeeded", "failed", "cancelled",
                           "terminated", "timed_out"}) {
    ScenarioOptions options;
    options.seeds = {StateFixture(state)};
    Scenario fixture(options);
    const auto id = options.seeds[0].job_id.value;
    result |= ContractResponse(document, "/v1/jobs/{job_id}", "get",
                               fixture.Exchange(Request("GET", JobPath(id))), 200);
    const auto cancel = fixture.Exchange(Request("POST", JobPath(id, true)));
    result |= ContractResponse(document, "/v1/jobs/{job_id}/cancel", "post", cancel,
                               std::string_view(state) == "admitted" ? 202 : 409);
  }
  for (const auto& vector : ErrorVectors()) {
    if (vector.status == 405) continue;  // The component response is checked below with Allow.
    std::string path = "/v1/jobs";
    std::string method = vector.request.starts_with("POST ") ? "post" : "get";
    if (vector.name == "invalid-id" || vector.name == "unknown-job") path = "/v1/jobs/{job_id}";
    result |= ContractResponse(document, path, method,
                               Send(*fresh.backend, vector.options, vector.request), vector.status);
  }
  ScenarioOptions closed_options;
  closed_options.seeds = {StateFixture("admitted")};
  closed_options.unresolved = true;
  Scenario closed(closed_options);
  result |= ContractResponse(document, "/v1/ready", "get",
                             closed.Exchange(Request("GET", "/v1/ready")), 503);
  result |= ContractResponse(document, "/v1/jobs", "post",
                             closed.Exchange(Request("POST", "/v1/jobs", CreateBody())), 503);
  result |= ContractResponse(document, "/v1/jobs/{job_id}/cancel", "post",
                             closed.Exchange(Request("POST", JobPath(k_unknown_id, true))), 503);
  closed.writer->LatchReadinessFailure();
  result |= ContractResponse(document, "/v1/jobs", "post",
                             closed.Exchange(Request("POST", "/v1/jobs", CreateBody())), 503);
  result |= ContractResponse(document, "/v1/jobs/{job_id}/cancel", "post",
                             closed.Exchange(Request("POST", JobPath(k_unknown_id, true))), 503);
  for (const bool resident_full : {false, true}) {
    ScenarioOptions limits;
    if (resident_full) {
      limits.seeds = {StateFixture("admitted")};
      limits.max_jobs = 1;
    }
    Scenario limited(limits);
    WriterQueueHold hold(*limited.writer);
    const auto create = limited.Exchange(Request("POST", "/v1/jobs", CreateBody()));
    result |=
        RestError(create, name, 503, "service", resident_full ? "capacity_exhausted" : "busy");
    result |= ContractResponse(document, "/v1/jobs", "post", create, 503);
    const auto cancel = limited.Exchange(Request("POST", JobPath(k_unknown_id, true)));
    result |= RestError(cancel, name, 503, "service", "busy");
    result |= ContractResponse(document, "/v1/jobs/{job_id}/cancel", "post", cancel, 503);
  }
  for (const auto& [path, item] : document.at("paths").items()) {
    auto target = path;
    const auto parameter = target.find("{job_id}");
    if (parameter != std::string::npos) target.replace(parameter, 8, k_unknown_id);
    std::string allow;
    for (const auto& [method, operation] : item.items()) {
      if (!operation.is_object() || !operation.contains("operationId")) continue;
      if (!allow.empty()) allow.append(", ");
      auto upper = method;
      std::ranges::transform(upper, upper.begin(),
                             [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      allow.append(upper);
    }
    const auto refusal = fresh.Exchange(Request("PUT", target));
    result |= RestError(refusal, name, 405, "request", "method_not_allowed");
    result |= Check(refusal.headers.contains("allow") && refusal.headers.at("allow") == allow, name,
                    "Allow differs from OpenAPI operation inventory");
    if (refusal.status == 405)
      result |= Check(SchemaMatches(document,
                                    document.at("components")
                                        .at("responses")
                                        .at("MethodNotAllowed")
                                        .at("content")
                                        .at("application/json")
                                        .at("schema"),
                                    Json::parse(refusal.body)),
                      name, "405 body violates the OpenAPI component schema");
  }
  ThrowingBackend throwing;
  result |= ContractResponse(document, "/v1/ready", "get",
                             Send(throwing, Options{}, Request("GET", "/v1/ready")), 500);
  const auto& schema = document.at("components").at("schemas").at("Job");
  Json valid{
      {"job_id", GeneratedId(0)}, {"state", "admitted"}, {"terminal", false}, {"outcome", nullptr}};
  result |= Check(SchemaMatches(document, schema, valid) && CrossFieldsMatch(valid), name,
                  "schema evaluator rejected a valid Job control");
  for (const auto field : {"job_id", "state", "terminal", "outcome"}) {
    auto missing = valid;
    missing.erase(field);
    result |= Check(!SchemaMatches(document, schema, missing), name,
                    "schema evaluator accepted a missing required field");
  }
  auto extra = valid;
  extra["future_optional_field"] = 1;
  result |= Check(SchemaMatches(document, schema, extra), name,
                  "schema evaluator rejected an additive response field");
  for (const auto invalid_state : {"absent", "INVALID"}) {
    auto invalid = valid;
    invalid["state"] = invalid_state;
    result |= Check(!SchemaMatches(document, schema, invalid), name,
                    "schema evaluator ignored the closed state enum");
  }
  auto invalid = valid;
  invalid["job_id"] = "01890f30-7b54-4cc3-98c4-dc0c0c07398f";
  result |= Check(!SchemaMatches(document, schema, invalid), name,
                  "schema evaluator ignored UUID pattern");
  invalid = valid;
  invalid["terminal"] = true;
  result |= Check(!CrossFieldsMatch(invalid), name,
                  "cross-field check accepted inconsistent terminal state");
  auto impossible = schema;
  impossible["properties"]["job_id"]["pattern"] = "^never-a-job-id$";
  result |= Check(!SchemaMatches(document, impossible, valid), name,
                  "schema evaluator did not consult the schema pattern");
  const auto& create_schema = document.at("components").at("schemas").at("CreateJobRequest");
  result |= Check(
      SchemaMatches(document, create_schema, Json::parse(CreateBody())) &&
          !SchemaMatches(document, create_schema, Json{{"application_id", "app"}, {"extra", 1}}) &&
          !SchemaMatches(document, create_schema, Json{{"application_id", ""}}),
      name, "schema evaluator ignored request shape or character bounds");
  const auto& cancel_schema = document.at("components").at("schemas").at("CancelJobRequest");
  result |= Check(SchemaMatches(document, cancel_schema, Json::object()) &&
                      !SchemaMatches(document, cancel_schema, Json{{"x", 1}}),
                  name, "schema evaluator ignored empty cancel-object bound");
  const auto& location_schema = document.at("paths")
                                    .at("/v1/jobs")
                                    .at("post")
                                    .at("responses")
                                    .at("202")
                                    .at("headers")
                                    .at("Location")
                                    .at("schema");
  result |= Check(!SchemaMatches(document, location_schema, "/v1/jobs/invalid"), name,
                  "schema evaluator accepted an invalid Location");
  bool unsupported_refused = false;
  try {
    auto unreviewed = schema;
    unreviewed["unreviewedValidationKeyword"] = true;
    (void)SchemaMatches(document, unreviewed, valid);
  } catch (const std::runtime_error&) {
    unsupported_refused = true;
  }
  result |=
      Check(unsupported_refused, name, "schema evaluator silently ignored an unknown keyword");
  return result;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int SitometrondEmptyRegistry() {
  constexpr std::string_view name = "rest_v1_sitometrond_empty_registry";
  int result = 0;
  for (const unsigned seed : {0U, 1U, 2U}) {
    const bool unresolved = seed == 1;
    const bool resolved = seed == 2;
    const ScopedTempDirectory directory("issue115-empty-registry");
    const auto segment = directory.Path() / sitometron::journal::SegmentJournal::SegmentName(1);
    if (unresolved || resolved) {
      sitometron::journal::SegmentJournal journal(sitometron::journal::SystemFileSystem());
      if (!journal.Open(directory.Path().string()).ok)
        return Check(false, name, "seed Journal did not open");
      if (resolved) {
        for (const auto& event : sitometron::test::ClosedJobEvents(1, 115))
          if (journal.Commit(event) != core::LogicalCommitResult::kCommitted)
            return Check(false, name, "resolved seed Journal did not commit");
      } else {
        const core::Uuid id{std::string(k_job_id)};
        if (journal.Commit(core::LogicalJobEvent{1,
                                                 1,
                                                 core::EventType::kJobCreated,
                                                 {"2026-10-23T00:00:00Z"},
                                                 id,
                                                 core::JobCreatedPayload{id}}) !=
            core::LogicalCommitResult::kCommitted)
          return Check(false, name, "seed Journal did not commit");
      }
    }
    const auto original = ReadText(segment);
    sitometron::daemon::StartupConfig config;
    config.journal_path = directory.Path().string();
    config.http.listen_port = 0;
    config.max_jobs = 2;
    sitometron::daemon::Phase1Composition daemon(config);
    std::string error;
    if (!daemon.Start(error)) return Check(false, name, "production composition did not start");
    for (const auto& id : {std::string("a"), std::string("unknown"), std::string(128, 'z'),
                           std::string("UPPER"), std::string("a/b"), std::string("\xc3\xa9")})
      result |=
          RestError(SendToPort(daemon.listen_port(), Request("POST", "/v1/jobs", CreateBody(id))),
                    name, 422, "job", "unknown_application");
    if (!unresolved) {
      const auto turns_before = daemon.writer_turn_count();
      const auto wait_started = std::chrono::steady_clock::now();
      const auto canceled =
          daemon.backend().CancelJob(std::string(k_unknown_id), config.http.cancel_principal);
      const auto blocking = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - wait_started)
                                .count();
      const auto turns = daemon.writer_turn_count() - turns_before;
      result |= Check(canceled.code == sitometron::http::JobResultCode::kJobNotFound &&
                          !canceled.job && turns == 1,
                      name, "production Backend unknown cancel did not reject in one writer turn");
      std::cout << name << ": production unknown cancel synchronous_call_us=" << blocking
                << " writer_turns=" << turns << '\n';
      const auto started = std::chrono::steady_clock::now();
      const auto response =
          SendToPort(daemon.listen_port(), Request("POST", JobPath(k_unknown_id, true)));
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      std::cout << name << ": production unknown cancel status=" << response.status
                << " round_trip_us=" << elapsed << '\n';
      result |= RestError(response, name, 404, "job", "job_not_found");
    }
    if (resolved) {
      const auto turns_before = daemon.writer_turn_count();
      result |= RestError(SendToPort(daemon.listen_port(),
                                     Request("POST", JobPath(sitometron::test::Job(115), true))),
                          name, 409, "job", "command_not_allowed_in_state");
      result |= Check(daemon.writer_turn_count() == turns_before + 1, name,
                      "terminal replay cancel did not take one reducer-rejection writer turn");
    }
    const auto list = SendToPort(daemon.listen_port(), Request("GET", "/v1/jobs"));
    result |= ExpectStatus(list, name, 200);
    if (list.status == 200)
      result |= Check(Json::parse(list.body).at("jobs").size() == (seed != 0 ? 1U : 0U), name,
                      "production empty registry created a resident Job");
    daemon.Stop();
    result |= Check(daemon.clock_reads() == 0 && daemon.identity_reads() == 0, name,
                    "production empty registry accessed the clock or identity source");
    result |= Check(ReadText(segment) == original, name,
                    "production registry refusal changed Journal bytes");
  }
  return result;
}

}  // namespace job_routes

int Run(std::string_view name, const std::string& sitometrond_path) {
  if (name == "rest_v1_openapi_contract") return job_routes::OpenApiContract();
  if (name == "rest_v1_create_commits_before_202") return job_routes::CreateCommitsBefore202();
  if (name == "rest_v1_create_rejects_unknown_application")
    return job_routes::CreateRejectsUnknownApplication();
  if (name == "rest_v1_create_never_waits") return job_routes::CreateNeverWaits();
  if (name == "rest_v1_job_resource_from_committed_state")
    return job_routes::JobResourceFromCommittedState();
  if (name == "rest_v1_list_resident_in_creation_order")
    return job_routes::ListResidentInCreationOrder();
  if (name == "rest_v1_cancel_status_mapping") return job_routes::CancelStatusMapping();
  if (name == "rest_v1_cancel_records_configured_principal")
    return job_routes::CancelRecordsConfiguredPrincipal();
  if (name == "rest_v1_validation_before_submission")
    return job_routes::ValidationBeforeSubmission();
  if (name == "rest_v1_error_envelope_and_codes") return job_routes::ErrorEnvelopeAndCodes();
  if (name == "rest_v1_errors_leak_nothing") return job_routes::ErrorsLeakNothing();
  if (name == "rest_v1_not_ready_refuses_writes") return job_routes::NotReadyRefusesWrites();
  if (name == "rest_v1_create_serialized") return job_routes::CreateSerialized();
  if (name == "rest_v1_already_pending_is_busy") return job_routes::AlreadyPendingIsBusy();
  if (name == "rest_v1_create_without_body") return job_routes::CreateWithoutBody();
  if (name == "rest_v1_json_rejects_raw_nul") return job_routes::JsonRejectsRawNul();
  if (name == "rest_v1_json_rejects_invalid_utf8") return job_routes::JsonRejectsInvalidUtf8();
  if (name == "rest_v1_json_rejects_trailing_data") return job_routes::JsonRejectsTrailingData();
  if (name == "rest_v1_json_rejects_duplicate_keys_at_any_depth")
    return job_routes::JsonRejectsDuplicateKeysAtAnyDepth();
  if (name == "rest_v1_registered_application_id_form")
    return job_routes::RegisteredApplicationIdForm();
  if (name == "rest_v1_cancel_body") return job_routes::CancelBody();
  if (name == "rest_v1_json_rejects_leading_bom") return job_routes::JsonRejectsLeadingBom();
  if (name == "rest_v1_json_rejects_lone_surrogate") return job_routes::JsonRejectsLoneSurrogate();
  if (name == "rest_v1_sitometrond_empty_registry") return job_routes::SitometrondEmptyRegistry();
  if (name == "rest_v1_loopback_only") return LoopbackOnly();
  if (name == "rest_v1_unknown_route_and_method") return UnknownRouteAndMethod();
  if (name == "rest_v1_job_routes_not_ready") return JobRoutesNotReady();
  if (name == "rest_v1_health_and_readiness") return HealthAndReadiness();
  if (name == "rest_v1_malformed_request_refused") return MalformedRequest();
  if (name == "rest_v1_bare_lf_refused") return BareLfRefused();
  if (name == "rest_v1_http10_request_answered") return Http10Answered();
  if (name == "rest_v1_absolute_form_target") return AbsoluteForm();
  if (name == "rest_v1_targets_without_a_path") return TargetsWithoutPath();
  if (name == "rest_v1_header_receive_timeout") return HeaderTimeout();
  if (name == "rest_v1_target_too_long") return TargetTooLong();
  if (name == "rest_v1_headers_too_large") return HeadersTooLarge();
  if (name == "rest_v1_unknown_transfer_coding") return UnknownTransferCoding();
  if (name == "rest_v1_body_bound_after_routing") return BodyBoundAfterRouting();
  if (name == "rest_v1_body_receive_timeout") return BodyTimeout();
  if (name == "rest_v1_query_refused") return QueryRefused();
  if (name == "rest_v1_unlisted_methods_refused") return UnlistedMethods();
  if (name == "rest_v1_error_details_empty") return ErrorDetailsEmpty();
  if (name == "rest_v1_invalid_request_target_refused") return InvalidTarget();
  if (name == "rest_v1_missing_host_refused") return MissingHost();
  if (name == "rest_v1_repeated_host_refused") return RepeatedHost();
  if (name == "rest_v1_invalid_host_refused") return InvalidHost();
  if (name == "rest_v1_nonfinal_transfer_coding_refused") return NonfinalTransferCoding();
  if (name == "rest_v1_repeated_chunked_refused") return RepeatedChunked();
  if (name == "rest_v1_chunked_parameters_refused") return ChunkedParameters();
  if (name == "rest_v1_invalid_transfer_coding_refused") return InvalidTransferCoding();
  if (name == "rest_v1_transfer_encoding_with_content_length_refused")
    return TransferEncodingWithContentLengthRefused();
  if (name == "rest_v1_http10_transfer_encoding_refused") return Http10TransferEncoding();
  if (name == "rest_v1_obs_fold_refused") return ObsFoldRefused();
  if (name == "rest_v1_phase1_ports_not_called") return Phase1PortsNotCalled();
  if (name == "rest_v1_sitometrond_options") return SitometrondOptions();
  if (name == "rest_v1_sitometrond_entrypoint") return SitometrondEntrypoint(sitometrond_path);
  std::cerr << "unknown check " << name << '\n';
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "usage: sitometron_http_tests <check> [sitometrond]\n";
      return 2;
    }
    return Run(argv[1], argc == 3 ? argv[2] : "");
  } catch (...) {
    std::cerr << "uncaught test exception\n";
    return 1;
  }
}
