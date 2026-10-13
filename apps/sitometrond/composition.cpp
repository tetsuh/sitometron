#include "composition.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

class Phase1Backend final : public http::Backend {
 public:
  explicit Phase1Backend(JobOrchestrator& orchestrator) : orchestrator_(orchestrator) {}

  [[nodiscard]] http::Readiness GetReadiness() override {
    http::Readiness result;
    result.ready = orchestrator_.ready();
    if (orchestrator_.failed()) result.reasons.push_back({"service_failed", {}});
    std::vector<std::string> unresolved;
    for (const auto& id : orchestrator_.unresolved()) unresolved.push_back(id.value);
    if (!unresolved.empty()) result.reasons.push_back({"unresolved_jobs", std::move(unresolved)});
    return result;
  }

 private:
  JobOrchestrator& orchestrator_;
};

}  // namespace

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
  std::unique_ptr<Phase1Backend> backend;
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
    impl_->backend = std::make_unique<Phase1Backend>(*impl_->orchestrator);
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

std::uint16_t Phase1Composition::listen_port() const noexcept {
  return impl_->server ? impl_->server->port() : 0;
}

}  // namespace sitometron::daemon
