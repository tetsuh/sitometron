#include "composition.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "job_orchestrator.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::daemon {
namespace {

using core::internal::IngressCode;
using core::internal::JobOrchestrator;

class Phase1Clock final : public core::ClockPort {
 public:
  explicit Phase1Clock(std::size_t& reads) : reads_(reads) {}
  [[nodiscard]] core::ClockReading Read() override {
    ++reads_;
    throw std::runtime_error("Phase 1 clock placeholder was called");
  }

 private:
  std::size_t& reads_;
};

class Phase1IdentitySource final : public core::IdentitySourcePort {
 public:
  explicit Phase1IdentitySource(std::size_t& reads) : reads_(reads) {}
  [[nodiscard]] core::JobSessionIdentityResult GenerateJobSessionIdentity() override {
    ++reads_;
    return core::IdentitySourceExhausted{};
  }
  [[nodiscard]] core::WorkerIdentityResult GenerateWorkerIdentity() override {
    ++reads_;
    return core::IdentitySourceExhausted{};
  }
  [[nodiscard]] core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    ++reads_;
    return core::IdentitySourceExhausted{};
  }

 private:
  std::size_t& reads_;
};

class Phase1Runner final : public core::ApplicationRunnerPort {
 public:
  void HandoffLaunch(core::ApplicationLaunchRequest&&) noexcept override {}
  void HandoffCooperativeStop(core::ApplicationStopRequest&&) noexcept override {}
  void HandoffForcedStop(core::ApplicationStopRequest&&) noexcept override {}
};

class Phase1SessionRetainer final : public core::SessionRetainerPort {
 public:
  void HandoffRetainSameIdentity(core::SessionRetainRequest&&) noexcept override {}
};

bool RegisteredApplicationForm(std::string_view value) {
  const auto alphanumeric = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 128 && alphanumeric(value.front()) &&
         std::ranges::all_of(value, [&](unsigned char c) {
           return alphanumeric(c) || c == '.' || c == '_' || c == ':' || c == '-';
         });
}

http::JobResultCode IngressStatus(IngressCode code) {
  switch (code) {
    case IngressCode::kServiceFailed:
      return http::JobResultCode::kServiceFailed;
    case IngressCode::kAdmissionClosed:
      return http::JobResultCode::kNotReady;
    case IngressCode::kResidentLimit:
      return http::JobResultCode::kCapacityExhausted;
    case IngressCode::kNormalFull:
    case IngressCode::kAlreadyPending:
      return http::JobResultCode::kBusy;
    case IngressCode::kAdmitted:
    case IngressCode::kCoalescedPending:
      return http::JobResultCode::kInternal;
  }
  return http::JobResultCode::kInternal;
}

class Phase1Backend final : public http::Backend {
 public:
  Phase1Backend(JobOrchestrator& orchestrator, std::size_t max_jobs,
                const std::vector<core::Snapshot>& replayed,
                const std::vector<std::string>& applications)
      : orchestrator_(orchestrator), applications_(applications), order_(max_jobs) {
    if (max_jobs == 0 || replayed.size() > max_jobs ||
        !std::ranges::all_of(applications_, RegisteredApplicationForm))
      throw std::invalid_argument("invalid Backend composition");
    // Reserve every fixed-size UUID string before serving requests. Index publication reuses
    // this storage only after successful completion and confirmation of the committed resource.
    for (auto& id : order_) id.reserve(36);
    for (const auto& snapshot : replayed) order_[order_size_++] = snapshot.job_id.value;
  }

  [[nodiscard]] http::Readiness GetReadiness() override {
    http::Readiness result;
    result.ready = orchestrator_.ready();
    // A later failure must not attach a false-readiness reason to an earlier ready observation.
    if (!result.ready && orchestrator_.failed()) result.reasons.push_back({"service_failed", {}});
    std::vector<std::string> unresolved;
    for (const auto& id : orchestrator_.unresolved()) unresolved.push_back(id.value);
    if (!unresolved.empty()) result.reasons.push_back({"unresolved_jobs", std::move(unresolved)});
    if (!result.ready && result.reasons.empty()) result.reasons.push_back({"shutting_down", {}});
    return result;
  }

