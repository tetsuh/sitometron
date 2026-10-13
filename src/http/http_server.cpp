#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "sitometron/http/server.hpp"

namespace sitometron::http {
namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;
using Json = nlohmann::json;

constexpr std::uint64_t k_framing_allowance = 4096;
constexpr std::string_view k_json_content_type = "application/json";

struct HeaderField {
  std::string name;
  std::string value;
};

enum class EarlyError : std::uint8_t { kNone, kMalformed, kTargetTooLong, kNotImplemented };

enum class TargetKind : std::uint8_t { kOrigin, kAbsolute, kNoPath };

struct TargetInfo {
  TargetKind kind = TargetKind::kOrigin;
  std::string path;
  bool has_query = false;
};

struct RequestInfo {
  std::string method;
  std::string target;
  std::string version;
  std::string normalized_header;
  std::vector<HeaderField> fields;
  TargetInfo target_info;
  std::optional<std::uint64_t> content_length;
  bool content_length_overflow = false;
  bool has_content_length = false;
  bool has_transfer_encoding = false;
  bool chunked = false;
  bool body_present = false;
  bool expect_continue = false;
  std::size_t header_bytes = 0;
};

struct HeaderRead {
  enum class Result : std::uint8_t {
    kComplete,
    kTimeout,
    kClosed,
    kTooLarge
  } result = Result::kClosed;
  std::string bytes;
  std::size_t end = 0;
};

struct ChunkScanner {
  enum class State : std::uint8_t { kSize, kData, kDataCrlf, kTrailer } state = State::kSize;
  std::string line;
  std::uint64_t remaining = 0;
  bool overflow = false;

