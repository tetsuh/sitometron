#ifndef SITOMETRON_HTTP_SERVER_HPP_
#define SITOMETRON_HTTP_SERVER_HPP_

#include <cstdint>
#include <memory>
#include <optional>
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

struct JobResource {
  std::string job_id;
  std::string state;
  bool terminal = false;
  std::optional<std::string> outcome;
};

enum class JobResultCode : std::uint8_t {
  kSuccess,
  kUnknownApplication,
  kJobNotFound,
  kStopCauseAlreadyLatched,
  kCommandNotAllowedInState,
  kNotReady,
  kServiceFailed,
  kCapacityExhausted,
  kBusy,
  kInternal
};

struct JobResult {
  JobResultCode code = JobResultCode::kInternal;
  std::optional<JobResource> job;
};

class Backend {
 public:
  virtual ~Backend() = default;
  [[nodiscard]] virtual Readiness GetReadiness() = 0;
  [[nodiscard]] virtual JobResult CreateJob(const std::string& application_id) = 0;
  [[nodiscard]] virtual std::optional<JobResource> GetJob(const std::string& job_id) = 0;
  [[nodiscard]] virtual std::vector<JobResource> ListJobs() = 0;
  [[nodiscard]] virtual JobResult CancelJob(const std::string& job_id,
                                            const std::string& principal) = 0;
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
