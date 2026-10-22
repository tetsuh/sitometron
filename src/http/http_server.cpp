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
  bool has_authority_host = false;
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
    kMalformed,
    kTimeout,
    kClosed,
    kTooLarge
  } result = Result::kClosed;
  std::string bytes;
  std::size_t end = 0;
  bool head_request = false;
};

struct HeaderTerminator {
  std::size_t position = 0;
  std::size_t size = 0;
};

std::optional<HeaderTerminator> FindHeaderTerminator(std::string_view bytes) {
  constexpr std::array<std::string_view, 4> k_terminators = {"\r\n\r\n", "\n\n", "\n\r\n",
                                                             "\r\n\n"};
  std::optional<HeaderTerminator> result;
  for (const auto terminator : k_terminators) {
    const auto position = bytes.find(terminator);
    if (position != std::string_view::npos && (!result || position < result->position)) {
      result = HeaderTerminator{position, terminator.size()};
    }
  }
  return result;
}

bool HasBareLineEnding(std::string_view bytes, std::size_t end) {
  for (std::size_t index = 0; index < end; ++index) {
    if (bytes[index] == '\n' && (index == 0 || bytes[index - 1] != '\r')) return true;
  }
  return false;
}

std::string RewriteHttpVersion(std::string_view version) {
  if (version.size() == 8 && version.starts_with("HTTP/1.") && version.back() >= '2' &&
      version.back() <= '9')
    return "HTTP/1.1";
  return std::string(version);
}

bool IsTchar(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') ||
         std::string_view("!#$%&'*+-.^_`|~").find(value) != std::string_view::npos;
}

bool IsHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

bool IsQdText(char value) {
  const auto byte = static_cast<unsigned char>(value);
  return byte == 0x09U || byte == 0x20U || byte == 0x21U || (byte >= 0x23U && byte <= 0x5bU) ||
         (byte >= 0x5dU && byte <= 0x7eU) || byte >= 0x80U;
}

bool IsQuotedPair(char value) {
  const auto byte = static_cast<unsigned char>(value);
  return byte == 0x09U || byte == 0x20U || (byte >= 0x21U && byte <= 0x7eU) || byte >= 0x80U;
}

struct ChunkScanner {
  enum class State : std::uint8_t { kSize, kData, kDataCrlf, kTrailer } state = State::kSize;
  std::string line;
  std::uint64_t remaining = 0;
  bool overflow = false;
  bool stopped = false;
  bool size_cr_seen = false;
  bool data_cr_seen = false;

  struct SizeResult {
    std::uint64_t value = 0;
    bool overflow = false;
  };

  static std::optional<SizeResult> ParseSizeLine(std::string_view value) {
    SizeResult result;
    if (value.empty() || !IsHex(value.front())) return std::nullopt;

    std::size_t position = 0;
    while (position < value.size() && IsHex(value[position])) {
      const char digit = value[position++];
      const auto number = digit >= '0' && digit <= '9' ? static_cast<std::uint64_t>(digit - '0')
                          : digit >= 'a' && digit <= 'f'
                              ? static_cast<std::uint64_t>(digit - 'a') + 10U
                              : static_cast<std::uint64_t>(digit - 'A') + 10U;
      if (!result.overflow) {
        if (result.value > (std::numeric_limits<std::uint64_t>::max() - number) / 16U)
          result.overflow = true;
        else
          result.value = result.value * 16U + number;
      }
    }

    for (;;) {
      const auto bws_begin = position;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      if (position == value.size()) {
        if (position != bws_begin) return std::nullopt;
        return result;
      }
      if (value[position] != ';') return std::nullopt;
      ++position;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      const auto name_begin = position;
      while (position < value.size() && IsTchar(value[position])) ++position;
      if (name_begin == position) return std::nullopt;

      const auto after_name = position;
      while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
        ++position;
      const bool had_bws_after_name = position != after_name;
      if (position < value.size() && value[position] == '=') {
        ++position;
        while (position < value.size() && (value[position] == ' ' || value[position] == '\t'))
          ++position;
        if (position == value.size()) return std::nullopt;
        if (value[position] == '"') {
          ++position;
          bool closed = false;
          while (position < value.size()) {
            if (value[position] == '\\') {
              ++position;
              if (position == value.size() || !IsQuotedPair(value[position])) return std::nullopt;
              ++position;
            } else if (value[position] == '"') {
              ++position;
              closed = true;
              break;
            } else {
              if (!IsQdText(value[position])) return std::nullopt;
              ++position;
            }
          }
          if (!closed) return std::nullopt;
        } else {
          const auto token_begin = position;
          while (position < value.size() && IsTchar(value[position])) ++position;
          if (token_begin == position) return std::nullopt;
        }
      } else {
        if (position == value.size()) {
          if (had_bws_after_name) return std::nullopt;
        } else if (value[position] != ';') {
          return std::nullopt;
        }
      }
    }
  }

