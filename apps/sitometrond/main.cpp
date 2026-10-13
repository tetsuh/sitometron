#include <iostream>

#include "sitometron/core/version.hpp"

#ifdef SITOMETRON_ENABLE_HTTP

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "composition.hpp"

namespace {

std::atomic_bool g_stopping{false};

void HandleSignal(int) { g_stopping.store(true); }

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty() || text.front() == '+' || text.front() == '-') return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool ParseListen(std::string_view text, std::string& address, std::uint16_t& port) {
  std::string_view port_text;
  if (text.starts_with('[')) {
    const auto close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':')
      return false;
    address = std::string(text.substr(1, close - 1));
    port_text = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || text.find(':') != colon) return false;
    address = std::string(text.substr(0, colon));
    port_text = text.substr(colon + 1);
  }
  std::uint64_t parsed = 0;
  if (address.empty() || !ParseUnsigned(port_text, parsed) || parsed > 65535) return false;
  port = static_cast<std::uint16_t>(parsed);
  return true;
}

bool VisibleAscii(std::string_view text, std::size_t maximum) {
  return !text.empty() && text.size() <= maximum &&
         std::ranges::all_of(text, [](unsigned char c) { return c > ' ' && c <= '~'; });
}

struct ParseResult {
  bool ok = false;
  bool help = false;
  std::string error;
  sitometron::daemon::StartupConfig config;
};

ParseResult ParseOptions(int argc, char** argv) {
  ParseResult result;
  result.config.http.listen_address = "127.0.0.1";
  result.config.http.listen_port = 8080;
  result.config.max_jobs = 32;
  std::set<std::string, std::less<>> seen;
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    if (flag == "--help" || flag == "-h") {
      if (argc != 2) {
        result.error = "--help cannot be combined with other options";
        return result;
      }
      result.help = true;
      result.ok = true;
      return result;
    }
    if (!flag.starts_with("--") || !seen.insert(std::string(flag)).second) {
      result.error = "unknown or repeated option: " + std::string(flag);
      return result;
    }
    const bool known_option =
        flag == "--listen" || flag == "--journal" || flag == "--cancel-principal" ||
        flag == "--max-jobs" || flag == "--target-limit" || flag == "--header-limit" ||
        flag == "--body-limit" || flag == "--header-timeout-ms" || flag == "--body-timeout-ms";
    if (!known_option) {
      result.error = "unknown option: " + std::string(flag);
      return result;
    }
    if (index + 1 >= argc) {
      result.error = "missing value for " + std::string(flag);
      return result;
    }
    const std::string_view value(argv[++index]);
    if (value.starts_with("--")) {
      result.error = "missing value for " + std::string(flag);
      return result;
    }
    if (flag == "--listen") {
      if (!ParseListen(value, result.config.http.listen_address, result.config.http.listen_port)) {
        result.error = "--listen must be ADDRESS:PORT or [IPv6]:PORT";
        return result;
      }
    } else if (flag == "--journal") {
      if (value.empty()) {
        result.error = "--journal needs a directory";
        return result;
      }
      result.config.journal_path = value;
    } else if (flag == "--cancel-principal") {
      if (!VisibleAscii(value, 256)) {
        result.error = "--cancel-principal must be 1 to 256 visible ASCII characters";
        return result;
      }
      result.config.http.cancel_principal = value;
    } else {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, parsed)) {
        result.error = "malformed numeric value for " + std::string(flag);
        return result;
      }
      if (flag == "--max-jobs") {
        if (parsed < 1 || parsed > 1024) {
          result.error = "--max-jobs must be between 1 and 1024";
          return result;
        }
        result.config.max_jobs = static_cast<std::size_t>(parsed);
      } else if (flag == "--target-limit") {
        if (parsed < 2048 || parsed > 65536) {
          result.error = "--target-limit must be between 2048 and 65536";
          return result;
        }
        result.config.http.target_limit_bytes = parsed;
      } else if (flag == "--header-limit") {
        if (parsed < 8192 || parsed > 262144) {
          result.error = "--header-limit must be between 8192 and 262144";
          return result;
        }
        result.config.http.header_limit_bytes = parsed;
      } else if (flag == "--body-limit") {
        if (parsed < 4096 || parsed > 1048576) {
          result.error = "--body-limit must be between 4096 and 1048576";
          return result;
        }
        result.config.http.body_limit_bytes = parsed;
      } else if (flag == "--header-timeout-ms") {
        if (parsed == 0 || parsed > 60000) {
          result.error = "--header-timeout-ms must be between 1 and 60000";
          return result;
        }
        result.config.http.header_timeout_ms = parsed;
      } else if (flag == "--body-timeout-ms") {
        if (parsed == 0 || parsed > 60000) {
          result.error = "--body-timeout-ms must be between 1 and 60000";
          return result;
        }
        result.config.http.body_timeout_ms = parsed;
      } else {
        result.error = "unknown option: " + std::string(flag);
        return result;
      }
    }
  }
  if (result.config.journal_path.empty()) {
    result.error = "--journal is required";
    return result;
  }
  const auto header = result.config.http.header_limit_bytes;
  const auto body = result.config.http.body_limit_bytes;
  if (header > std::numeric_limits<std::uint64_t>::max() - body ||
      header + body > std::numeric_limits<std::uint64_t>::max() - 4096U) {
    result.error = "request bounds overflow";
    return result;
  }
  result.config.http.combined_request_limit_bytes = header + body + 4096U;
  result.ok = true;
  return result;
}

void PrintUsage() {
  std::cout << "usage: sitometrond --journal DIR [options]\n"
               "  --listen ADDRESS:PORT       default 127.0.0.1:8080\n"
               "  --cancel-principal NAME    default local-operator\n"
               "  --max-jobs N                default 32\n"
               "  --target-limit BYTES       default 8192\n"
               "  --header-limit BYTES       default 16384\n"
               "  --body-limit BYTES         default 65536\n"
               "  --header-timeout-ms N      default 5000\n"
               "  --body-timeout-ms N        default 5000\n";
}

}  // namespace

#endif

int main(int argc, char** argv) {
  try {
#ifndef SITOMETRON_ENABLE_HTTP
    (void)argc;
    (void)argv;
    std::cout << "sitometrond " << sitometron::Version() << '\n';
    return 0;
#else
    const auto parsed = ParseOptions(argc, argv);
    if (!parsed.ok) {
      if (!parsed.error.empty()) std::cerr << "error: " << parsed.error << '\n';
      PrintUsage();
      return parsed.help ? 0 : 2;
    }
    if (parsed.help) {
      PrintUsage();
      return 0;
    }
    sitometron::daemon::Phase1Composition composition(parsed.config);
    std::string error;
    if (!composition.Start(error)) {
      std::cerr << "error: sitometrond startup failed: " << error << '\n';
      return 1;
    }
    std::signal(SIGINT, HandleSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, HandleSignal);
#endif
    std::cout << "sitometrond " << sitometron::Version() << " listening on "
              << parsed.config.http.listen_address << ':' << composition.listen_port() << '\n'
              << std::flush;
    while (!g_stopping.load()) std::this_thread::yield();
    composition.Stop();
    return 0;
#endif
  } catch (...) {
    std::cerr << "error: sitometrond terminated unexpectedly\n";
    return 1;
  }
}