  void Feed(std::string_view input) {
    for (std::size_t index = 0; index < input.size() && !overflow; ++index) {
      const char byte = input[index];
      if (state == State::kSize) {
        line.push_back(byte);
        if (line.size() > 8192) return;
        if (line.size() >= 2 && line[line.size() - 2] == '\r' && byte == '\n') {
          auto value = std::string_view(line).substr(0, line.size() - 2);
          const auto semicolon = value.find(';');
          if (semicolon != std::string_view::npos) value = value.substr(0, semicolon);
          while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
          while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
          if (value.empty()) return;
          std::uint64_t parsed = 0;
          for (const char digit : value) {
            std::uint64_t number = 0;
            if (digit >= '0' && digit <= '9')
              number = static_cast<std::uint64_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f')
              number = static_cast<std::uint64_t>(digit - 'a') + 10U;
            else if (digit >= 'A' && digit <= 'F')
              number = static_cast<std::uint64_t>(digit - 'A') + 10U;
            else
              return;
            if (parsed > (std::numeric_limits<std::uint64_t>::max() - number) / 16U) {
              overflow = true;
              return;
            }
            parsed = parsed * 16U + number;
          }
          line.clear();
          remaining = parsed;
          state = parsed == 0 ? State::kTrailer : State::kData;
        }
      } else if (state == State::kData) {
        if (remaining != 0) --remaining;
        if (remaining == 0) state = State::kDataCrlf;
      } else if (state == State::kDataCrlf) {
        if (byte == '\n') state = State::kSize;
      } else {
        // Trailer syntax is owned by Beast. No chunk-size overflow can occur here.
      }
    }
  }
};

bool IsTchar(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') ||
         std::string_view("!#$%&'*+-.^_`|~").find(value) != std::string_view::npos;
}

bool IsToken(std::string_view value) {
  if (value.empty()) return false;
  return std::ranges::all_of(value, IsTchar);
}

std::string Lower(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string_view TrimOws(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}

bool HasInvalidTargetOctet(std::string_view target) {
  return std::ranges::any_of(target, [](unsigned char c) { return c <= 0x20U || c == 0x7fU; });
}

bool IsValidHost(std::string_view value, bool absolute_target) {
  value = TrimOws(value);
  if (value.empty()) return !absolute_target;
  if (value.front() == '[') {
    const auto close = value.find(']');
    if (close == std::string_view::npos || close == 1) return false;
    for (const char c : value.substr(1, close - 1)) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
            c == ':' || c == '.'))
        return false;
    }
    value.remove_prefix(close + 1);
    if (value.empty()) return true;
    if (value.front() != ':') return false;
    value.remove_prefix(1);
    return std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
  }
  const auto colon = value.find(':');
  const auto host = colon == std::string_view::npos ? value : value.substr(0, colon);
  if (host.empty() || host.find(':') != std::string_view::npos) return false;
  for (const char c : host) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '!' ||
                         c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
                         c == '+' || c == ',' || c == ';' || c == '=';
    if (!allowed) return false;
  }
  if (colon == std::string_view::npos) return true;
  return std::ranges::all_of(value.substr(colon + 1), [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<TargetInfo> ParseTarget(std::string_view method, std::string_view target) {
  if (target == "*") return TargetInfo{TargetKind::kNoPath, {}, false};
  if (HasInvalidTargetOctet(target) || target.find('#') != std::string_view::npos)
    return std::nullopt;
  if (method == "CONNECT" && target.find('/') == std::string_view::npos &&
      target.find('?') == std::string_view::npos) {
    const auto colon = target.rfind(':');
    if (colon != std::string_view::npos && colon != 0 && colon + 1 < target.size() &&
        std::ranges::all_of(target.substr(colon + 1), [](char c) { return c >= '0' && c <= '9'; }))
      return TargetInfo{TargetKind::kNoPath, {}, false};
  }
  const auto scheme = target.find("://");
  if (scheme != std::string_view::npos && scheme != 0) {
    if (!std::ranges::all_of(target.substr(0, scheme), IsTchar)) return std::nullopt;
    const auto authority_begin = scheme + 3;
    const auto path_begin = target.find_first_of("/?", authority_begin);
    const auto authority = target.substr(authority_begin, path_begin == std::string_view::npos
                                                              ? std::string_view::npos
                                                              : path_begin - authority_begin);
    if (authority.empty()) return std::nullopt;
    if (path_begin == std::string_view::npos) return TargetInfo{TargetKind::kAbsolute, "/", false};
    if (target[path_begin] == '?') return TargetInfo{TargetKind::kAbsolute, "/", true};
    const auto path = target.substr(path_begin);
    return TargetInfo{TargetKind::kAbsolute, std::string(path),
                      path.find('?') != std::string_view::npos};
  }
  if (target.empty() || target.front() != '/') return std::nullopt;
  return TargetInfo{TargetKind::kOrigin, std::string(target),
                    target.find('?') != std::string_view::npos};
}

std::optional<std::uint64_t> ParseUint64(std::string_view value, bool& overflow) {
  overflow = false;
  value = TrimOws(value);
  if (value.empty()) return std::nullopt;
  std::uint64_t result = 0;
  for (const char digit : value) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const auto number = static_cast<std::uint64_t>(digit - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - number) / 10U) {
      overflow = true;
      return std::nullopt;
    }
    result = result * 10U + number;
  }
  return result;
}

bool ParseTransferCoding(std::string_view value, std::vector<std::string>& codings,
                         bool& chunked_parameter) {
  std::size_t position = 0;
  while (position < value.size()) {
    while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
      ++position;
    if (position == value.size()) return false;
    const auto coding_begin = position;
    while (position < value.size() && IsTchar(value[position])) ++position;
    if (coding_begin == position) return false;
    codings.push_back(Lower(value.substr(coding_begin, position - coding_begin)));
    for (;;) {
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      if (position == value.size() || value[position] == ',') break;
      if (value[position] != ';') return false;
      ++position;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      const auto parameter_begin = position;
      while (position < value.size() && IsTchar(value[position])) ++position;
      if (parameter_begin == position) return false;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      if (position == value.size() || value[position] != '=') return false;
      ++position;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      if (position == value.size()) return false;
      if (value[position] == '"') {
        ++position;
        bool closed = false;
        while (position < value.size()) {
          if (value[position] == '\\') {
            ++position;
            if (position == value.size()) return false;
            ++position;
          } else if (value[position] == '"') {
            ++position;
            closed = true;
            break;
          } else {
            ++position;
          }
        }
        if (!closed) return false;
      } else {
        const auto parameter_value_begin = position;
        while (position < value.size() && IsTchar(value[position])) ++position;
        if (parameter_value_begin == position) return false;
      }
      if (codings.back() == "chunked") chunked_parameter = true;
    }
    while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
      ++position;
    if (position == value.size()) break;
    if (value[position] != ',') return false;
    ++position;
    if (position == value.size()) return false;
  }
  return !codings.empty();
}

