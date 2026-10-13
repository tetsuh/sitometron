#ifndef SITOMETRON_HTTP_SERVER_HPP_
#define SITOMETRON_HTTP_SERVER_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sitometron::http {

struct Options {
  std::string listen_address = "127.0.0.1";
  std::uint16_t listen_port = 8080;
  std::uint64_t target_limit_bytes = 8192;
  std::uint64_t header_limit_bytes = 16384;
  std::uint64_t body_limit_bytes = 65536;
  std::uint64_t header_timeout_ms = 5000;
  std::uint64_t body_timeout_ms = 5000;
  std::uint64_t combined_request_limit_bytes = 86016;
  std::string cancel_principal = "local-operator";
};

struct ReadinessReason {
  std::string code;
  std::vector<std::string> job_ids;
};

struct Readiness {
  bool ready = true;
  std::vector<ReadinessReason> reasons;
};

class Backend {
 public:
  virtual ~Backend() = default;
  [[nodiscard]] virtual Readiness GetReadiness() = 0;
};

class Server final {
 public:
  Server(Options options, Backend& backend);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  [[nodiscard]] bool Start(std::string& error);
  void Stop() noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitometron::http

#endif  // SITOMETRON_HTTP_SERVER_HPP_