  [[nodiscard]] http::JobResult CreateJob(const std::string& application_id) override {
    if (std::ranges::find(applications_, application_id) == applications_.end())
      return {http::JobResultCode::kUnknownApplication, std::nullopt};
    // Serialize generation, admission, the sequence-specific wait, and index publication. Reads
    // use only the separate short-lived index mutex, never this create mutex.
    const std::lock_guard creation(create_mutex_);
    const auto created = orchestrator_.CreateJob();
    if (created.ingress.code != IngressCode::kAdmitted)
      return {IngressStatus(created.ingress.code), std::nullopt};
    if (!created.job_id) return {};
    // Owner decision A, 2026-10-24: synchronously await this turn on the submitting I/O thread.
    const auto completion = orchestrator_.AwaitCompletion(created.ingress.ingress_sequence);
    if (!completion) return {};
    if (completion->code == core::internal::Completion::Code::kServiceFailed)
      return {http::JobResultCode::kServiceFailed, std::nullopt};
    if (completion->code != core::internal::Completion::Code::kSuccess) return {};
    auto job = GetJob(created.job_id->value);
    if (!job) return {};
    {
      const std::lock_guard index(index_mutex_);
      if (std::ranges::find(order_, job->job_id) == order_.end()) {
        if (order_size_ == order_.size()) throw std::logic_error("resident index exhausted");
        order_[order_size_++] = job->job_id;
      }
    }
    return {http::JobResultCode::kSuccess, std::move(job)};
  }

  [[nodiscard]] std::optional<http::JobResource> GetJob(const std::string& job_id) override {
    const auto snapshot = orchestrator_.SnapshotFor(core::Uuid{job_id});
    if (!snapshot || !snapshot->entity_exists) return std::nullopt;
    const auto state = std::string(core::ToString(snapshot->state));
    const auto terminal = core::IsTerminalState(snapshot->state);
    return http::JobResource{snapshot->job_id.value, state, terminal,
                             terminal ? std::optional<std::string>{state} : std::nullopt};
  }

  [[nodiscard]] std::vector<http::JobResource> ListJobs() override {
    std::vector<http::JobResource> result;
    const std::lock_guard index(index_mutex_);
    result.reserve(order_size_);
    for (std::size_t position = 0; position < order_size_; ++position) {
      auto job = GetJob(order_[position]);
      if (job) result.push_back(std::move(*job));
    }
    return result;
  }

  [[nodiscard]] http::JobResult CancelJob(const std::string& job_id,
                                          const std::string& principal) override {
    const auto input = orchestrator_.SubmitCommand(
        core::Command{1, core::CommandType::kCancel, core::Uuid{job_id}, principal});
    if (input.code != IngressCode::kAdmitted) return {IngressStatus(input.code), std::nullopt};
    const auto completion = orchestrator_.AwaitCompletion(input.ingress_sequence);
    if (!completion) return {};
    if (completion->code == core::internal::Completion::Code::kServiceFailed)
      return {http::JobResultCode::kServiceFailed, std::nullopt};
    if (completion->code == core::internal::Completion::Code::kReducerRejection) {
      if (!completion->rejection) return {};
      switch (completion->rejection->reason) {
        case core::RejectionReason::kJobNotFound:
          return {http::JobResultCode::kJobNotFound, std::nullopt};
        case core::RejectionReason::kStopCauseAlreadyLatched:
          return {http::JobResultCode::kStopCauseAlreadyLatched, std::nullopt};
        case core::RejectionReason::kCommandNotAllowedInState:
          return {http::JobResultCode::kCommandNotAllowedInState, std::nullopt};
        default:
          return {};
      }
    }
    auto job = GetJob(job_id);
    if (!job) return {};
    return {http::JobResultCode::kSuccess, std::move(job)};
  }

 private:
  JobOrchestrator& orchestrator_;
  const std::vector<std::string> applications_;
  std::mutex create_mutex_;
  std::mutex index_mutex_;
  std::vector<std::string> order_;
  std::size_t order_size_ = 0;
};

}  // namespace

namespace {

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

}  // namespace

std::unique_ptr<http::Backend> MakeJobBackend(JobOrchestrator& orchestrator, std::size_t max_jobs,
                                              const std::vector<core::Snapshot>& replayed,
                                              const std::vector<std::string>& applications) {
  return std::make_unique<Phase1Backend>(orchestrator, max_jobs, replayed, applications);
}