std::string HeaderValue(const RequestInfo& request, std::string_view name) {
  const auto lowered = Lower(name);
  for (const auto& field : request.fields)
    if (field.name == lowered) return field.value;
  return {};
}

std::size_t HeaderCount(const RequestInfo& request, std::string_view name) {
  const auto lowered = Lower(name);
  return static_cast<std::size_t>(std::ranges::count_if(
      request.fields, [&](const auto& field) { return field.name == lowered; }));
}

bool IsJsonMediaType(std::string_view value) {
  const auto semicolon = value.find(';');
  if (semicolon != std::string_view::npos) value = value.substr(0, semicolon);
  value = TrimOws(value);
  return Lower(value) == k_json_content_type;
}

std::string ErrorBody(std::string_view domain, std::string_view code, std::string_view message) {
  return Json{
      {"error",
       {{"domain", domain}, {"code", code}, {"message", message}, {"details", Json::object()}}}}
      .dump();
}

struct ResponseSpec {
  int status = 500;
  std::string body;
  std::optional<std::string> allow = std::nullopt;
};

ResponseSpec ErrorResponse(int status, std::string_view domain, std::string_view code,
                           std::string_view message) {
  return {status, ErrorBody(domain, code, message), std::nullopt};
}

enum class Route : std::uint8_t { kNone, kJobs, kJob, kCancel, kHealth, kReady };

Route ClassifyRoute(std::string_view path, std::string& job_id) {
  if (path == "/v1/jobs") return Route::kJobs;
  if (path == "/v1/health") return Route::kHealth;
  if (path == "/v1/ready") return Route::kReady;
  constexpr std::string_view prefix = "/v1/jobs/";
  if (!path.starts_with(prefix)) return Route::kNone;
  auto suffix = path.substr(prefix.size());
  if (suffix.ends_with("/cancel")) {
    suffix.remove_suffix(7);
    if (suffix.empty() || suffix.find('/') != std::string_view::npos) return Route::kNone;
    job_id = suffix;
    return Route::kCancel;
  }
  if (suffix.empty() || suffix.find('/') != std::string_view::npos) return Route::kNone;
  job_id = suffix;
  return Route::kJob;
}

std::string MethodAllow(Route route) {
  switch (route) {
    case Route::kJobs:
      return "GET, POST";
    case Route::kJob:
      return "GET";
    case Route::kCancel:
      return "POST";
    case Route::kHealth:
    case Route::kReady:
      return "GET";
    case Route::kNone:
      break;
  }
  return {};
}

bool MethodAllowed(Route route, std::string_view method) {
  if (route == Route::kJobs) return method == "GET" || method == "POST";
  if (route == Route::kJob || route == Route::kHealth || route == Route::kReady)
    return method == "GET";
  if (route == Route::kCancel) return method == "POST";
  return false;
}

ResponseSpec Dispatch(const RequestInfo& request, std::string_view body, Backend& backend) {
  std::string ignored_job_id;
  const auto route = ClassifyRoute(request.target_info.path, ignored_job_id);
  if (route == Route::kNone)
    return ErrorResponse(404, "request", "route_not_found", "there is no such resource");
  if (!MethodAllowed(route, request.method)) {
    auto result = ErrorResponse(405, "request", "method_not_allowed",
                                "the method is not allowed for this resource");
    result.allow = MethodAllow(route);
    return result;
  }
  const auto body_present = !body.empty() || request.body_present;
  if (body_present && !IsJsonMediaType(HeaderValue(request, "content-type")))
    return ErrorResponse(415, "request", "unsupported_media_type",
                         "the body must be application/json");

  if (route == Route::kHealth) {
    if (body_present)
      return ErrorResponse(422, "request", "validation_failed", "the request has no body");
    return {200, R"({"status":"ok"})", std::nullopt};
  }
  if (route == Route::kReady) {
    if (body_present)
      return ErrorResponse(422, "request", "validation_failed", "the request has no body");
    const auto readiness = backend.GetReadiness();
    Json reasons = Json::array();
    for (const auto& reason : readiness.reasons) {
      Json value{{"code", reason.code}};
      if (!reason.job_ids.empty()) value["job_ids"] = reason.job_ids;
      reasons.push_back(std::move(value));
    }
    return {readiness.ready ? 200 : 503,
            Json{{"ready", readiness.ready}, {"reasons", std::move(reasons)}}.dump(), std::nullopt};
  }

  // Job resource semantics, including JSON parsing and business validation, belong to Issue #106.
  return ErrorResponse(503, "service", "not_ready", "Job routes are not available in Phase 1");
}

