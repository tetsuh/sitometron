#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#ifdef _WIN32
#include <process.h>
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
#include <boost/asio/write.hpp>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "apps/sitometrond/composition.hpp"
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

Response Send(Backend& backend, Options options, std::string request) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  boost::asio::ip::tcp::socket socket(context);
  boost::system::error_code ec;
  ec = socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
  if (ec) {
    server.Stop();
    return {};
  }
  boost::asio::write(socket, boost::asio::buffer(request.data(), request.size()), ec);
  std::string bytes;
  std::array<char, 4096> buffer{};
  while (!ec) {
    const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
    if (count != 0) bytes.append(buffer.data(), count);
  }
  if (ec != boost::asio::error::eof) {
    // A complete response may still have been received before the peer closes it.
  }
  ec = socket.close(ec);
  server.Stop();
  return ParseResponse(bytes);
}

Response SendPartial(Backend& backend, Options options, std::string prefix) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  boost::asio::ip::tcp::socket socket(context);
  boost::system::error_code ec;
  ec = socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
  if (!ec) boost::asio::write(socket, boost::asio::buffer(prefix.data(), prefix.size()), ec);
  std::string bytes;
  std::array<char, 4096> buffer{};
  while (!ec) {
    const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
    if (count != 0) bytes.append(buffer.data(), count);
  }
  ec = socket.close(ec);
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
  boost::asio::ip::tcp::socket socket(context);
  boost::system::error_code ec;
  ec = socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
  for (const auto& part : parts) {
    if (ec) break;
    boost::asio::write(socket, boost::asio::buffer(part.data(), part.size()), ec);
  }
  std::string bytes;
  std::array<char, 4096> buffer{};
  while (!ec) {
    const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
    if (count != 0) bytes.append(buffer.data(), count);
  }
  ec = socket.close(ec);
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

Response SendChunkedParts(Backend& backend, Options options, std::string header,
                          const std::vector<std::string>& parts, bool* saw_continue = nullptr,
                          bool wait_for_continue = false) {
  options.listen_port = 0;
  Server server(std::move(options), backend);
  std::string error;
  if (!server.Start(error)) {
    std::cerr << "server start: " << error << '\n';
    return {};
  }
  boost::asio::io_context context;
  boost::asio::ip::tcp::socket socket(context);
  boost::system::error_code ec;
  ec = socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()}, ec);
  if (!ec) boost::asio::write(socket, boost::asio::buffer(header.data(), header.size()), ec);

  constexpr std::string_view k_continue = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string bytes;
  std::array<char, 4096> buffer{};
  bool got_continue = false;
  if (wait_for_continue && !ec) {
    std::string interim;
    while (interim.size() < k_continue.size() && !ec) {
      const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
      if (count != 0) interim.append(buffer.data(), count);
    }
    got_continue = interim.starts_with(k_continue);
    if (!got_continue) bytes = std::move(interim);
  }
  if (!wait_for_continue || got_continue) {
    for (const auto& part : parts) {
      boost::asio::write(socket, boost::asio::buffer(part.data(), part.size()), ec);
      if (ec) break;
    }
  }
  if (!wait_for_continue || got_continue) {
    while (!ec) {
      const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
      if (count != 0) bytes.append(buffer.data(), count);
    }
  }
  ec = socket.close(ec);
  server.Stop();

  if (!wait_for_continue) {
    got_continue = bytes.starts_with(k_continue);
    if (got_continue) bytes.erase(0, k_continue.size());
  }
  if (saw_continue != nullptr) *saw_continue = got_continue;
  return ParseResponse(bytes);
}

