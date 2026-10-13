#include <iostream>

#include "sitometron/core/version.hpp"

#ifdef SITOMETRON_ENABLE_HTTP

#include <atomic>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include "composition.hpp"

namespace {

std::atomic_bool g_stopping{false};

void HandleSignal(int) { g_stopping.store(true); }

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
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    const auto parsed = sitometron::daemon::ParseStartupOptions(args);
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