RequestInfo ParseHeader(std::string_view bytes, std::size_t header_end, const Options& options,
                        EarlyError& error) {
  RequestInfo request;
  error = EarlyError::kNone;
  std::size_t start = 0;
  while (start + 1 < header_end && bytes[start] == '\r' && bytes[start + 1] == '\n') start += 2;
  const auto first_end = bytes.find("\r\n", start);
  if (first_end == std::string_view::npos || first_end > header_end) {
    error = EarlyError::kMalformed;
    return request;
  }
  const auto line = bytes.substr(start, first_end - start);
  const auto first_space = line.find(' ');
  const auto second_space = first_space == std::string_view::npos ? std::string_view::npos
                                                                  : line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      second_space == first_space + 1) {
    error = EarlyError::kMalformed;
    return request;
  }
  request.method = std::string(line.substr(0, first_space));
  request.target = std::string(line.substr(first_space + 1, second_space - first_space - 1));
  request.version = std::string(line.substr(second_space + 1));
  const auto target_info = ParseTarget(request.method, request.target);
  if (!target_info) {
    error = EarlyError::kMalformed;
    return request;
  }
  request.target_info = *target_info;
  if (request.target.size() > options.target_limit_bytes) {
    error = EarlyError::kTargetTooLong;
    return request;
  }

  std::string rewritten_version = request.version;
  if (request.version.size() == 8 && request.version.starts_with("HTTP/1.") &&
      request.version.back() >= '2' && request.version.back() <= '9')
    rewritten_version = "HTTP/1.1";
  const auto version_for_rules = rewritten_version;

  std::string normalized;
  normalized.reserve(header_end - start + 8);
  normalized.append(request.method)
      .append(" ")
      .append(request.target)
      .append(" ")
      .append(rewritten_version)
      .append("\r\n");
  bool have_field = false;
  std::vector<std::string> transfer_values;
  std::vector<std::string> content_lengths;
  std::size_t position = first_end + 2;
  while (position < header_end) {
    const auto line_end = bytes.find("\r\n", position);
    if (line_end == std::string_view::npos || line_end > header_end) {
      error = EarlyError::kMalformed;
      return request;
    }
    const auto field_line = bytes.substr(position, line_end - position);
    position = line_end + 2;
    if (field_line.empty()) break;
    if (field_line.front() == ' ' || field_line.front() == '\t') {
      if (!have_field) {
        // RFC 9112 permits a recipient to consume whitespace-preceded lines before the first field.
        continue;
      }
      normalized.append(field_line).append("\r\n");
      if (!request.fields.empty()) {
        auto continuation = TrimOws(field_line);
        request.fields.back().value.append(" ").append(continuation);
      }
      continue;
    }
    const auto colon = field_line.find(':');
    if (colon == std::string_view::npos || colon == 0 || !IsToken(field_line.substr(0, colon))) {
      error = EarlyError::kMalformed;
      return request;
    }
    const auto name = Lower(field_line.substr(0, colon));
    const auto value = std::string(TrimOws(field_line.substr(colon + 1)));
    request.fields.push_back({name, value});
    have_field = true;
    if (name == "transfer-encoding") transfer_values.push_back(value);
    if (name == "content-length") content_lengths.push_back(value);
    if (name == "expect" && Lower(value) == "100-continue") request.expect_continue = true;
    normalized.append(field_line).append("\r\n");
  }
  normalized.append("\r\n");
  request.normalized_header = std::move(normalized);
  request.header_bytes = header_end + 4;

  const auto host_count = HeaderCount(request, "host");
  const bool http11_or_later =
      version_for_rules == "HTTP/1.1" ||
      (version_for_rules.size() == 8 && version_for_rules.starts_with("HTTP/1."));
  if (http11_or_later && host_count != 1) {
    error = EarlyError::kMalformed;
    return request;
  }
  if (host_count == 1 && !IsValidHost(HeaderValue(request, "host"),
                                      request.target_info.kind == TargetKind::kAbsolute)) {
    error = EarlyError::kMalformed;
    return request;
  }

  bool length_overflow = false;
  std::optional<std::uint64_t> length;
  for (const auto& value : content_lengths) {
    std::size_t item_start = 0;
    while (item_start <= value.size()) {
      const auto comma = value.find(',', item_start);
      const auto item = std::string_view(value).substr(
          item_start, comma == std::string::npos ? std::string::npos : comma - item_start);
      bool overflow = false;
      const auto parsed = ParseUint64(item, overflow);
      if (!parsed && !overflow) {
        error = EarlyError::kMalformed;
        return request;
      }
      if (overflow) {
        length_overflow = true;
      } else if (length && *length != *parsed) {
        error = EarlyError::kMalformed;
        return request;
      } else if (parsed) {
        length = parsed;
      }
      if (comma == std::string::npos) break;
      item_start = comma + 1;
    }
  }
  request.content_length = length;
  request.content_length_overflow = length_overflow;
  request.has_content_length = !content_lengths.empty();

  if (!transfer_values.empty()) {
    request.has_transfer_encoding = true;
    std::vector<std::string> codings;
    bool chunked_parameter = false;
    std::string combined;
    for (const auto& value : transfer_values) {
      if (!combined.empty()) combined.push_back(',');
      combined.append(value);
    }
    if (!ParseTransferCoding(combined, codings, chunked_parameter)) {
      error = EarlyError::kMalformed;
      return request;
    }
    const auto chunked_count = static_cast<std::size_t>(
        std::count(codings.begin(), codings.end(), std::string("chunked")));
    if (version_for_rules == "HTTP/1.0" || chunked_parameter || chunked_count > 1 ||
        (chunked_count == 1 && codings.back() != "chunked")) {
      error = EarlyError::kMalformed;
      return request;
    }
    if (chunked_count == 0) {
      error = EarlyError::kNotImplemented;
      return request;
    }
    request.chunked = true;
  }
  if (request.has_transfer_encoding && request.has_content_length) {
    // Beast owns this malformed framing case; returning the same refusal here preserves the
    // pre-routing owner and avoids ever reading a body with ambiguous framing.
    error = EarlyError::kMalformed;
    return request;
  }
  request.body_present = request.chunked || request.content_length_overflow ||
                         (request.content_length && *request.content_length != 0);
  if (request.content_length_overflow) {
    auto sentinel = options.body_limit_bytes == std::numeric_limits<std::uint64_t>::max()
                        ? options.body_limit_bytes
                        : options.body_limit_bytes + 1;
    std::string rebuilt;
    std::size_t line_start = 0;
    while (line_start < request.normalized_header.size()) {
      const auto line_end = request.normalized_header.find("\r\n", line_start);
      if (line_end == std::string::npos) break;
      const auto normalized_line =
          std::string_view(request.normalized_header).substr(line_start, line_end - line_start);
      const auto colon = normalized_line.find(':');
      if (colon != std::string_view::npos &&
          Lower(normalized_line.substr(0, colon)) == "content-length") {
        rebuilt.append(normalized_line.substr(0, colon))
            .append(": ")
            .append(std::to_string(sentinel));
      } else {
        rebuilt.append(normalized_line);
      }
      rebuilt.append("\r\n");
      line_start = line_end + 2;
    }
    request.normalized_header = std::move(rebuilt);
  }
  return request;
}