  void Feed(std::string_view input) {
    for (std::size_t index = 0; index < input.size(); ++index) {
      if (overflow || stopped) return;
      const char byte = input[index];
      if (state == State::kSize) {
        if (size_cr_seen) {
          if (byte != '\n') {
            stopped = true;
            return;
          }
          line.push_back(byte);
          size_cr_seen = false;
          const auto value = std::string_view(line).substr(0, line.size() - 2);
          const auto parsed = ParseSizeLine(value);
          if (!parsed) {
            stopped = true;
            return;
          }
          line.clear();
          if (parsed->overflow) {
            overflow = true;
            return;
          }
          remaining = parsed->value;
          state = parsed->value == 0 ? State::kTrailer : State::kData;
        } else if (byte == '\r') {
          line.push_back(byte);
          size_cr_seen = true;
        } else if (byte == '\n') {
          stopped = true;
          return;
        } else {
          line.push_back(byte);
        }
      } else if (state == State::kData) {
        --remaining;
        if (remaining == 0) {
          state = State::kDataCrlf;
          data_cr_seen = false;
        }
      } else if (state == State::kDataCrlf) {
        if (data_cr_seen) {
          if (byte != '\n') {
            stopped = true;
            return;
          }
          data_cr_seen = false;
          state = State::kSize;
        } else if (byte == '\r') {
          data_cr_seen = true;
        } else {
          stopped = true;
          return;
        }
      } else {
        // Trailer syntax is owned by Beast. No chunk-size overflow can occur here.
      }
    }
  }
};

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

std::vector<std::string_view> SplitList(std::string_view value) {
  std::vector<std::string_view> elements;
  std::size_t element_start = 0;
  bool in_quotes = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char byte = value[index];
    if (in_quotes) {
      if (byte == '\\') {
        if (index + 1 < value.size()) ++index;
      } else if (byte == '"') {
        in_quotes = false;
      }
    } else if (byte == '"') {
      in_quotes = true;
    } else if (byte == ',') {
      const auto element = TrimOws(value.substr(element_start, index - element_start));
      if (!element.empty()) elements.push_back(element);
      element_start = index + 1;
    }
  }
  const auto element = TrimOws(value.substr(element_start));
  if (!element.empty()) elements.push_back(element);
  return elements;
}

bool HasInvalidTargetOctet(std::string_view target) {
  return std::ranges::any_of(target, [](unsigned char c) { return c <= 0x20U || c == 0x7fU; });
}

bool IsUnreserved(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
         value == '~';
}

bool IsSubDelim(char value) {
  return std::string_view("!$&'()*+,;=").find(value) != std::string_view::npos;
}

bool IsPctEncoded(std::string_view value, std::size_t position) {
  return position + 2 < value.size() && value[position] == '%' && IsHex(value[position + 1]) &&
         IsHex(value[position + 2]);
}

bool IsValidRegName(std::string_view value) {
  if (value.empty()) return false;
  for (std::size_t position = 0; position < value.size(); ++position) {
    const char character = value[position];
    if (IsUnreserved(character) || IsSubDelim(character)) continue;
    if (!IsPctEncoded(value, position)) return false;
    position += 2;
  }
  return true;
}

bool IsValidIpLiteral(std::string_view value) {
  if (value.empty()) return false;
  if (value.front() == 'v' || value.front() == 'V') {
    const auto dot = value.find('.');
    if (dot == std::string_view::npos || dot <= 1 || dot + 1 >= value.size()) return false;
    if (!std::ranges::all_of(value.substr(1, dot - 1), IsHex)) return false;
    return std::ranges::all_of(value.substr(dot + 1), [](char character) {
      return IsUnreserved(character) || IsSubDelim(character) || character == ':';
    });
  }
  boost::system::error_code error;
  if (value.find('%') != std::string_view::npos) return false;
  const auto address = asio::ip::make_address(std::string(value), error);
  return !error && address.is_v6();
}

bool IsValidUriHost(std::string_view value) { return IsValidRegName(value); }