Response SendToPort(std::uint16_t port, std::string request) {
  boost::asio::io_context context;
  boost::asio::ip::tcp::socket socket(context);
  boost::system::error_code ec;
  ec = socket.connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
  if (ec) return {};
  boost::asio::write(socket, boost::asio::buffer(request.data(), request.size()), ec);
  std::string bytes;
  std::array<char, 4096> buffer{};
  while (!ec) {
    const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
    if (count != 0) bytes.append(buffer.data(), count);
  }
  ec = socket.close(ec);
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

class FakeBackend final : public Backend {
 public:
  Readiness readiness{};

  Readiness GetReadiness() override { return readiness; }
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
  int result = ExpectError(Send(backend, Options{}, HealthRequest("GET", "/v1/jobs")),
                           "rest_v1_job_routes_not_ready", 503, "service", "not_ready");
  result |= ExpectError(Send(backend, Options{}, HealthRequest("POST", "/v1/jobs")),
                        "rest_v1_job_routes_not_ready", 503, "service", "not_ready");
  result |= ExpectError(
      Send(backend, Options{}, HealthRequest("GET", "/v1/jobs/" + std::string(k_job_id))),
      "rest_v1_job_routes_not_ready", 503, "service", "not_ready");
  result |=
      ExpectError(Send(backend, Options{},
                       HealthRequest("POST", "/v1/jobs/" + std::string(k_job_id) + "/cancel")),
                  "rest_v1_job_routes_not_ready", 503, "service", "not_ready");
  bool http11_expect_continue = false;
  const auto http11_expect_response = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
      {"1", "\r\nx\r\n0\r\n\r\n"}, &http11_expect_continue, true);
  result |= Check(http11_expect_continue, "rest_v1_job_routes_not_ready",
                  "HTTP/1.1 Expect did not produce 100 Continue");
  result |= ExpectError(http11_expect_response, "rest_v1_job_routes_not_ready", 503, "service",
                        "not_ready");
  bool http10_expect_continue = false;
  const auto http10_expect_response = SendChunkedParts(
      backend, Options{},
      "POST /v1/jobs HTTP/1.0\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 2\r\nExpect: 100-continue\r\n\r\n",
      {"{}"}, &http10_expect_continue);
  result |= Check(!http10_expect_continue, "rest_v1_job_routes_not_ready",
                  "HTTP/1.0 Expect produced 100 Continue");
  result |= ExpectError(http10_expect_response, "rest_v1_job_routes_not_ready", 503, "service",
                        "not_ready");
  return result;
}

int HealthAndReadiness() {
  FakeBackend backend;
  int result =
      ExpectStatus(Send(backend, Options{}, HealthRequest()), "rest_v1_health_and_readiness", 200);

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
  result |=
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nHost Name: loopback\r\n\r\n"),
                  "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
  result |= ExpectError(
      Send(backend, Options{}, "GET /v1/health HTTP/1.1 extra\r\nHost: loopback\r\n\r\n"),
      "rest_v1_malformed_request_refused", 400, "request", "malformed_request");
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
  return result;
}

int AbsoluteForm() {
  FakeBackend backend;
  const auto response =
      Send(backend, Options{}, HealthRequest("GET", "http://example.test/v1/health"));
  return ExpectStatus(response, "rest_v1_absolute_form_target", 200);
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
  return result;
}

