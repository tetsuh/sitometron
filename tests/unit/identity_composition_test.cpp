#include <cstddef>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "apps/sitometrond/entrypoint.hpp"
#include "identity_test_support.hpp"

#ifdef SITOMETRON_IDENTITY_TEST_HTTP
#include "apps/sitometrond/composition.hpp"
#endif

namespace sitometron::test::identity_checks {
namespace {

std::size_t g_factory_calls = 0;
std::size_t g_live_v7 = 0;
unsigned g_failure_stage = 0;

class SeededV7 final : public adapter::UuidV7Generator {
 public:
  SeededV7() {
    if (g_failure_stage == 0) throw std::runtime_error("scripted v7 seed failure");
    ++g_live_v7;
  }
  ~SeededV7() override { --g_live_v7; }
  adapter::UuidBytes Generate() override { throw std::logic_error("failed factory was used"); }
};

class SeededV4 final : public adapter::UuidV4Generator {
 public:
  SeededV4() {
    if (g_failure_stage == 1) throw std::runtime_error("scripted v4 seed failure");
    throw 17;
  }
  adapter::UuidBytes Generate() override { throw std::logic_error("failed factory was used"); }
};

std::unique_ptr<core::IdentitySourcePort> FailingFactory(std::size_t capacity) {
  ++g_factory_calls;
  auto v7 = std::make_unique<SeededV7>();
  auto v4 = std::make_unique<SeededV4>();
  return std::make_unique<adapter::ProductionIdentitySource>(std::move(v7), std::move(v4),
                                                             capacity);
}

std::unique_ptr<core::IdentitySourcePort> CountedProductionFactory(std::size_t capacity) {
  ++g_factory_calls;
  return adapter::MakeProductionIdentitySource(capacity);
}

class OutputCapture final {
 public:
  OutputCapture()
      : old_out_(std::cout.rdbuf(out_.rdbuf())), old_err_(std::cerr.rdbuf(err_.rdbuf())) {}
  ~OutputCapture() {
    std::cout.rdbuf(old_out_);
    std::cerr.rdbuf(old_err_);
  }
  OutputCapture(const OutputCapture&) = delete;
  OutputCapture& operator=(const OutputCapture&) = delete;
  std::string output() const { return out_.str(); }
  std::string error() const { return err_.str(); }