bool IsValidAuthority(std::string_view value, bool require_port, bool allow_userinfo,
                      bool allow_empty_host) {
  if (value.empty()) return allow_empty_host && !require_port;
  const auto at = value.rfind('@');
  if (at != std::string_view::npos) {
    if (!allow_userinfo) return false;
    const auto userinfo = value.substr(0, at);
    for (std::size_t position = 0; position < userinfo.size(); ++position) {
      const char character = userinfo[position];
      if (IsUnreserved(character) || IsSubDelim(character) || character == ':') continue;
      if (!IsPctEncoded(userinfo, position)) return false;
      position += 2;
    }
    value.remove_prefix(at + 1);
  }
  if (value.empty()) return allow_empty_host && !require_port;

  std::string_view host;
  std::string_view port;
  bool has_port = false;
  if (value.front() == '[') {
    const auto close = value.find(']');
    if (close == std::string_view::npos || !IsValidIpLiteral(value.substr(1, close - 1)))
      return false;
    host = value.substr(1, close - 1);
    const auto suffix = value.substr(close + 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':') return false;
      has_port = true;
      port = suffix.substr(1);
    }
  } else {
    const auto colon = value.find(':');
    if (colon != std::string_view::npos) {
      if (value.find(':', colon + 1) != std::string_view::npos) return false;
      has_port = true;
      host = value.substr(0, colon);
      port = value.substr(colon + 1);
    } else {
      host = value;
    }
    if (!(allow_empty_host && host.empty()) && !IsValidUriHost(host)) return false;
  }
  if (value.front() == '[' && host.empty()) return false;
  if (require_port && !has_port) return false;
  if (!has_port) return true;
  return std::ranges::all_of(port,
                             [](char character) { return character >= '0' && character <= '9'; });
}

bool IsValidHost(std::string_view value, bool target_has_host) {
  value = TrimOws(value);
  if (value.empty()) return !target_has_host;
  return IsValidAuthority(value, false, false, true);
}

bool IsValidScheme(std::string_view value) {
  if (value.empty() || ((value.front() < 'A' || value.front() > 'Z') &&
                        (value.front() < 'a' || value.front() > 'z')))
    return false;
  return std::ranges::all_of(value.substr(1), [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '+' || character == '-' ||
           character == '.';
  });
}

bool IsValidPathAndQuery(std::string_view value) {
  const auto query = value.find('?');
  const auto path = value.substr(0, query);
  const auto query_part =
      query == std::string_view::npos ? std::string_view{} : value.substr(query + 1);
  const auto valid_pchar = [](char character) {
    return IsUnreserved(character) || IsSubDelim(character) || character == ':' || character == '@';
  };
  for (std::size_t position = 0; position < path.size(); ++position) {
    if (path[position] == '/') continue;
    if (path[position] == '%' && IsPctEncoded(path, position)) {
      position += 2;
      continue;
    }
    if (!valid_pchar(path[position])) return false;
  }
  for (std::size_t position = 0; position < query_part.size(); ++position) {
    if (query_part[position] == '/' || query_part[position] == '?' ||
        valid_pchar(query_part[position]))
      continue;
    if (query_part[position] == '%' && IsPctEncoded(query_part, position)) {
      position += 2;
      continue;
    }
    return false;
  }
  return true;
}

struct TargetInput {
  std::string_view method;
  std::string_view target;
};

