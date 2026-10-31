#include "entrypoint.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

#include "sitometron/core/version.hpp"
#include "sitometron/identity/system_clock.hpp"

#ifdef SITOMETRON_ENABLE_HTTP
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include "composition.hpp"
#endif

namespace sitometron::daemon {
#ifdef SITOMETRON_ENABLE_HTTP
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

int RunSitometrond(const std::vector<std::string>& args, IdentityFactory factory) {
  try {
#ifndef SITOMETRON_ENABLE_HTTP
    (void)args;
    if (!factory) throw std::invalid_argument("identity factory must be present");
    const auto source = factory(identity::kDefaultIdentityCapacity);
    const auto clock = identity::MakeProductionClock();
    if (!source || !clock) throw std::invalid_argument("production factory returned no port");
    std::cout << "sitometrond " << sitometron::Version() << '\n';
    return 0;
#else
    const auto parsed = ParseStartupOptions(args);
    if (!parsed.ok) {
      if (!parsed.error.empty()) std::cerr << "error: " << parsed.error << '\n';
      PrintUsage();
      return parsed.help ? 0 : 2;
    }
    if (parsed.help) {
      PrintUsage();
      return 0;
    }
    Phase1Composition composition(parsed.config, factory);
    std::string error;
    if (!composition.Start(error)) {
      std::cerr << "error: sitometrond startup failed: " << error << '\n';
      return 1;
    }
    g_stopping.store(false);
    std::signal(SIGINT, HandleSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, HandleSignal);
#endif
    std::cout << "sitometrond " << sitometron::Version() << " listening on "
              << parsed.config.http.listen_address << ':' << composition.listen_port() << '\n'
              << std::flush;
    while (!g_stopping.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    composition.Stop();
    return 0;
#endif
  } catch (const std::exception& error) {
    std::cerr << "error: sitometrond startup failed: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "error: sitometrond startup failed\n";
    return 1;
  }
}

}  // namespace sitometron::daemon