 private:
  std::ostringstream out_;
  std::ostringstream err_;
  std::streambuf* old_out_;
  std::streambuf* old_err_;
};

int EntryStatus(const std::vector<std::string>& args, daemon::IdentityFactory factory,
                std::string_view name) {
  try {
    return daemon::RunSitometrond(args, factory);
  } catch (...) {
    Require(false, name,
            "entrypoint let a startup exception escape instead of returning its status");
    throw;
  }
}

void RealPortCommit(std::string_view name) {
  auto source = adapter::MakeProductionIdentitySource();
  auto clock = adapter::MakeProductionClock();
  CoreFixture fixture(*source, *clock);
  std::string id;
#ifdef SITOMETRON_IDENTITY_TEST_HTTP
  auto backend = daemon::MakeJobBackend(*fixture.writer, 4, {}, {"issue115-test"});
  const auto result = backend->CreateJob("issue115-test");
  Require(
      result.code == sitometron::http::JobResultCode::kSuccess && result.job.has_value(), name,
      "registered test-only Backend create did not commit with production identity/clock ports");
  id = result.job->job_id;
#else
  // The HTTP-off build has no Backend or Application registry; this is a test-only core fixture.
  const auto result = fixture.writer->CreateJob();
  Require(result.ingress.code == internal::IngressCode::kAdmitted && result.job_id.has_value(),
          name, "HTTP-off test-only create did not admit with the production identity source");
  const auto completion = fixture.writer->AwaitCompletion(result.ingress.ingress_sequence);
  Require(completion && completion->code == internal::Completion::Code::kSuccess, name,
          "HTTP-off test-only create did not commit with the production clock");
  id = result.job_id->value;
#endif
  Require(Canonical(id, '7') && fixture.journal.count == 1, name,
          "create did not commit exactly one canonical UUIDv7 Job");
  const auto& event = *fixture.journal.events[0];
  const auto* payload = std::get_if<core::JobCreatedPayload>(&event.payload);
  Require(event.job_id.value == id && payload && payload->session_id == event.job_id, name,
          "Job and Session did not reuse the same generated value");
  Require(event.recorded_at.rfc3339.size() == 24 && event.recorded_at.rfc3339.back() == 'Z', name,
          "production clock did not supply a UTC millisecond diagnostic timestamp");
  const auto worker = fixture.writer->GeneratedWorker(core::Uuid{id});
  const auto launch = fixture.writer->GeneratedLaunch(core::Uuid{id});
  Require(worker && Canonical(worker->value, '4') && launch && Canonical(launch->value, '7'), name,
          "creation did not retain production Worker/launch identities");
  Require(fixture.runner.Verify() && fixture.session.Verify(), name,
          "test create unexpectedly reached the runner or Session retainer");
}

}  // namespace

void IdentitySeedFailureRefusesStartup() {
  constexpr std::string_view name = "identity_seed_failure_refuses_startup";
  for (unsigned stage = 0; stage < 3; ++stage) {
    g_failure_stage = stage;
    g_factory_calls = 0;
    Require(g_live_v7 == 0, name, "prior partial generator construction leaked");
    const ScopedDirectory directory;
    const std::vector<std::string> args{"--journal", directory.path().string(), "--listen",
                                        "127.0.0.1:0"};
#ifdef SITOMETRON_IDENTITY_TEST_HTTP
    daemon::StartupConfig config;
    config.journal_path = directory.path().string();
    config.http.listen_port = 0;
    daemon::Phase1Composition composition(config, FailingFactory);
    std::string error;
    const bool started = composition.Start(error);
    Require(!started, name, "generator seed failure did not refuse production startup");
    Require(!error.empty() && composition.listen_port() == 0 && g_factory_calls == 1, name,
            "seed refusal did not invoke the factory exactly once before opening a listener");
    composition.Stop();
    Require(g_live_v7 == 0, name,
            "failed second-generator construction leaked the first generator");
    g_factory_calls = 0;
#endif
    {
      OutputCapture capture;
      const auto status = EntryStatus(args, FailingFactory, name);
      Require(status == 1 && g_factory_calls == 1 && g_live_v7 == 0, name,
              "seed refusal did not return status 1 and clean up partial construction");
      Require(capture.output().find("listening") == std::string::npos && !capture.error().empty(),
              name, "seed refusal announced a listener or omitted its startup diagnostic");
    }
  }
#ifdef SITOMETRON_IDENTITY_TEST_HTTP
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"--unknown"}, std::vector<std::string>{"--journal"}}) {
    g_factory_calls = 0;
    OutputCapture capture;
    Require(EntryStatus(args, FailingFactory, name) == 2 && g_factory_calls == 0, name,
            "malformed CLI did not return 2 before generator construction");
  }
#endif
}

void SitometrondComposesProductionIdentity() {
  constexpr std::string_view name = "sitometrond_composes_production_identity";
  g_factory_calls = 0;
#ifdef SITOMETRON_IDENTITY_TEST_HTTP
  const ScopedDirectory directory;
  daemon::StartupConfig config;
  config.journal_path = directory.path().string();
  config.http.listen_port = 0;
  daemon::Phase1Composition composition(config, CountedProductionFactory);
  std::string error;
  Require(composition.Start(error), name,
          "production composition did not start on an ephemeral port");
  Require(g_factory_calls == 1, name,
          "production composition did not construct its real identity source");
  const auto create = composition.backend().CreateJob("issue115-test");
  Require(create.code == sitometron::http::JobResultCode::kUnknownApplication && !create.job &&
              composition.identity_reads() == 0 && composition.clock_reads() == 0 &&
              composition.writer_turn_count() == 0,
          name, "production empty-registry create reached either port or ingress");
  const auto cancel = composition.backend().CancelJob(std::string(kJob), "issue115-operator");
  Require(cancel.code == sitometron::http::JobResultCode::kJobNotFound &&
              composition.identity_reads() == 0 && composition.clock_reads() == 0,
          name, "production unknown cancel did not reject before either port");
  composition.Stop();
#else
  {
    OutputCapture capture;
    Require(EntryStatus({}, CountedProductionFactory, name) == 0 && g_factory_calls == 1, name,
            "HTTP-off startup did not construct real ports before returning status 0");
    Require(capture.output().starts_with("sitometrond ") &&
                capture.output().find("listening") == std::string::npos,
            name, "HTTP-off startup changed its version-only output");
  }
#endif
  RealPortCommit(name);
}

}  // namespace sitometron::test::identity_checks