std::optional<TargetInfo> ParseTarget(TargetInput input) {
  const auto method = input.method;
  const auto target = input.target;
  if (HasInvalidTargetOctet(target) || target.find('#') != std::string_view::npos)
    return std::nullopt;
  if (target == "*") {
    if (method != "OPTIONS") return std::nullopt;
    return TargetInfo{TargetKind::kNoPath, {}, false, false};
  }
  if (method == "CONNECT") {
    if (!IsValidAuthority(target, true, false, false)) return std::nullopt;
    std::size_t port_separator = std::string_view::npos;
    if (target.front() == '[') {
      const auto close = target.find(']');
      if (close == std::string_view::npos || close + 1 >= target.size() || target[close + 1] != ':')
        return std::nullopt;
      port_separator = close + 1;
    } else {
      port_separator = target.find(':');
      if (port_separator == std::string_view::npos) return std::nullopt;
    }
    const auto port_begin = port_separator + 1;
    if (port_begin >= target.size()) return std::nullopt;
    std::uint64_t port = 0;
    const auto parsed =
        std::from_chars(target.data() + port_begin, target.data() + target.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != target.data() + target.size() || port > 65535)
      return std::nullopt;
    return TargetInfo{TargetKind::kNoPath, {}, false, true};
  }

  const auto colon = target.find(':');
  const auto first_path_delimiter = target.find_first_of("/?");
  const bool is_absolute =
      colon != std::string_view::npos &&
      (first_path_delimiter == std::string_view::npos || colon < first_path_delimiter);
  if (is_absolute) {
    const auto scheme = Lower(target.substr(0, colon));
    if (!IsValidScheme(scheme)) return std::nullopt;
    const bool requires_host = scheme == "http" || scheme == "https";
    const auto hier_part = target.substr(colon + 1);
    if (!hier_part.starts_with("//")) {
      if (requires_host || !IsValidPathAndQuery(hier_part)) return std::nullopt;
      const auto query = hier_part.find('?');
      const auto path = hier_part.substr(0, query);
      return TargetInfo{TargetKind::kAbsolute, path.empty() ? "/" : std::string(path),
                        query != std::string_view::npos, false};
    }
    const auto authority_begin = colon + 3;
    const auto path_begin = target.find_first_of("/?", authority_begin);
    const auto authority = target.substr(authority_begin, path_begin == std::string_view::npos
                                                              ? std::string_view::npos
                                                              : path_begin - authority_begin);
    if (!IsValidAuthority(authority, false, true, !requires_host)) return std::nullopt;
    auto host_port = authority;
    if (const auto at = host_port.rfind('@'); at != std::string_view::npos)
      host_port.remove_prefix(at + 1);
    const bool has_host = !host_port.empty() && !host_port.starts_with(':');
    if (path_begin == std::string_view::npos)
      return TargetInfo{TargetKind::kAbsolute, "/", false, has_host};
    if (target[path_begin] == '?') {
      if (!IsValidPathAndQuery(target.substr(path_begin))) return std::nullopt;
      return TargetInfo{TargetKind::kAbsolute, "/", true, has_host};
    }
    const auto path = target.substr(path_begin);
    if (!IsValidPathAndQuery(path)) return std::nullopt;
    const auto query = path.find('?');
    return TargetInfo{TargetKind::kAbsolute, std::string(path.substr(0, query)),
                      query != std::string_view::npos, has_host};
  }
  if (target.empty() || target.front() != '/' || !IsValidPathAndQuery(target)) return std::nullopt;
  const auto query = target.find('?');
  return TargetInfo{TargetKind::kOrigin, std::string(target.substr(0, query)),
                    query != std::string_view::npos, false};
}

std::optional<std::uint64_t> ParseUint64(std::string_view value, bool& overflow) {
  overflow = false;
  if (value.empty()) return std::nullopt;
  std::uint64_t result = 0;
  bool overflowed = false;
  for (const char digit : value) {
    if (digit < '0' || digit > '9') {
      overflow = false;
      return std::nullopt;
    }
    if (overflowed) continue;
    const auto number = static_cast<std::uint64_t>(digit - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - number) / 10U) {
      overflowed = true;
      continue;
    }
    result = result * 10U + number;
  }
  overflow = overflowed;
  return overflowed ? std::nullopt : std::optional<std::uint64_t>(result);
}

std::string NormalizeDecimal(std::string_view value) {
  const auto first_nonzero = value.find_first_not_of('0');
  if (first_nonzero == std::string_view::npos) return "0";
  return std::string(value.substr(first_nonzero));
}

