#ifndef SITOMETRON_SITOMETROND_COMPOSITION_HPP_
#define SITOMETRON_SITOMETROND_COMPOSITION_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sitometron/http/server.hpp"

namespace sitometron::daemon {

struct StartupConfig {
  std::string journal_path;
  std::size_t max_jobs = 32;
  http::Options http;
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
  explicit Phase1Composition(StartupConfig config);
  ~Phase1Composition();
  Phase1Composition(const Phase1Composition&) = delete;
  Phase1Composition& operator=(const Phase1Composition&) = delete;

  [[nodiscard]] bool Start(std::string& error);
  void Stop() noexcept;
  [[nodiscard]] http::Backend& backend() noexcept;
  [[nodiscard]] std::size_t clock_reads() const noexcept;
  [[nodiscard]] std::size_t identity_reads() const noexcept;
  [[nodiscard]] std::uint16_t listen_port() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitometron::daemon

#endif  // SITOMETRON_SITOMETROND_COMPOSITION_HPP_