ResponseSpec DispatchError(EarlyError error) {
  switch (error) {
    case EarlyError::kMalformed:
      return ErrorResponse(400, "request", "malformed_request", "the request is not valid HTTP");
    case EarlyError::kTargetTooLong:
      return ErrorResponse(414, "request", "target_too_long", "the request target is too long");
    case EarlyError::kNotImplemented:
      return ErrorResponse(501, "request", "not_implemented",
                           "the transfer coding is not implemented");
    case EarlyError::kNone:
      break;
  }
  return ErrorResponse(500, "service", "internal", "the request could not be handled");
}

}  // namespace

struct Server::Impl {
  Impl(Options value, Backend& backend_value)
      : options(std::move(value)), backend(backend_value), acceptor(io) {}

  Options options;
  Backend& backend;
  asio::io_context io;
  tcp::acceptor acceptor;
  std::atomic_bool stopping{false};
  std::thread thread;
  std::uint16_t port = 0;
};

namespace {

asio::awaitable<std::tuple<ErrorCode, std::size_t>> ReadSome(tcp::socket& socket,
                                                             asio::mutable_buffer buffer,
                                                             std::uint64_t timeout_ms) {
  co_return co_await socket.async_read_some(
      buffer,
      asio::cancel_after(std::chrono::milliseconds(timeout_ms), asio::cancellation_type::total,
                         asio::as_tuple(asio::use_awaitable)));
}

asio::awaitable<bool> WriteRaw(tcp::socket& socket, std::string_view bytes) {
  auto [error, ignored] = co_await asio::async_write(
      socket, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
  (void)ignored;
  co_return !error;
}

asio::awaitable<bool> WriteResponse(tcp::socket& socket, const ResponseSpec& spec) {
  http::response<http::string_body> response{static_cast<http::status>(spec.status), 11};
  response.set(http::field::content_type, "application/json");
  if (spec.allow) response.set(http::field::allow, *spec.allow);
  response.body() = spec.body;
  response.keep_alive(false);
  response.prepare_payload();
  auto [error, ignored] =
      co_await http::async_write(socket, response, asio::as_tuple(asio::use_awaitable));
  (void)ignored;
  co_return !error;
}

asio::awaitable<HeaderRead> ReadHeader(tcp::socket& socket, const Options& options) {
  HeaderRead result;
  std::string bytes;
  std::array<char, 4096> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(options.header_timeout_ms);
  for (;;) {
    const auto end = bytes.find("\r\n\r\n");
    std::size_t leading = 0;
    while (leading + 1 < bytes.size() && bytes[leading] == '\r' && bytes[leading + 1] == '\n')
      leading += 2;
    if (end != std::string::npos) {
      const auto header_size = end + 4 - leading;
      if (header_size > options.header_limit_bytes) {
        result.result = HeaderRead::Result::kTooLarge;
        co_return result;
      }
      result.result = HeaderRead::Result::kComplete;
      result.bytes = std::move(bytes);
      result.end = end;
      co_return result;
    }
    if (bytes.size() - leading > options.header_limit_bytes) {
      result.result = HeaderRead::Result::kTooLarge;
      co_return result;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.result = HeaderRead::Result::kTimeout;
      co_return result;
    }
    const auto remaining = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    auto [error, count] =
        co_await ReadSome(socket, asio::buffer(buffer), remaining == 0 ? 1 : remaining);
    if (error == asio::error::operation_aborted) {
      result.result = HeaderRead::Result::kTimeout;
      co_return result;
    }
    if (error == asio::error::eof || (error && count == 0)) {
      result.result = HeaderRead::Result::kClosed;
      co_return result;
    }
    if (error) {
      result.result = HeaderRead::Result::kClosed;
      co_return result;
    }
    bytes.append(buffer.data(), count);
  }
}

asio::awaitable<void> HandleConnection(tcp::socket socket, Server::Impl& impl) {
  const auto header = co_await ReadHeader(socket, impl.options);
  if (header.result == HeaderRead::Result::kTimeout) {
    (void)co_await WriteResponse(socket, ErrorResponse(408, "request", "request_timeout",
                                                       "the request was not received in time"));
    co_return;
  }
  if (header.result == HeaderRead::Result::kTooLarge) {
    (void)co_await WriteResponse(socket, ErrorResponse(431, "request", "headers_too_large",
                                                       "the request headers are too large"));
    co_return;
  }
  if (header.result != HeaderRead::Result::kComplete) co_return;

  EarlyError early = EarlyError::kNone;
  auto request = ParseHeader(header.bytes, header.end, impl.options, early);
  if (early != EarlyError::kNone) {
    (void)co_await WriteResponse(socket, DispatchError(early));
    co_return;
  }

  http::request_parser<http::string_body> parser;
  parser.eager(false);
  parser.header_limit(static_cast<std::uint32_t>(impl.options.header_limit_bytes));
  ErrorCode parser_error;
  parser.put(asio::buffer(request.normalized_header.data(), request.normalized_header.size()),
             parser_error);
  if (parser_error) {
    const auto status = parser_error == http::error::header_limit ? 431 : 400;
    const auto code = status == 431 ? "headers_too_large" : "malformed_request";
    (void)co_await WriteResponse(socket,
                                 ErrorResponse(status, "request", code,
                                               status == 431 ? "the request headers are too large"
                                                             : "the request is not valid HTTP"));
    co_return;
  }

  std::string job_id;
  const auto route_kind = ClassifyRoute(request.target_info.path, job_id);
  if (request.target_info.has_query || request.target_info.kind == TargetKind::kNoPath ||
      route_kind == Route::kNone) {
    (void)co_await WriteResponse(
        socket, ErrorResponse(404, "request", "route_not_found", "there is no such resource"));
    co_return;
  }
  if (!MethodAllowed(route_kind, request.method)) {
    auto response = ErrorResponse(405, "request", "method_not_allowed",
                                  "the method is not allowed for this resource");
    response.allow = MethodAllow(route_kind);
    (void)co_await WriteResponse(socket, response);
    co_return;
  }
  const bool body_present = request.body_present;
  if (body_present && !IsJsonMediaType(HeaderValue(request, "content-type"))) {
    (void)co_await WriteResponse(socket, ErrorResponse(415, "request", "unsupported_media_type",
                                                       "the body must be application/json"));
    co_return;
  }
  const auto known_length = request.content_length.value_or(0);
  const bool body_too_large =
      request.content_length_overflow || known_length > impl.options.body_limit_bytes ||
      request.header_bytes + known_length > impl.options.combined_request_limit_bytes;
  if (body_too_large) {
    (void)co_await WriteResponse(socket, ErrorResponse(413, "request", "payload_too_large",
                                                       "the request body is too large"));
    co_return;
  }
  if (request.expect_continue && body_present) {
    if (!(co_await WriteRaw(socket, "HTTP/1.1 100 Continue\r\n\r\n"))) co_return;
  }

  parser.body_limit(impl.options.body_limit_bytes);
  std::string body_bytes;
  std::size_t raw_total = header.end + 4;
  ChunkScanner scanner;
  const auto feed = [&](std::string_view bytes) -> ErrorCode {
    if (request.chunked) {
      scanner.Feed(bytes);
      if (scanner.overflow) return http::error::body_limit;
    }
    ErrorCode error;
    parser.put(asio::buffer(bytes.data(), bytes.size()), error);
    return error;
  };
  const auto prebody = std::string_view(header.bytes).substr(header.end + 4);
  if (!prebody.empty()) {
    raw_total += prebody.size();
    if (raw_total > impl.options.combined_request_limit_bytes) {
      (void)co_await WriteResponse(socket, ErrorResponse(413, "request", "payload_too_large",
                                                         "the request body is too large"));
      co_return;
    }
    const auto error = feed(prebody);
    if (error) {
      const auto status = error == http::error::body_limit ? 413 : 400;
      const auto code = status == 413 ? "payload_too_large" : "malformed_request";
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(status, "request", code,
                                                 status == 413 ? "the request body is too large"
                                                               : "the request body is not valid"));
      co_return;
    }
  }
  const auto body_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(impl.options.body_timeout_ms);
  std::array<char, 4096> buffer{};
  while (body_present && !parser.is_done()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= body_deadline) {
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(408, "request", "request_timeout",
                                                 "the request body was not received in time"));
      co_return;
    }
    const auto remaining = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(body_deadline - now).count());
    auto [error, count] =
        co_await ReadSome(socket, asio::buffer(buffer), remaining == 0 ? 1 : remaining);
    if (error == asio::error::operation_aborted) {
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(408, "request", "request_timeout",
                                                 "the request body was not received in time"));
      co_return;
    }
    if (error == asio::error::eof || error || count == 0) {
      (void)co_await WriteResponse(socket, ErrorResponse(400, "request", "malformed_request",
                                                         "the request body is incomplete"));
      co_return;
    }
    raw_total += count;
    if (raw_total > impl.options.combined_request_limit_bytes) {
      (void)co_await WriteResponse(socket, ErrorResponse(413, "request", "payload_too_large",
                                                         "the request body is too large"));
      co_return;
    }
    const auto parse_error = feed(std::string_view(buffer.data(), count));
    if (parse_error) {
      const auto status = parse_error == http::error::body_limit ? 413 : 400;
      const auto code = status == 413 ? "payload_too_large" : "malformed_request";
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(status, "request", code,
                                                 status == 413 ? "the request body is too large"
                                                               : "the request body is not valid"));
      co_return;
    }
  }
  body_bytes = parser.is_done() ? parser.get().body() : std::string{};
  const auto response = Dispatch(request, body_bytes, impl.backend);
  (void)co_await WriteResponse(socket, response);
}