bool ParseTransferCoding(std::string_view value, std::vector<std::string>& codings,
                         bool& chunked_parameter) {
  for (const auto element : SplitList(value)) {
    std::size_t position = 0;
    const auto coding_begin = position;
    while (position < element.size() && IsTchar(element[position])) ++position;
    if (coding_begin == position) return false;
    const auto coding = Lower(element.substr(coding_begin, position - coding_begin));
    codings.push_back(coding);
    for (;;) {
      while (position < element.size() && (element[position] == ' ' || element[position] == '\t'))
        ++position;
      if (position == element.size()) break;
      if (element[position] != ';') return false;
      ++position;
      while (position < element.size() && (element[position] == ' ' || element[position] == '\t'))
        ++position;
      const auto parameter_begin = position;
      while (position < element.size() && IsTchar(element[position])) ++position;
      if (parameter_begin == position) return false;
      while (position < element.size() && (element[position] == ' ' || element[position] == '\t'))
        ++position;
      if (position == element.size() || element[position] != '=') return false;
      ++position;
      while (position < element.size() && (element[position] == ' ' || element[position] == '\t'))
        ++position;
      if (position == element.size()) return false;
      if (element[position] == '"') {
        ++position;
        bool closed = false;
        while (position < element.size()) {
          if (element[position] == '\\') {
            ++position;
            if (position == element.size() || !IsQuotedPair(element[position])) return false;
            ++position;
          } else if (element[position] == '"') {
            ++position;
            closed = true;
            break;
          } else {
            if (!IsQdText(element[position])) return false;
            ++position;
          }
        }
        if (!closed) return false;
      } else {
        const auto parameter_value_begin = position;
        while (position < element.size() && IsTchar(element[position])) ++position;
        if (parameter_value_begin == position) return false;
      }
      if (coding == "chunked") chunked_parameter = true;
    }
  }
  return true;
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
  while (start < header_end) {
    if (start + 1 < header_end && bytes[start] == '\r' && bytes[start + 1] == '\n') {
      start += 2;
    } else if (bytes[start] == '\n') {
      ++start;
    } else {
      break;
    }
  }
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
  const auto target_info = ParseTarget(TargetInput{request.method, request.target});
  if (!target_info) {
    error = EarlyError::kMalformed;
    return request;
  }
  request.target_info = *target_info;
  if (request.target.size() > options.target_limit_bytes) {
    error = EarlyError::kTargetTooLong;
    return request;
  }

  const auto rewritten_version = RewriteHttpVersion(request.version);
  const auto& version_for_rules = rewritten_version;

  std::string normalized;
  normalized.reserve(header_end - start + 8);
  normalized.append(request.method)
      .append(" ")
      .append(request.target)
      .append(" ")
      .append(rewritten_version)
      .append("\r\n");
  bool have_field = false;
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
      error = EarlyError::kMalformed;
      return request;
    }
    const auto colon = field_line.find(':');
    if (colon == std::string_view::npos || colon == 0 || !IsToken(field_line.substr(0, colon))) {
      normalized.append(field_line).append("\r\n");
      continue;
    }
    const auto name = Lower(field_line.substr(0, colon));
    const auto value = std::string(field_line.substr(colon + 1));
    request.fields.push_back({name, value});
    have_field = true;
    normalized.append(field_line).append("\r\n");
  }
  normalized.append("\r\n");

  for (auto& field : request.fields) field.value = std::string(TrimOws(field.value));
  std::vector<std::string_view> transfer_values;
  std::vector<std::string_view> content_lengths;
  for (const auto& field : request.fields) {
    if (field.name == "transfer-encoding") transfer_values.push_back(field.value);
    if (field.name == "content-length") content_lengths.push_back(field.value);
    if (field.name == "expect" && version_for_rules == "HTTP/1.1") {
      for (const auto element : SplitList(field.value)) {
        if (Lower(element) == "100-continue") request.expect_continue = true;
      }
    }
  }
  request.normalized_header = std::move(normalized);
  request.header_bytes = header_end + 4;

  const auto host_count = HeaderCount(request, "host");
  const bool http11_or_later = version_for_rules == "HTTP/1.1";
  if (host_count > 1 || (http11_or_later && host_count != 1)) {
    error = EarlyError::kMalformed;
    return request;
  }
  if (host_count == 1 &&
      !IsValidHost(HeaderValue(request, "host"), request.target_info.has_authority_host)) {
    error = EarlyError::kMalformed;
    return request;
  }

  bool length_overflow = false;
  bool content_length_invalid = false;
  std::optional<std::uint64_t> length;
  std::optional<std::string> overflow_value;
  for (const auto value : content_lengths) {
    if (content_length_invalid) break;
    for (const auto item : SplitList(value)) {
      bool overflow = false;
      const auto parsed = ParseUint64(item, overflow);
      if (!parsed && !overflow) {
        content_length_invalid = true;
        break;
      }
      if (overflow) {
        const auto normalized_item = NormalizeDecimal(item);
        if (length || (overflow_value && *overflow_value != normalized_item)) {
          content_length_invalid = true;
          break;
        }
        length_overflow = true;
        overflow_value = normalized_item;
      } else {
        if (length_overflow || overflow_value || (length && *length != *parsed)) {
          content_length_invalid = true;
          break;
        }
        length = parsed;
      }
    }
  }
  if (content_length_invalid) {
    length.reset();
    length_overflow = false;
  }
  request.content_length = length;
  request.content_length_overflow = length_overflow;
  request.has_content_length = !content_lengths.empty();

  if (!transfer_values.empty()) {
    request.has_transfer_encoding = true;
    if (request.has_content_length) {
      error = EarlyError::kMalformed;
      return request;
    }
    std::vector<std::string> codings;
    bool chunked_parameter = false;
    for (const auto& value : transfer_values) {
      if (!ParseTransferCoding(value, codings, chunked_parameter)) {
        error = EarlyError::kMalformed;
        return request;
      }
    }
    const auto chunked_count = static_cast<std::size_t>(
        std::count(codings.begin(), codings.end(), std::string("chunked")));
    if (version_for_rules == "HTTP/1.0" || chunked_parameter || chunked_count > 1 ||
        (!codings.empty() && codings.back() != "chunked")) {
      error = EarlyError::kMalformed;
      return request;
    }
    if (std::ranges::any_of(codings,
                            [](const std::string& coding) { return coding != "chunked"; })) {
      error = EarlyError::kNotImplemented;
      return request;
    }
    request.chunked = !codings.empty();
  }
  request.body_present = request.chunked || request.content_length_overflow ||
                         (request.content_length && *request.content_length != 0);
  std::optional<std::string> canonical_content_length;
  if (!content_length_invalid) {
    if (request.content_length_overflow) {
      const auto sentinel = options.body_limit_bytes == std::numeric_limits<std::uint64_t>::max()
                                ? options.body_limit_bytes
                                : options.body_limit_bytes + 1;
      canonical_content_length = std::to_string(sentinel);
    } else if (request.content_length) {
      canonical_content_length = std::to_string(*request.content_length);
    }
  }

  std::string rebuilt;
  std::size_t line_start = 0;
  while (line_start < request.normalized_header.size()) {
    const auto line_end = request.normalized_header.find("\r\n", line_start);
    if (line_end == std::string::npos) break;
    const auto normalized_line =
        std::string_view(request.normalized_header).substr(line_start, line_end - line_start);
    if (normalized_line.empty()) {
      if (canonical_content_length) {
        rebuilt.append("Content-Length:").append(*canonical_content_length).append("\r\n");
      }
      if (request.chunked) rebuilt.append("Transfer-Encoding:chunked\r\n");
      rebuilt.append("\r\n");
    } else {
      const auto colon = normalized_line.find(':');
      std::string field_name;
      if (colon != std::string_view::npos) field_name = Lower(normalized_line.substr(0, colon));
      if (field_name != "transfer-encoding" &&
          !(canonical_content_length && field_name == "content-length")) {
        rebuilt.append(normalized_line).append("\r\n");
      }
    }
    line_start = line_end + 2;
  }
  request.normalized_header = std::move(rebuilt);
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

