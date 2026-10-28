#ifndef SITOMETRON_TESTS_SUPPORT_IDENTITY_TEST_SUPPORT_HPP_
#define SITOMETRON_TESTS_SUPPORT_IDENTITY_TEST_SUPPORT_HPP_

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "core_fakes.hpp"
#include "job_orchestrator.hpp"
#include "sitometron/identity/identity_source.hpp"
#include "sitometron/identity/system_clock.hpp"

namespace sitometron::test::identity_checks {

namespace core = sitometron::core;
namespace adapter = sitometron::identity;
namespace internal = core::internal;

inline constexpr std::string_view kJob = "01890f30-7b54-7cc3-98c4-dc0c0c07398f";
inline constexpr std::string_view kFresh = "01890f30-7b54-7cc3-98c4-dc0c0c073990";
inline constexpr std::string_view kWorker = "550e8400-e29b-41d4-a716-446655440000";

class AssertionFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline void Fail(std::string_view name, std::string_view message,
                              std::source_location where = std::source_location::current()) {
  throw AssertionFailure(std::string(where.file_name()) + ":" + std::to_string(where.line()) +
                         ": " + std::string(name) + ": " + std::string(message));
}

inline void Require(bool condition, std::string_view name, std::string_view message,
                    std::source_location where = std::source_location::current()) {
  if (!condition) Fail(name, message, where);
}

inline bool Canonical(std::string_view value, char version) {
  if (value.size() != 36 || value[14] != version ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b'))
    return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

// Test-only parsing, not UUID generation. The independent oracle deliberately accepts no
// extensions.
inline adapter::UuidBytes Bytes(std::string_view text) {
  Require(Canonical(text, '7') || Canonical(text, '4'), "script setup", "noncanonical UUID");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  adapter::UuidBytes value;
  std::size_t byte = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '-') {
      ++i;
    } else {
      value.bytes[byte++] = static_cast<std::uint8_t>(nibble(text[i]) * 16 + nibble(text[i + 1]));
      i += 2;
    }
  }
  return value;
}

enum class Kind { kJob, kWorker, kLaunch };

inline std::optional<std::string> Generate(core::IdentitySourcePort& source, Kind kind) {
  const auto text = [](const auto& result) -> std::optional<std::string> {
    return std::visit(
        [](const auto& value) -> std::optional<std::string> {
          if constexpr (requires { value.value.value; }) {
            return value.value.value;
          } else {
            return std::nullopt;
          }
        },
        result);
  };
  switch (kind) {
    case Kind::kJob:
      return text(source.GenerateJobSessionIdentity());
    case Kind::kWorker:
      return text(source.GenerateWorkerIdentity());
    case Kind::kLaunch:
      return text(source.GenerateLaunchOperationIdentity());
  }
  throw std::logic_error("invalid test generation kind");
}

struct Script {
  std::vector<adapter::UuidBytes> values;
  std::size_t calls = 0;
  adapter::UuidBytes Next() {
    if (calls >= values.size()) throw std::logic_error("generator script over-read");
    return values[calls++];
  }
};

template <class Interface>
class ScriptedGenerator final : public Interface {
 public:
  explicit ScriptedGenerator(std::shared_ptr<Script> script) : script_(std::move(script)) {}
  adapter::UuidBytes Generate() override { return script_->Next(); }

 private:
  std::shared_ptr<Script> script_;
};

struct ScriptedSource {
  std::shared_ptr<Script> v7 = std::make_shared<Script>();
  std::shared_ptr<Script> v4 = std::make_shared<Script>();
  adapter::ProductionIdentitySource source;
  explicit ScriptedSource(std::size_t capacity = 32)
      : source(std::make_unique<ScriptedGenerator<adapter::UuidV7Generator>>(v7),
               std::make_unique<ScriptedGenerator<adapter::UuidV4Generator>>(v4), capacity) {}
  Script& For(Kind kind) { return kind == Kind::kWorker ? *v4 : *v7; }
};

struct SampleState {
  adapter::ClockSample value;
  std::size_t calls = 0;
};

class ScriptedSampler final : public adapter::ClockSampler {
 public:
  explicit ScriptedSampler(std::shared_ptr<SampleState> state) : state_(std::move(state)) {}
  adapter::ClockSample Sample() override {
    ++state_->calls;
    return state_->value;
  }

 private:
  std::shared_ptr<SampleState> state_;
};

inline core::ClockReading ReadClock(core::ClockPort& clock, std::string_view name,
                                    std::source_location where = std::source_location::current()) {
  try {
    return clock.Read();
  } catch (const std::exception& error) {
    Require(false, name, std::string("valid clock sample threw: ") + error.what(), where);
    throw;
  }
}

inline bool ReadThrows(core::ClockPort& clock) {
  try {
    (void)clock.Read();
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

class CountingClock final : public core::ClockPort {
 public:
  explicit CountingClock(core::ClockPort& clock) : clock_(clock) {}
  core::ClockReading Read() override {
    ++calls;
    return clock_.Read();
  }
  std::size_t calls = 0;

 private:
  core::ClockPort& clock_;
};

class RecordingJournal final : public core::JobJournalPort {
 public:
  core::LogicalCommitResult Commit(const core::LogicalJobEvent& event) noexcept override {
    if (count == events.size()) return core::LogicalCommitResult::kDefiniteFailure;
    try {
      events[count].emplace(event);
      ++count;
      return core::LogicalCommitResult::kCommitted;
    } catch (...) {
      return core::LogicalCommitResult::kDefiniteFailure;
    }
  }
  std::array<std::optional<core::LogicalJobEvent>, 16> events;
  std::size_t count = 0;
};

class CoreFixture final {
 public:
  CoreFixture(core::IdentitySourcePort& source, core::ClockPort& clock) {
    internal::Config config;
    config.max_jobs = 4;
    config.normal_capacity = 4;
    config.trace_capacity = 512;
    config.completion_capacity = 128;
    config.handoff_capacity = 16;
    config.ack_capacity = 32;
    config.callback_registration_capacity = 16;
    config.ports = {&clock, &journal, &runner, &session, &source};
    writer = std::make_unique<internal::JobOrchestrator>(config);
  }
  RecordingJournal journal;
  FakeApplicationRunner runner{{}, 0};
  FakeSessionRetainer session{{}, 0};
  std::unique_ptr<internal::JobOrchestrator> writer;
};

class ScopedDirectory final {
 public:
  ScopedDirectory() {
    static std::atomic_size_t serial{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("issue115-identity-" + std::to_string(tick) + "-" + std::to_string(serial.fetch_add(1)));
      if (std::filesystem::create_directory(candidate)) {
        path_ = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("could not create an owned test directory");
  }
  ~ScopedDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void IdentitySeedFailureRefusesStartup();
void SitometrondComposesProductionIdentity();

}  // namespace sitometron::test::identity_checks

#endif  // SITOMETRON_TESTS_SUPPORT_IDENTITY_TEST_SUPPORT_HPP_