asio::awaitable<void> AcceptLoop(Server::Impl& impl) {
  while (!impl.stopping.load()) {
    auto [error, socket] = co_await impl.acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (error) {
      if (impl.stopping.load()) co_return;
      continue;
    }
    asio::co_spawn(impl.io, HandleConnection(std::move(socket), impl), asio::detached);
  }
}

}  // namespace

Server::Server(Options options, Backend& backend)
    : impl_(std::make_unique<Impl>(std::move(options), backend)) {}

Server::~Server() { Stop(); }

bool Server::Start(std::string& error) {
  if (impl_->thread.joinable()) {
    error = "server is already running";
    return false;
  }
  boost::system::error_code address_error;
  const auto address = asio::ip::make_address(impl_->options.listen_address, address_error);
  if (address_error || !address.is_loopback()) {
    error = "listen address must be loopback";
    return false;
  }
  if (impl_->options.target_limit_bytes < 2048 || impl_->options.header_limit_bytes < 8192 ||
      impl_->options.body_limit_bytes < 4096 || impl_->options.header_timeout_ms == 0 ||
      impl_->options.body_timeout_ms == 0) {
    error = "HTTP bounds are outside the supported range";
    return false;
  }
  if (impl_->options.header_limit_bytes > std::numeric_limits<std::uint32_t>::max()) {
    error = "header bound is too large";
    return false;
  }
  if (impl_->options.header_limit_bytes >
          std::numeric_limits<std::uint64_t>::max() - impl_->options.body_limit_bytes ||
      impl_->options.header_limit_bytes + impl_->options.body_limit_bytes >
          std::numeric_limits<std::uint64_t>::max() - k_framing_allowance) {
    error = "combined request bounds overflow";
    return false;
  }
  const auto minimum_combined =
      impl_->options.header_limit_bytes + impl_->options.body_limit_bytes + k_framing_allowance;
  if (impl_->options.combined_request_limit_bytes < minimum_combined) {
    error = "combined request bound is too small";
    return false;
  }
  boost::system::error_code open_error;
  open_error = impl_->acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6(), open_error);
  if (open_error) {
    error = "cannot open listener";
    return false;
  }
  open_error = impl_->acceptor.set_option(tcp::acceptor::reuse_address(true), open_error);
  open_error = impl_->acceptor.bind({address, impl_->options.listen_port}, open_error);
  if (!open_error)
    open_error = impl_->acceptor.listen(asio::socket_base::max_listen_connections, open_error);
  if (open_error) {
    error = "cannot bind listener";
    open_error = impl_->acceptor.close(open_error);
    return false;
  }
  impl_->port = impl_->acceptor.local_endpoint().port();
  impl_->stopping.store(false);
  impl_->io.restart();
  asio::co_spawn(impl_->io, AcceptLoop(*impl_), asio::detached);
  impl_->thread = std::thread([impl = impl_.get()] { impl->io.run(); });
  return true;
}

void Server::Stop() noexcept {
  if (!impl_ || !impl_->thread.joinable()) return;
  impl_->stopping.store(true);
  boost::system::error_code error;
  error = impl_->acceptor.cancel(error);
  error = impl_->acceptor.close(error);
  impl_->io.stop();
  impl_->thread.join();
}

std::uint16_t Server::port() const noexcept { return impl_ ? impl_->port : 0; }

}  // namespace sitometron::http