ResponseSpec ParserErrorResponse(const ErrorCode& error) {
  const auto status = error == http::error::header_limit ? 431 : 400;
  return ErrorResponse(
      status, "request", status == 431 ? "headers_too_large" : "malformed_request",
      status == 431 ? "the request headers are too large" : "the request is not valid HTTP");
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

asio::awaitable<bool> WriteResponse(tcp::socket& socket, const ResponseSpec& spec,
                                    bool suppress_body = false) {
  http::response<http::string_body> response{static_cast<http::status>(spec.status), 11};
  response.set(http::field::content_type, "application/json");
  if (spec.allow) response.set(http::field::allow, *spec.allow);
  if (suppress_body) {
    response.content_length(static_cast<std::uint64_t>(spec.body.size()));
  } else {
    response.body() = spec.body;
    response.prepare_payload();
  }
  response.keep_alive(false);
  auto [error, ignored] =
      co_await http::async_write(socket, response, asio::as_tuple(asio::use_awaitable));
  (void)ignored;
  co_return !error;
}

bool RequestLineHasParserError(std::string_view bytes, std::size_t line_end) {
  std::string line(bytes.substr(0, line_end + 1));
  const auto first_space = line.find(' ');
  const auto second_space =
      first_space == std::string::npos ? std::string::npos : line.find(' ', first_space + 1);
  auto version_end = line_end;
  if (version_end > 0 && bytes[version_end - 1] == '\r') --version_end;
  if (second_space != std::string::npos && version_end > second_space + 1) {
    const auto version = bytes.substr(second_space + 1, version_end - second_space - 1);
    line.replace(second_space + 1, version.size(), RewriteHttpVersion(version));
  }

  http::request_parser<http::string_body> parser;
  parser.eager(false);
  ErrorCode error;
  parser.put(asio::buffer(line.data(), line.size()), error);
  return error == http::error::bad_version;
}

asio::awaitable<HeaderRead> ReadHeader(tcp::socket& socket, const Options& options) {
  HeaderRead result;
  std::string bytes;
  std::array<char, 4096> buffer{};
  bool request_line_checked = false;
  bool head_request_checked = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(options.header_timeout_ms);
  for (;;) {
    std::size_t leading = 0;
    while (leading < bytes.size()) {
      if (leading + 1 < bytes.size() && bytes[leading] == '\r' && bytes[leading + 1] == '\n') {
        leading += 2;
      } else if (bytes[leading] == '\n') {
        ++leading;
      } else {
        break;
      }
    }
    if (leading != 0) bytes.erase(0, leading);

    if (!head_request_checked && bytes.size() >= 5) {
      result.head_request = bytes.starts_with("HEAD ");
      head_request_checked = true;
    }

    const auto terminator = FindHeaderTerminator(bytes);
    if (terminator) {
      if (terminator->position + terminator->size > options.header_limit_bytes) {
        result.result = HeaderRead::Result::kTooLarge;
        co_return result;
      }
    } else if (bytes.size() > options.header_limit_bytes) {
      result.result = HeaderRead::Result::kTooLarge;
      co_return result;
    }
    if (!request_line_checked) {
      const auto line_end = bytes.find('\n');
      if (line_end != std::string::npos) {
        request_line_checked = true;
        if (line_end == 0 || bytes[line_end - 1] != '\r' ||
            RequestLineHasParserError(bytes, line_end)) {
          result.result = HeaderRead::Result::kMalformed;
          co_return result;
        }
      }
    }
    if (terminator) {
      if (HasBareLineEnding(bytes, terminator->position + terminator->size)) {
        result.result = HeaderRead::Result::kMalformed;
        co_return result;
      }
      result.result = HeaderRead::Result::kComplete;
      result.bytes = std::move(bytes);
      result.end = terminator->position;
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
  const bool head = header.head_request;
  if (header.result == HeaderRead::Result::kTimeout) {
    (void)co_await WriteResponse(
        socket,
        ErrorResponse(408, "request", "request_timeout", "the request was not received in time"),
        head);
    co_return;
  }
  if (header.result == HeaderRead::Result::kTooLarge) {
    (void)co_await WriteResponse(
        socket,
        ErrorResponse(431, "request", "headers_too_large", "the request headers are too large"),
        head);
    co_return;
  }
  if (header.result == HeaderRead::Result::kMalformed) {
    (void)co_await WriteResponse(socket, DispatchError(EarlyError::kMalformed), head);
    co_return;
  }
  if (header.result != HeaderRead::Result::kComplete) co_return;

  EarlyError early = EarlyError::kNone;
  auto request = ParseHeader(header.bytes, header.end, impl.options, early);
  if (early != EarlyError::kNone) {
    (void)co_await WriteResponse(socket, DispatchError(early), head);
    co_return;
  }

  http::request_parser<http::string_body> parser;
  parser.eager(false);
  parser.body_limit(std::numeric_limits<std::uint64_t>::max());
  parser.header_limit(static_cast<std::uint32_t>(impl.options.header_limit_bytes));
  ErrorCode parser_error;
  parser.put(asio::buffer(request.normalized_header.data(), request.normalized_header.size()),
             parser_error);
  if (parser_error && parser_error != http::error::need_more) {
    (void)co_await WriteResponse(socket, ParserErrorResponse(parser_error), head);
    co_return;
  }

  std::string job_id;
  const auto route_kind = ClassifyRoute(request.target_info.path, job_id);
  if (request.target_info.has_query || request.target_info.kind == TargetKind::kNoPath ||
      route_kind == Route::kNone) {
    (void)co_await WriteResponse(
        socket, ErrorResponse(404, "request", "route_not_found", "there is no such resource"),
        head);
    co_return;
  }
  if (!MethodAllowed(route_kind, request.method)) {
    auto response = ErrorResponse(405, "request", "method_not_allowed",
                                  "the method is not allowed for this resource");
    response.allow = MethodAllow(route_kind);
    (void)co_await WriteResponse(socket, response, head);
    co_return;
  }
  const bool body_present = request.body_present;
  if (body_present && !IsJsonMediaType(HeaderValue(request, "content-type"))) {
    (void)co_await WriteResponse(socket,
                                 ErrorResponse(415, "request", "unsupported_media_type",
                                               "the body must be application/json"),
                                 head);
    co_return;
  }
  const auto known_length = request.content_length.value_or(0);
  const bool body_too_large =
      request.content_length_overflow || known_length > impl.options.body_limit_bytes ||
      request.header_bytes + known_length > impl.options.combined_request_limit_bytes;
  if (body_too_large) {
    (void)co_await WriteResponse(
        socket, ErrorResponse(413, "request", "payload_too_large", "the request body is too large"),
        head);
    co_return;
  }
  parser.body_limit(impl.options.body_limit_bytes);
  if (request.expect_continue && body_present) {
    if (!(co_await WriteRaw(socket, "HTTP/1.1 100 Continue\r\n\r\n"))) co_return;
  }

  std::string body_bytes;
  std::size_t bytes_fed = 0;
  std::size_t raw_total = request.header_bytes;
  ChunkScanner scanner;
  std::string pending;
  const auto feed = [&](std::string_view bytes) -> ErrorCode {
    std::size_t held_back = 0;
    if (request.chunked) {
      scanner.Feed(bytes);
      if (scanner.overflow) return http::error::body_limit;
      if (scanner.state == ChunkScanner::State::kSize && !scanner.stopped &&
          (!scanner.line.empty() || scanner.size_cr_seen)) {
        // ChunkScanner buffers only this unfinished size line, which is the pending tail.
        held_back = scanner.line.size();
      }
    }
    pending.append(bytes.data(), bytes.size());
    while (!pending.empty() && !parser.is_done()) {
      if (pending.size() <= held_back) break;
      const auto forward_size = pending.size() - held_back;
      ErrorCode error;
      const auto consumed = parser.put(asio::buffer(pending.data(), forward_size), error);
      pending.erase(0, consumed);
      if (error && error != http::error::need_more) return error;
      if (error == http::error::need_more || consumed == 0) break;
    }
    return {};
  };
  const auto charge_raw_total = [&](std::size_t count) {
    bytes_fed += count;
    raw_total = request.header_bytes + bytes_fed;
    if (parser.is_done()) raw_total -= pending.size();
    return raw_total > impl.options.combined_request_limit_bytes;
  };
  const auto prebody = std::string_view(header.bytes).substr(header.end + 4);
  if (!prebody.empty()) {
    const auto error = feed(prebody);
    if (charge_raw_total(prebody.size())) {
      (void)co_await WriteResponse(
          socket,
          ErrorResponse(413, "request", "payload_too_large", "the request body is too large"),
          head);
      co_return;
    }
    if (error) {
      const auto status = error == http::error::body_limit ? 413 : 400;
      const auto code = status == 413 ? "payload_too_large" : "malformed_request";
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(status, "request", code,
                                                 status == 413 ? "the request body is too large"
                                                               : "the request body is not valid"),
                                   head);
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
                                                 "the request body was not received in time"),
                                   head);
      co_return;
    }
    const auto remaining = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(body_deadline - now).count());
    auto [error, count] =
        co_await ReadSome(socket, asio::buffer(buffer), remaining == 0 ? 1 : remaining);
    if (error == asio::error::operation_aborted) {
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(408, "request", "request_timeout",
                                                 "the request body was not received in time"),
                                   head);
      co_return;
    }
    if (error == asio::error::eof || error || count == 0) {
      (void)co_await WriteResponse(
          socket,
          ErrorResponse(400, "request", "malformed_request", "the request body is incomplete"),
          head);
      co_return;
    }
    const auto parse_error = feed(std::string_view(buffer.data(), count));
    if (charge_raw_total(count)) {
      (void)co_await WriteResponse(
          socket,
          ErrorResponse(413, "request", "payload_too_large", "the request body is too large"),
          head);
      co_return;
    }
    if (parse_error) {
      const auto status = parse_error == http::error::body_limit ? 413 : 400;
      const auto code = status == 413 ? "payload_too_large" : "malformed_request";
      (void)co_await WriteResponse(socket,
                                   ErrorResponse(status, "request", code,
                                                 status == 413 ? "the request body is too large"
                                                               : "the request body is not valid"),
                                   head);
      co_return;
    }
  }
  body_bytes = parser.is_done() ? parser.get().body() : std::string{};
  const auto response = Dispatch(request, body_bytes, impl.backend);
  (void)co_await WriteResponse(socket, response, head);
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
  impl_->io.stop();
  impl_->thread.join();
  boost::system::error_code error;
  error = impl_->acceptor.cancel(error);
  error = impl_->acceptor.close(error);
}

std::uint16_t Server::port() const noexcept { return impl_ ? impl_->port : 0; }

}  // namespace sitometron::http