int BodyBoundAfterRouting() {
  FakeBackend backend;
  int result = 0;
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
                        "rest_v1_body_bound_after_routing", 503, "service", "not_ready");

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
                        "rest_v1_body_bound_after_routing", 503, "service", "not_ready");

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
                        "rest_v1_body_bound_after_routing", 503, "service", "not_ready");
  result |= ExpectError(Send(backend, raw_options, chunked_over_combined),
                        "rest_v1_body_bound_after_routing", 413, "request", "payload_too_large");

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
                        "rest_v1_body_bound_after_routing", 503, "service", "not_ready");

  const auto overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, overflow), "rest_v1_body_bound_after_routing", 413,
                        "request", "payload_too_large");
  const auto conflicting_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Content-Length: 18446744073709551616, 18446744073709551617\r\n\r\n";
  result |= ExpectError(Send(backend, Options{}, conflicting_overflow),
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

int UnlistedMethods() {
  FakeBackend backend;
  const auto response = Send(backend, Options{}, HealthRequest("HEAD"));
  int result = ExpectError(response, "rest_v1_unlisted_methods_refused", 405, "request",
                           "method_not_allowed");
  if (response.status == 405)
    result |= Check(response.headers.at("allow") == "GET", "rest_v1_unlisted_methods_refused",
                    "Allow header mismatch");
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
  return ExpectError(Send(backend, Options{},
                          "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                          "Transfer-Encoding: chunked;foo\r\n\r\n"),
                     "rest_v1_chunked_parameters_refused", 400, "request", "malformed_request");
}

int InvalidTransferCoding() {
  FakeBackend backend;
  int result =
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                       "Transfer-Encoding: gzip;foo\r\n\r\n"),
                  "rest_v1_invalid_transfer_coding_refused", 400, "request", "malformed_request");
  result |=
      ExpectError(Send(backend, Options{},
                       "GET /v1/health HTTP/1.1\r\nHost: loopback\r\n"
                       "Transfer-Encoding:\r\nTransfer-Encoding: chunked\r\n\r\n"),
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
  return result;
}

int Http10TransferEncoding() {
  FakeBackend backend;
  return ExpectError(Send(backend, Options{},
                          "GET /v1/health HTTP/1.0\r\nHost: loopback\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n"),
                     "rest_v1_http10_transfer_encoding_refused", 400, "request",
                     "malformed_request");
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
    composition.Stop();
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

ProcessResult RunAndWait(const std::string& executable, const std::vector<std::string>& arguments) {
  std::vector<std::string> values;
  values.reserve(arguments.size() + 1);
  values.push_back(executable);
  values.insert(values.end(), arguments.begin(), arguments.end());
#ifdef _WIN32
  std::vector<const char*> argv;
  argv.reserve(values.size() + 1);
  for (const auto& value : values) argv.push_back(value.c_str());
  argv.push_back(nullptr);
  const auto exit_code = _spawnv(_P_WAIT, executable.c_str(), argv.data());
  if (exit_code < 0) return {};
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
  pid_t waited = 0;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status)) return {true, -1};
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
  std::array<char, 512> buffer{};
  for (;;) {
    const auto newline = line.find('\n');
    if (newline != std::string::npos) {
      line.resize(newline);
      return true;
    }
    if (line.size() > 4096) return false;
    pollfd event{descriptor, POLLIN, 0};
    int ready = 0;
    do {
      ready = ::poll(&event, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return false;
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count <= 0) return false;
    line.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

int StopRunningProcess(RunningProcess& process) {
  if (process.pid < 0) return -1;
  (void)::kill(process.pid, SIGTERM);
  int status = 0;
  pid_t waited = 0;
  do {
    waited = ::waitpid(process.pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  (void)::close(process.output_fd);
  process = {};
  if (waited <= 0 || !WIFEXITED(status)) return -1;
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

  const auto expect_rejected = [&](const std::vector<std::string>& args,
                                   std::string_view case_name) {
    const auto parsed = sitometron::daemon::ParseStartupOptions(args);
    return Check(!parsed.ok, "rest_v1_sitometrond_options",
                 std::string(case_name) + " command line was accepted");
  };
  result |= expect_rejected({"--journal", journal_path, "--unknown", "x"}, "unknown");
  result |= expect_rejected({"--journal", journal_path, "--journal", "other"}, "repeated");
  result |= expect_rejected({"--journal", journal_path, "--max-jobs"}, "missing");
  result |= expect_rejected({"--journal", journal_path, "--body-limit", "nope"}, "malformed");
  result |= expect_rejected({"--journal", journal_path, "--body-limit", "1048577"}, "out-of-range");

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
    result |= ExpectError(SendToPort(composition.listen_port(), HealthRequest("GET", "/v1/jobs")),
                          "rest_v1_sitometrond_options", 503, "service", "not_ready");
  }
  composition.Stop();
  return result;
}

int Run(std::string_view name, const std::string& sitometrond_path) {
  if (name == "rest_v1_loopback_only") return LoopbackOnly();
  if (name == "rest_v1_unknown_route_and_method") return UnknownRouteAndMethod();
  if (name == "rest_v1_job_routes_not_ready") return JobRoutesNotReady();
  if (name == "rest_v1_health_and_readiness") return HealthAndReadiness();
  if (name == "rest_v1_malformed_request_refused") return MalformedRequest();
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
