#include <algorithm>
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
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

std::string ReadUntilHeaders(boost::asio::ip::tcp::socket& socket, boost::system::error_code& ec) {
  std::string bytes;
  std::array<char, 4096> buffer{};
  while (!ec && bytes.find("\r\n\r\n") == std::string::npos) {
    const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
    if (count != 0) bytes.append(buffer.data(), count);
  }
  return bytes;
}

Response SendChunkedParts(Backend& backend, Options options, std::string header,
                          const std::vector<std::string>& parts) {
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
  const auto interim = ReadUntilHeaders(socket, ec);
  if (ec || !interim.starts_with("HTTP/1.1 100 Continue")) {
    ec = socket.close(ec);
    server.Stop();
    return ParseResponse(interim);
  }
  for (const auto& part : parts) {
    boost::asio::write(socket, boost::asio::buffer(part.data(), part.size()), ec);
    if (ec) break;
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
  result |= ExpectError(
      SendChunkedParts(
          backend, Options{},
          "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
          "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
          {"1", "\r\nx\r\n0\r\n\r\n"}),
      "rest_v1_job_routes_not_ready", 503, "service", "not_ready");
  return result;
}

int HealthAndReadiness() {
  FakeBackend backend;
  int result =
      ExpectStatus(Send(backend, Options{}, HealthRequest()), "rest_v1_health_and_readiness", 200);
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
  const auto request = "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nContent-Length: nope\r\n\r\n";
  return ExpectError(Send(backend, Options{}, request), "rest_v1_malformed_request_refused", 400,
                     "request", "malformed_request");
}

int Http10Answered() {
  FakeBackend backend;
  const auto response = Send(backend, Options{}, HealthRequest("GET", "/v1/health", "HTTP/1.0"));
  int result = ExpectStatus(response, "rest_v1_http10_request_answered", 200);
  result |= Check(response.version == "HTTP/1.1", "rest_v1_http10_request_answered",
                  "response version is not HTTP/1.1");
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
  return ExpectError(
      Send(backend, options,
           "GET /v1/" + std::string(9000, 'a') + " HTTP/1.1\r\nHost: loopback\r\n\r\n"),
      "rest_v1_target_too_long", 414, "request", "target_too_long");
}

int HeadersTooLarge() {
  FakeBackend backend;
  Options options;
  options.header_limit_bytes = 16384;
  return ExpectError(Send(backend, options,
                          "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nX-Fill: " +
                              std::string(17000, 'a') + "\r\n\r\n"),
                     "rest_v1_headers_too_large", 431, "request", "headers_too_large");
}

int UnknownTransferCoding() {
  FakeBackend backend;
  int result = ExpectError(
      Send(backend, Options{},
           "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nTransfer-Encoding: gzip\r\n\r\n"),
      "rest_v1_unknown_transfer_coding", 501, "request", "not_implemented");
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
  const auto chunk_overflow =
      "POST /v1/jobs HTTP/1.1\r\nHost: loopback\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\n";
  result |= ExpectError(Send(backend, Options{}, chunk_overflow),
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
  return ExpectError(Send(backend, Options{},
                          "GET /v1/health HTTP/1.1\r\nHost: loopback\r\nHost: loopback\r\n\r\n"),
                     "rest_v1_repeated_host_refused", 400, "request", "malformed_request");
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

int Phase1PortsNotCalled() {
  const auto directory =
      std::filesystem::temp_directory_path() / "issue105-phase1-composition-journal";
  std::error_code filesystem_error;
  std::filesystem::remove_all(directory, filesystem_error);
  {
    sitometron::journal::SegmentJournal journal(sitometron::journal::SystemFileSystem());
    const auto opened = journal.Open(directory.string());
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
  config.journal_path = directory.string();
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
  std::filesystem::remove_all(directory, filesystem_error);
  return result;
}

int SitometrondOptions() {
  int result = 0;
  const auto valid = sitometron::daemon::ParseStartupOptions(
      {"--journal", "/tmp/issue105-cli-journal", "--listen", "127.0.0.1:0", "--cancel-principal",
       "operator", "--max-jobs", "7", "--target-limit", "4096", "--header-limit", "8192",
       "--body-limit", "4096", "--header-timeout-ms", "17", "--body-timeout-ms", "19"});
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
  result |=
      expect_rejected({"--journal", "/tmp/issue105-cli-journal", "--unknown", "x"}, "unknown");
  result |=
      expect_rejected({"--journal", "/tmp/issue105-cli-journal", "--journal", "other"}, "repeated");
  result |= expect_rejected({"--journal", "/tmp/issue105-cli-journal", "--max-jobs"}, "missing");
  result |= expect_rejected({"--journal", "/tmp/issue105-cli-journal", "--body-limit", "nope"},
                            "malformed");
  result |= expect_rejected({"--journal", "/tmp/issue105-cli-journal", "--body-limit", "1048577"},
                            "out-of-range");

  const auto directory =
      std::filesystem::temp_directory_path() / "issue105-sitometrond-options-journal";
  std::error_code filesystem_error;
  std::filesystem::remove_all(directory, filesystem_error);
  sitometron::daemon::StartupConfig config;
  config.journal_path = directory.string();
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
  std::filesystem::remove_all(directory, filesystem_error);
  return result;
}

int Run(std::string_view name) {
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
  if (name == "rest_v1_http10_transfer_encoding_refused") return Http10TransferEncoding();
  if (name == "rest_v1_phase1_ports_not_called") return Phase1PortsNotCalled();
  if (name == "rest_v1_sitometrond_options") return SitometrondOptions();
  std::cerr << "unknown check " << name << '\n';
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: sitometron_http_tests <check>\n";
      return 2;
    }
    return Run(argv[1]);
  } catch (...) {
    std::cerr << "uncaught test exception\n";
    return 1;
  }
}
