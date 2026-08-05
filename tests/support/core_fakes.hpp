#ifndef SITOMETRON_TESTS_SUPPORT_CORE_FAKES_HPP_
#define SITOMETRON_TESTS_SUPPORT_CORE_FAKES_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::test {

class FakeClock final : public core::ClockPort {
 public:
  explicit FakeClock(core::ClockReading reading);

  [[nodiscard]] core::ClockReading Read() override;
  void SetDiagnostic(core::DiagnosticTimestamp timestamp);
  [[nodiscard]] bool AdvanceMonotonic(std::uint64_t delta) noexcept;
  [[nodiscard]] bool verification_failed() const noexcept;

 private:
  core::ClockReading reading_;
  bool verification_failed_ = false;
};

struct JournalExpectation {
  core::LogicalJobEvent event;
  core::LogicalCommitResult result = core::LogicalCommitResult::kOutcomeUnknown;
  bool fail_observation = false;
};

class FakeJobJournal final : public core::JobJournalPort {
 public:
  explicit FakeJobJournal(std::vector<JournalExpectation> expectations);

  [[nodiscard]] core::LogicalCommitResult Commit(
      const core::LogicalJobEvent& event) noexcept override;
  [[nodiscard]] std::vector<core::LogicalJobEvent> CopyObservations() const;
  [[nodiscard]] std::size_t remaining_expectations() const noexcept;
  [[nodiscard]] bool verification_failed() const noexcept;
  [[nodiscard]] bool Verify() noexcept;

 private:
  std::vector<JournalExpectation> expectations_;
  std::vector<std::optional<core::LogicalJobEvent>> observations_;
  std::size_t next_expectation_ = 0;
  std::size_t observation_count_ = 0;
  bool verification_failed_ = false;
};

enum class RunnerCallKind { kLaunch, kCooperativeStop, kForcedStop };
using RunnerRequest = std::variant<core::ApplicationLaunchRequest, core::ApplicationStopRequest>;

struct RunnerExpectation {
  RunnerCallKind kind = RunnerCallKind::kLaunch;
  RunnerRequest expected;
  std::optional<core::RawCandidateEvent> candidate;
};

struct RunnerObservation {
  RunnerCallKind kind = RunnerCallKind::kLaunch;
  RunnerRequest request;
};

[[nodiscard]] RunnerExpectation ExpectLaunch(core::ApplicationLaunchRequest expected, bool started);
[[nodiscard]] RunnerExpectation ExpectLaunchProcessAlreadyExited(
    core::ApplicationLaunchRequest expected);
[[nodiscard]] RunnerExpectation ExpectCooperativeStop(core::ApplicationStopRequest expected);
[[nodiscard]] RunnerExpectation ExpectForcedStop(core::ApplicationStopRequest expected);

class FakeApplicationRunner final : public core::ApplicationRunnerPort {
 public:
  FakeApplicationRunner(std::vector<RunnerExpectation> expectations,
                        std::size_t candidate_capacity);

  void HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept override;
  void HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept override;
  void HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept override;

  [[nodiscard]] std::vector<RunnerObservation> CopyObservations() const;
  [[nodiscard]] std::optional<core::RawCandidateEvent> TakeNextCandidate();
  [[nodiscard]] bool CancelNextCandidate() noexcept;
  [[nodiscard]] std::size_t discarded_request_count() const noexcept;
  [[nodiscard]] bool verification_failed() const noexcept;
  [[nodiscard]] bool Verify() noexcept;

 private:
  void HandleLaunch(core::ApplicationLaunchRequest&& request) noexcept;
  void HandleStop(RunnerCallKind kind, core::ApplicationStopRequest&& request) noexcept;
  void StageCandidate(std::optional<core::RawCandidateEvent>& candidate) noexcept;

  std::vector<RunnerExpectation> expectations_;
  std::vector<std::optional<RunnerObservation>> observations_;
  std::vector<std::optional<core::RawCandidateEvent>> candidates_;
  std::size_t next_expectation_ = 0;
  std::size_t observation_count_ = 0;
  std::size_t candidate_write_ = 0;
  std::size_t candidate_read_ = 0;
  std::size_t pending_candidates_ = 0;
  std::size_t discarded_request_count_ = 0;
  bool verification_failed_ = false;
};

struct SessionExpectation {
  core::SessionRetainRequest expected;
  std::optional<core::RawCandidateEvent> candidate;
};

[[nodiscard]] SessionExpectation ExpectSessionRetain(core::SessionRetainRequest expected);

class FakeSessionRetainer final : public core::SessionRetainerPort {
 public:
  FakeSessionRetainer(std::vector<SessionExpectation> expectations, std::size_t candidate_capacity);

  void HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept override;

  [[nodiscard]] std::vector<core::SessionRetainRequest> CopyObservations() const;
  [[nodiscard]] std::optional<core::RawCandidateEvent> TakeNextCandidate();
  [[nodiscard]] bool CancelNextCandidate() noexcept;
  [[nodiscard]] std::size_t discarded_request_count() const noexcept;
  [[nodiscard]] bool verification_failed() const noexcept;
  [[nodiscard]] bool Verify() noexcept;

 private:
  std::vector<SessionExpectation> expectations_;
  std::vector<std::optional<core::SessionRetainRequest>> observations_;
  std::vector<std::optional<core::RawCandidateEvent>> candidates_;
  std::size_t next_expectation_ = 0;
  std::size_t observation_count_ = 0;
  std::size_t candidate_write_ = 0;
  std::size_t candidate_read_ = 0;
  std::size_t pending_candidates_ = 0;
  std::size_t discarded_request_count_ = 0;
  bool verification_failed_ = false;
};

class FakeIdentitySource final : public core::IdentitySourcePort {
 public:
  FakeIdentitySource(std::vector<core::JobSessionIdentityResult> job_session_script,
                     std::vector<core::WorkerIdentityResult> worker_script,
                     std::vector<core::LaunchOperationIdentityResult> launch_script);

  [[nodiscard]] core::JobSessionIdentityResult GenerateJobSessionIdentity() override;
  [[nodiscard]] core::WorkerIdentityResult GenerateWorkerIdentity() override;
  [[nodiscard]] core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override;

  [[nodiscard]] bool verification_failed() const noexcept;
  [[nodiscard]] bool Verify() noexcept;

 private:
  std::vector<core::JobSessionIdentityResult> job_session_script_;
  std::vector<core::WorkerIdentityResult> worker_script_;
  std::vector<core::LaunchOperationIdentityResult> launch_script_;
  std::size_t next_job_session_ = 0;
  std::size_t next_worker_ = 0;
  std::size_t next_launch_ = 0;
  bool verification_failed_ = false;
};

struct EffectObservation {
  std::uint64_t ordinal = 0;
  core::EffectId effect_id = core::EffectId::kInvalid;
};

class PassiveEffectObserver {
 public:
  explicit PassiveEffectObserver(std::size_t capacity);

  [[nodiscard]] bool Observe(core::EffectId effect_id) noexcept;
  [[nodiscard]] std::vector<EffectObservation> CopyObservations() const;
  [[nodiscard]] bool verification_failed() const noexcept;

 private:
  std::vector<std::optional<EffectObservation>> observations_;
  std::size_t observation_count_ = 0;
  bool verification_failed_ = false;
};

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_CORE_FAKES_HPP_