ParseResult ParseStartupOptions(const std::vector<std::string>& args) {
  ParseResult result;
  result.config.http.listen_address = "127.0.0.1";
  result.config.http.listen_port = 8080;
  result.config.max_jobs = 32;
  std::set<std::string, std::less<>> seen;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string_view flag(args[index]);
    if (flag == "--help" || flag == "-h") {
      if (args.size() != 1) {
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
    if (index + 1 >= args.size()) {
      result.error = "missing value for " + std::string(flag);
      return result;
    }
    const std::string_view value(args[++index]);
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

struct Phase1Composition::Impl {
  explicit Impl(StartupConfig value)
      : config(std::move(value)), clock(clock_read_count), identity(identity_read_count) {}

  StartupConfig config;
  std::size_t clock_read_count = 0;
  std::size_t identity_read_count = 0;
  Phase1Clock clock;
  Phase1IdentitySource identity;
  Phase1Runner runner;
  Phase1SessionRetainer session;
  std::unique_ptr<journal::SegmentJournal> journal;
  std::unique_ptr<JobOrchestrator> orchestrator;
  std::unique_ptr<http::Backend> backend;
  std::unique_ptr<http::Server> server;
  bool started = false;
};

Phase1Composition::Phase1Composition(StartupConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Phase1Composition::~Phase1Composition() { Stop(); }

bool Phase1Composition::Start(std::string& error) {
  if (impl_->started) {
    error = "composition is already started";
    return false;
  }
  if (impl_->config.journal_path.empty()) {
    error = "Journal path is required";
    return false;
  }
  try {
    impl_->journal = std::make_unique<journal::SegmentJournal>(journal::SystemFileSystem());
    const auto opened = impl_->journal->Open(impl_->config.journal_path);
    if (!opened.ok) {
      error = opened.detail;
      impl_->journal.reset();
      return false;
    }
    const auto replayed =
        journal::ReplayJournal(journal::SystemFileSystem(), impl_->config.journal_path,
                               journal::ReplayOptions{impl_->config.max_jobs});
    if (replayed.status != journal::ReplayStatus::kReplayed) {
      error = replayed.detail;
      impl_->journal.reset();
      return false;
    }
    core::internal::Config config;
    config.max_jobs = impl_->config.max_jobs;
    config.normal_capacity = 64;
    config.trace_capacity = config.total_capacity();
    config.completion_capacity = config.total_capacity();
    config.handoff_capacity = 4;
    config.ack_capacity = std::max<std::size_t>(impl_->config.max_jobs, 4);
    config.callback_registration_capacity = 16;
    config.initial_journal_sequence = replayed.next_sequence;
    config.replayed_jobs = replayed.jobs;
    config.unresolved_jobs = replayed.unresolved;
    config.ports = {&impl_->clock, impl_->journal.get(), &impl_->runner, &impl_->session,
                    &impl_->identity};
    impl_->orchestrator = std::make_unique<JobOrchestrator>(config);
    impl_->backend = MakeJobBackend(*impl_->orchestrator, config.max_jobs, replayed.jobs, {});
    impl_->server = std::make_unique<http::Server>(impl_->config.http, *impl_->backend);
    if (!impl_->server->Start(error)) {
      impl_->server.reset();
      impl_->backend.reset();
      impl_->orchestrator.reset();
      impl_->journal.reset();
      return false;
    }
    impl_->started = true;
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    Stop();
    return false;
  } catch (...) {
    error = "composition startup failed";
    Stop();
    return false;
  }
}

void Phase1Composition::Stop() noexcept {
  if (!impl_) return;
  if (impl_->server) {
    impl_->server->Stop();
    impl_->server.reset();
  }
  if (impl_->orchestrator) {
    const auto marker = impl_->orchestrator->SubmitShutdown();
    if (marker.code == IngressCode::kAdmitted) {
      (void)impl_->orchestrator->AwaitCompletion(marker.ingress_sequence);
      (void)impl_->orchestrator->BeginShutdown();
    }
    impl_->orchestrator.reset();
  }
  impl_->backend.reset();
  impl_->journal.reset();
  impl_->started = false;
}

http::Backend& Phase1Composition::backend() noexcept { return *impl_->backend; }

std::size_t Phase1Composition::clock_reads() const noexcept { return impl_->clock_read_count; }

std::size_t Phase1Composition::identity_reads() const noexcept {
  return impl_->identity_read_count;
}

std::size_t Phase1Composition::writer_turn_count() const noexcept {
  return impl_->orchestrator ? impl_->orchestrator->writer_turn_count() : 0;
}

std::uint16_t Phase1Composition::listen_port() const noexcept {
  return impl_->server ? impl_->server->port() : 0;
}

}  // namespace sitometron::daemon
