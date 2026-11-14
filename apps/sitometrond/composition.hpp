#ifndef SITOMETRON_SITOMETROND_COMPOSITION_HPP_
#define SITOMETRON_SITOMETROND_COMPOSITION_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "entrypoint.hpp"
#include "sitometron/http/server.hpp"

namespace sitometron::core {
struct Snapshot;
namespace internal {
class JobOrchestrator;
struct JobLaunchInputs;
}  // namespace internal
}  // namespace sitometron::core

namespace sitometron::daemon {

// Production passes identifiers from the startup-verified registry, or an empty list without
// --applications. Contract fixtures can also compose this Backend with test-only identifiers.
[[nodiscard]] std::unique_ptr<http::Backend> MakeJobBackend(
    core::internal::JobOrchestrator& orchestrator, std::size_t max_jobs,
    const std::vector<core::Snapshot>& replayed, const std::vector<std::string>& applications);

[[nodiscard]] std::unique_ptr<http::Backend> MakeJobBackend(
    core::internal::JobOrchestrator&, std::size_t, const std::vector<core::Snapshot>&,
    const std::vector<core::internal::JobLaunchInputs>&);

struct StartupConfig {
  std::string journal_path;
  std::optional<std::string> applications_path;
  std::size_t max_jobs = 32;
  http::Options http;
  bool hold_supervisor_for_test = false;
};

struct ParseResult {
  bool ok = false;
  bool help = false;
  std::string error;
  StartupConfig config;
};

[[nodiscard]] ParseResult ParseStartupOptions(const std::vector<std::string>& args);

class Phase1Composition final {
 public:
  explicit Phase1Composition(StartupConfig config,
                             IdentityFactory factory = identity::MakeProductionIdentitySource);
  ~Phase1Composition();
  Phase1Composition(const Phase1Composition&) = delete;
  Phase1Composition& operator=(const Phase1Composition&) = delete;

  [[nodiscard]] bool Start(std::string& error);
  void Stop() noexcept;
  [[nodiscard]] http::Backend& backend() noexcept;
  [[nodiscard]] std::size_t clock_reads() const noexcept;
  [[nodiscard]] std::size_t identity_reads() const noexcept;
  [[nodiscard]] std::size_t writer_turn_count() const noexcept;
  [[nodiscard]] std::uint16_t listen_port() const noexcept;
  bool WaitForSupervisorIdleForTest();
  bool ArmSupervisorBarrierForTest(std::string_view, std::string_view = {});
  bool WaitForSupervisorBarrierForTest();
  bool ReleaseSupervisorBarrierForTest();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitometron::daemon

#endif  // SITOMETRON_SITOMETROND_COMPOSITION_HPP_
