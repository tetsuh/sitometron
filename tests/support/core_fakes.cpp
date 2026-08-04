#include "core_fakes.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sitometron::test {
namespace {

bool EqualPayload(const core::EmptyPayload&, const core::EmptyPayload&) { return true; }
bool EqualPayload(const core::JobCreatedPayload& lhs, const core::JobCreatedPayload& rhs) {
  return lhs.session_id == rhs.session_id;
}
bool EqualPayload(const core::ResourcesCommittedPayload& lhs,
                  const core::ResourcesCommittedPayload& rhs) {
  return lhs.allocation_id == rhs.allocation_id && lhs.allocation_digest == rhs.allocation_digest &&
         lhs.schema_id == rhs.schema_id && lhs.schema_version == rhs.schema_version &&
         lhs.payload_utf8 == rhs.payload_utf8;
}
bool EqualPayload(const core::WorkerLaunchIntentPayload& lhs,
                  const core::WorkerLaunchIntentPayload& rhs) noexcept {
  return lhs.operation_id == rhs.operation_id && lhs.application_id == rhs.application_id &&
         lhs.application_version == rhs.application_version &&
         lhs.bundle_sha256 == rhs.bundle_sha256 && lhs.allocation_id == rhs.allocation_id &&
         lhs.allocation_digest == rhs.allocation_digest && lhs.worker_id == rhs.worker_id;
}
bool EqualPayload(const core::WorkerLaunchObservedPayload& lhs,
                  const core::WorkerLaunchObservedPayload& rhs) {
  return lhs.operation_id == rhs.operation_id && lhs.started == rhs.started;
}
bool EqualPayload(const core::WorkerRunningPayload& lhs, const core::WorkerRunningPayload& rhs) {
  return lhs.worker_id == rhs.worker_id;
}
bool EqualPayload(const core::PrincipalPayload& lhs, const core::PrincipalPayload& rhs) {
  return lhs.principal_subject == rhs.principal_subject;
}
bool EqualPayload(const core::TimeoutExpiredPayload& lhs, const core::TimeoutExpiredPayload& rhs) {
  return lhs.phase == rhs.phase && lhs.timer_generation == rhs.timer_generation;
}
bool EqualPayload(const core::WorkerEventPayload& lhs, const core::WorkerEventPayload& rhs) {
  return lhs.worker_id == rhs.worker_id && lhs.event_sequence == rhs.event_sequence;
}
bool EqualPayload(const core::ProcessExitConfirmedPayload& lhs,
                  const core::ProcessExitConfirmedPayload& rhs) {
  return lhs.completion_mode == rhs.completion_mode &&
         lhs.launch_operation_id == rhs.launch_operation_id;
}
bool EqualPayload(const core::SessionPayload& lhs, const core::SessionPayload& rhs) {
  return lhs.session_id == rhs.session_id;
}
bool EqualPayload(const core::TerminalOutcomePayload& lhs,
                  const core::TerminalOutcomePayload& rhs) {
  return lhs.outcome == rhs.outcome;
}
bool EqualPayload(const core::ResourcesReleasedPayload& lhs,
                  const core::ResourcesReleasedPayload& rhs) {
  return lhs.allocation_id == rhs.allocation_id && lhs.allocation_digest == rhs.allocation_digest;
}
bool EqualPayload(const core::CleanupStatusPayload& lhs, const core::CleanupStatusPayload& rhs) {
  return lhs.status == rhs.status;
}
bool EqualPayload(const core::LateWorkerEventPayload& lhs,
                  const core::LateWorkerEventPayload& rhs) {
  return lhs.original_event_type == rhs.original_event_type && lhs.worker_id == rhs.worker_id &&
         lhs.event_sequence == rhs.event_sequence;
}

bool EqualEventPayload(const core::EventPayload& lhs, const core::EventPayload& rhs) {
  if (lhs.index() != rhs.index()) {
    return false;
  }
  return std::visit(
      [&rhs](const auto& value) {
        return EqualPayload(value, std::get<std::decay_t<decltype(value)>>(rhs));
      },
      lhs);
}

bool EqualLogicalJobEvent(const core::LogicalJobEvent& lhs, const core::LogicalJobEvent& rhs) {
  return lhs.schema_version == rhs.schema_version && lhs.sequence == rhs.sequence &&
         lhs.event_type == rhs.event_type && lhs.recorded_at == rhs.recorded_at &&
         lhs.job_id == rhs.job_id && EqualEventPayload(lhs.payload, rhs.payload);
}

bool EqualLaunchRequest(const core::ApplicationLaunchRequest& lhs,
                        const core::ApplicationLaunchRequest& rhs) noexcept {
  return lhs.job_id == rhs.job_id && EqualPayload(lhs.intent, rhs.intent);
}

bool IsLowerHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool IsUuidVersion(std::string_view value, char version) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
      value[23] != '-' || value[14] != version ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b')) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      continue;
    }
    if (!IsLowerHex(value[index])) {
      return false;
    }
  }
  return true;
}

bool IsAsciiDigit(char value) { return value >= '0' && value <= '9'; }

bool IsAsciiAlphanumeric(char value) {
  return IsAsciiDigit(value) || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool IsStableId(std::string_view value) {
  if (value.empty() || value.size() > 128 || !IsAsciiAlphanumeric(value[0])) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return IsAsciiAlphanumeric(character) || character == '.' || character == '_' ||
           character == ':' || character == '-';
  });
}

bool IsDigest(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), IsLowerHex);
}

bool IsUtf8ScalarString(std::string_view value, std::size_t max_scalars) {
  if (value.empty()) {
    return false;
  }
  std::size_t scalars = 0;
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xBF;
    if (first <= 0x7F) {
      length = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
      length = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
      length = 3;
      if (first == 0xE0) second_min = 0xA0;
      if (first == 0xED) second_max = 0x9F;
    } else if (first >= 0xF0 && first <= 0xF4) {
      length = 4;
      if (first == 0xF0) second_min = 0x90;
      if (first == 0xF4) second_max = 0x8F;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    if (length > 1) {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if (second < second_min || second > second_max) {
        return false;
      }
      for (std::size_t continuation = 2; continuation < length; ++continuation) {
        const auto byte = static_cast<unsigned char>(value[index + continuation]);
        if (byte < 0x80 || byte > 0xBF) {
          return false;
        }
      }
    }
    index += length;
    if (++scalars > max_scalars) {
      return false;
    }
  }
  return true;
}

int ParseDigits(std::string_view value, std::size_t offset, std::size_t count) {
  int result = 0;
  if (offset + count > value.size()) {
    return -1;
  }
  for (std::size_t index = offset; index < offset + count; ++index) {
    if (!IsAsciiDigit(value[index])) {
      return -1;
    }
    result = result * 10 + (value[index] - '0');
  }
  return result;
}

bool IsRfc3339(std::string_view value) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
      (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':') {
    return false;
  }
  const int year = ParseDigits(value, 0, 4);
  const int month = ParseDigits(value, 5, 2);
  const int day = ParseDigits(value, 8, 2);
  const int hour = ParseDigits(value, 11, 2);
  const int minute = ParseDigits(value, 14, 2);
  const int second = ParseDigits(value, 17, 2);
  constexpr int kDaysByMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year <= 0 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60) {
    return false;
  }
  int max_day = kDaysByMonth[month];
  if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) {
    max_day = 29;
  }
  if (day < 1 || day > max_day) {
    return false;
  }
  std::size_t zone = 19;
  if (value[zone] == '.') {
    ++zone;
    const std::size_t fraction_start = zone;
    while (zone < value.size() && IsAsciiDigit(value[zone])) {
      ++zone;
    }
    if (zone == fraction_start) {
      return false;
    }
  }
  if (zone >= value.size()) {
    return false;
  }
  if (value[zone] == 'Z' || value[zone] == 'z') {
    return zone + 1 == value.size();
  }
  if ((value[zone] != '+' && value[zone] != '-') || zone + 6 != value.size() ||
      value[zone + 3] != ':') {
    return false;
  }
  const int offset_hour = ParseDigits(value, zone + 1, 2);
  const int offset_minute = ParseDigits(value, zone + 4, 2);
  return offset_hour >= 0 && offset_hour <= 23 && offset_minute >= 0 && offset_minute <= 59;
}

bool IsLogicalCommitResult(core::LogicalCommitResult result) {
  return result == core::LogicalCommitResult::kCommitted ||
         result == core::LogicalCommitResult::kDefiniteFailure ||
         result == core::LogicalCommitResult::kOutcomeUnknown;
}

bool IsValidLogicalJobEvent(const core::LogicalJobEvent& event) {
  if (event.schema_version != 1 || event.sequence == 0 || !IsRfc3339(event.recorded_at.rfc3339) ||
      !IsUuidVersion(event.job_id.value, '7')) {
    return false;
  }

  switch (event.event_type) {
    case core::EventType::kJobCreated:
      return std::holds_alternative<core::JobCreatedPayload>(event.payload);
    case core::EventType::kResourcesCommitted:
      return std::holds_alternative<core::ResourcesCommittedPayload>(event.payload);
    case core::EventType::kWorkerLaunchIntent:
      return std::holds_alternative<core::WorkerLaunchIntentPayload>(event.payload);
    case core::EventType::kWorkerLaunchObserved:
      return std::holds_alternative<core::WorkerLaunchObservedPayload>(event.payload);
    case core::EventType::kWorkerRunning:
      return std::holds_alternative<core::WorkerRunningPayload>(event.payload);
    case core::EventType::kCancelAccepted:
    case core::EventType::kTerminateAccepted:
      return std::holds_alternative<core::PrincipalPayload>(event.payload);
    case core::EventType::kTimeoutExpired:
      return std::holds_alternative<core::TimeoutExpiredPayload>(event.payload);
    case core::EventType::kWorkerCompleted:
    case core::EventType::kWorkerFailed:
      return std::holds_alternative<core::WorkerEventPayload>(event.payload);
    case core::EventType::kProcessExitConfirmed:
      return std::holds_alternative<core::ProcessExitConfirmedPayload>(event.payload);
    case core::EventType::kSessionRetainRequested:
    case core::EventType::kSessionRetained:
      return std::holds_alternative<core::SessionPayload>(event.payload);
    case core::EventType::kFinalizationCompleted:
    case core::EventType::kFinalizationFailed:
      return std::holds_alternative<core::EmptyPayload>(event.payload);
    case core::EventType::kTerminalOutcomeCommitted:
      return std::holds_alternative<core::TerminalOutcomePayload>(event.payload);
    case core::EventType::kResourcesReleased:
      return std::holds_alternative<core::ResourcesReleasedPayload>(event.payload);
    case core::EventType::kCleanupStatusRecorded:
      return std::holds_alternative<core::CleanupStatusPayload>(event.payload);
    case core::EventType::kLateWorkerEvent:
      return std::holds_alternative<core::LateWorkerEventPayload>(event.payload);
    case core::EventType::kInvalid:
      return false;
  }
  return false;
}

template <typename Request>
void ConsumeTransferredRequest(Request&& request) noexcept {
  using Value = std::decay_t<Request>;
  static_assert(std::is_nothrow_move_constructible_v<Value>);
  static_assert(std::is_nothrow_destructible_v<Value>);
  [[maybe_unused]] Value discarded(std::move(request));
}

core::RawCandidateEvent MakeLaunchObservedCandidate(const core::ApplicationLaunchRequest& request,
                                                    bool started) {
  return core::RawCandidateEvent{1, request.job_id, "worker_launch_observed",
                                 "{\"operation_id\":\"" + request.intent.operation_id.value +
                                     "\",\"outcome\":\"" + (started ? "started" : "failed") +
                                     "\"}"};
}

std::string_view CompletionModeText(core::CompletionMode completion_mode) {
  switch (completion_mode) {
    case core::CompletionMode::kCooperative:
      return "cooperative";
    case core::CompletionMode::kForced:
      return "forced";
    case core::CompletionMode::kProcessAlreadyExited:
      return "process_already_exited";
    default:
      return "invalid";
  }
}

core::RawCandidateEvent MakeProcessExitCandidate(const core::Uuid& job_id,
                                                 const core::StableId& operation_id,
                                                 core::CompletionMode completion_mode) {
  return core::RawCandidateEvent{1, job_id, "process_exit_confirmed",
                                 "{\"completion_mode\":\"" +
                                     std::string(CompletionModeText(completion_mode)) +
                                     "\",\"launch_operation_id\":\"" + operation_id.value + "\"}"};
}

core::RawCandidateEvent MakeSessionRetainedCandidate(const core::SessionRetainRequest& request) {
  return core::RawCandidateEvent{1, request.job_id, "session_retained",
                                 "{\"session_id\":\"" + request.session_id.value + "\"}"};
}

bool EqualRawCandidate(const core::RawCandidateEvent& lhs, const core::RawCandidateEvent& rhs) {
  return lhs.schema_version == rhs.schema_version && lhs.job_id == rhs.job_id &&
         lhs.event_type == rhs.event_type && lhs.payload_json == rhs.payload_json;
}

bool IsValidLaunchRequest(const core::ApplicationLaunchRequest& request) {
  return IsUuidVersion(request.job_id.value, '7') &&
         IsStableId(request.intent.operation_id.value) &&
         IsStableId(request.intent.application_id.value) &&
         IsUtf8ScalarString(request.intent.application_version, 128) &&
         IsDigest(request.intent.bundle_sha256.value) &&
         IsStableId(request.intent.allocation_id.value) &&
         IsDigest(request.intent.allocation_digest.value) &&
         IsUuidVersion(request.intent.worker_id.value, '4');
}

bool IsValidStopRequest(const core::ApplicationStopRequest& request) {
  return IsUuidVersion(request.job_id.value, '7') &&
         IsStableId(request.launch_operation_id.value) &&
         IsUuidVersion(request.worker_id.value, '4');
}

bool IsValidRunnerExpectation(const RunnerExpectation& expectation) {
  if (expectation.kind == RunnerCallKind::kLaunch) {
    const auto* request = std::get_if<core::ApplicationLaunchRequest>(&expectation.expected);
    if (request == nullptr || !IsValidLaunchRequest(*request)) {
      return false;
    }
    if (!expectation.candidate.has_value()) {
      return true;
    }
    const auto& candidate = *expectation.candidate;
    return EqualRawCandidate(candidate, MakeLaunchObservedCandidate(*request, true)) ||
           EqualRawCandidate(candidate, MakeLaunchObservedCandidate(*request, false)) ||
           EqualRawCandidate(candidate,
                             MakeProcessExitCandidate(request->job_id, request->intent.operation_id,
                                                      core::CompletionMode::kProcessAlreadyExited));
  }

  if (expectation.kind != RunnerCallKind::kCooperativeStop &&
      expectation.kind != RunnerCallKind::kForcedStop) {
    return false;
  }
  const auto* request = std::get_if<core::ApplicationStopRequest>(&expectation.expected);
  if (request == nullptr || !IsValidStopRequest(*request)) {
    return false;
  }
  if (!expectation.candidate.has_value()) {
    return true;
  }
  const auto completion_mode = expectation.kind == RunnerCallKind::kCooperativeStop
                                   ? core::CompletionMode::kCooperative
                                   : core::CompletionMode::kForced;
  return EqualRawCandidate(
      *expectation.candidate,
      MakeProcessExitCandidate(request->job_id, request->launch_operation_id, completion_mode));
}

bool IsValidSessionExpectation(const SessionExpectation& expectation) {
  return IsUuidVersion(expectation.expected.job_id.value, '7') &&
         IsUuidVersion(expectation.expected.session_id.value, '7') &&
         expectation.expected.job_id == expectation.expected.session_id &&
         (!expectation.candidate.has_value() ||
          EqualRawCandidate(*expectation.candidate,
                            MakeSessionRetainedCandidate(expectation.expected)));
}

template <typename Result, typename Generated>
bool ValidateIdentityScript(const std::vector<Result>& script, char uuid_version) {
  std::vector<std::string_view> issued;
  issued.reserve(script.size());
  for (const auto& result : script) {
    const auto* generated = std::get_if<Generated>(&result);
    if (generated == nullptr) {
      continue;
    }
    if (!IsUuidVersion(generated->value.value, uuid_version) ||
        std::find(issued.begin(), issued.end(), generated->value.value) != issued.end()) {
      return false;
    }
    issued.push_back(generated->value.value);
  }
  return true;
}

bool ValidateLaunchIdentityScript(const std::vector<core::LaunchOperationIdentityResult>& script) {
  std::vector<std::string_view> issued;
  issued.reserve(script.size());
  for (const auto& result : script) {
    const auto* generated = std::get_if<core::GeneratedLaunchOperationIdentity>(&result);
    if (generated == nullptr) {
      continue;
    }
    if (!IsStableId(generated->value.value) ||
        std::find(issued.begin(), issued.end(), generated->value.value) != issued.end()) {
      return false;
    }
    issued.push_back(generated->value.value);
  }
  return true;
}

}  // namespace

FakeClock::FakeClock(core::ClockReading reading) : reading_(std::move(reading)) {
  verification_failed_ = !IsRfc3339(reading_.recorded_at.rfc3339);
}

core::ClockReading FakeClock::Read() { return reading_; }

void FakeClock::SetDiagnostic(core::DiagnosticTimestamp timestamp) {
  if (!IsRfc3339(timestamp.rfc3339)) {
    verification_failed_ = true;
    return;
  }
  reading_.recorded_at = std::move(timestamp);
}

bool FakeClock::AdvanceMonotonic(std::uint64_t delta) noexcept {
  auto& value = reading_.monotonic_time.nanoseconds_since_origin;
  if (delta > std::numeric_limits<std::uint64_t>::max() - value) {
    verification_failed_ = true;
    return false;
  }
  value += delta;
  return true;
}

bool FakeClock::verification_failed() const noexcept { return verification_failed_; }

FakeJobJournal::FakeJobJournal(std::vector<JournalExpectation> expectations)
    : expectations_(std::move(expectations)), observations_(expectations_.size()) {
  verification_failed_ =
      !std::all_of(expectations_.begin(), expectations_.end(), [](const auto& expectation) {
        return IsLogicalCommitResult(expectation.result) &&
               IsValidLogicalJobEvent(expectation.event);
      });
}

core::LogicalCommitResult FakeJobJournal::Commit(const core::LogicalJobEvent& event) noexcept {
  if (verification_failed_) {
    return core::LogicalCommitResult::kOutcomeUnknown;
  }
  try {
    if (next_expectation_ >= expectations_.size()) {
      verification_failed_ = true;
      return core::LogicalCommitResult::kOutcomeUnknown;
    }
    const auto& expectation = expectations_[next_expectation_];
    if (!EqualLogicalJobEvent(event, expectation.event) || expectation.fail_observation ||
        observation_count_ >= observations_.size()) {
      verification_failed_ = true;
      return core::LogicalCommitResult::kOutcomeUnknown;
    }
    observations_[observation_count_].emplace(event);
    ++observation_count_;
    ++next_expectation_;
    return expectation.result;
  } catch (...) {
    verification_failed_ = true;
    return core::LogicalCommitResult::kOutcomeUnknown;
  }
}

std::vector<core::LogicalJobEvent> FakeJobJournal::CopyObservations() const {
  std::vector<core::LogicalJobEvent> result;
  result.reserve(observation_count_);
  for (std::size_t index = 0; index < observation_count_; ++index) {
    result.push_back(*observations_[index]);
  }
  return result;
}

std::size_t FakeJobJournal::remaining_expectations() const noexcept {
  return expectations_.size() - next_expectation_;
}

bool FakeJobJournal::verification_failed() const noexcept { return verification_failed_; }

bool FakeJobJournal::Verify() noexcept {
  if (next_expectation_ != expectations_.size()) {
    verification_failed_ = true;
  }
  return !verification_failed_;
}

RunnerExpectation ExpectLaunch(core::ApplicationLaunchRequest expected, bool started) {
  auto candidate = MakeLaunchObservedCandidate(expected, started);
  return RunnerExpectation{RunnerCallKind::kLaunch, std::move(expected), std::move(candidate)};
}

RunnerExpectation ExpectLaunchProcessAlreadyExited(core::ApplicationLaunchRequest expected) {
  auto candidate = MakeProcessExitCandidate(expected.job_id, expected.intent.operation_id,
                                            core::CompletionMode::kProcessAlreadyExited);
  return RunnerExpectation{RunnerCallKind::kLaunch, std::move(expected), std::move(candidate)};
}

RunnerExpectation ExpectCooperativeStop(core::ApplicationStopRequest expected) {
  auto candidate = MakeProcessExitCandidate(expected.job_id, expected.launch_operation_id,
                                            core::CompletionMode::kCooperative);
  return RunnerExpectation{RunnerCallKind::kCooperativeStop, std::move(expected),
                           std::move(candidate)};
}

RunnerExpectation ExpectForcedStop(core::ApplicationStopRequest expected) {
  auto candidate = MakeProcessExitCandidate(expected.job_id, expected.launch_operation_id,
                                            core::CompletionMode::kForced);
  return RunnerExpectation{RunnerCallKind::kForcedStop, std::move(expected), std::move(candidate)};
}

FakeApplicationRunner::FakeApplicationRunner(std::vector<RunnerExpectation> expectations,
                                             std::size_t candidate_capacity)
    : expectations_(std::move(expectations)),
      observations_(expectations_.size()),
      candidates_(candidate_capacity) {
  const auto candidate_count =
      std::count_if(expectations_.begin(), expectations_.end(),
                    [](const auto& expectation) { return expectation.candidate.has_value(); });
  verification_failed_ =
      static_cast<std::size_t>(candidate_count) > candidate_capacity ||
      !std::all_of(expectations_.begin(), expectations_.end(), IsValidRunnerExpectation);
}

void FakeApplicationRunner::HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept {
  HandleLaunch(std::move(request));
}

void FakeApplicationRunner::HandoffCooperativeStop(
    core::ApplicationStopRequest&& request) noexcept {
  HandleStop(RunnerCallKind::kCooperativeStop, std::move(request));
}

void FakeApplicationRunner::HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept {
  HandleStop(RunnerCallKind::kForcedStop, std::move(request));
}

void FakeApplicationRunner::HandleLaunch(core::ApplicationLaunchRequest&& request) noexcept {
  static_assert(std::is_nothrow_constructible_v<RunnerRequest, core::ApplicationLaunchRequest&&>);
  static_assert(std::is_nothrow_move_constructible_v<RunnerObservation>);
  static_assert(std::is_nothrow_destructible_v<RunnerObservation>);
  static_assert(noexcept(std::declval<std::optional<RunnerObservation>&>() =
                             std::declval<RunnerObservation&&>()));
  if (verification_failed_ || next_expectation_ >= expectations_.size()) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  auto& expectation = expectations_[next_expectation_];
  const auto* expected = std::get_if<core::ApplicationLaunchRequest>(&expectation.expected);
  if (expectation.kind != RunnerCallKind::kLaunch || expected == nullptr ||
      !EqualLaunchRequest(request, *expected)) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  observations_[observation_count_] =
      RunnerObservation{RunnerCallKind::kLaunch, RunnerRequest{std::move(request)}};
  ++observation_count_;
  StageCandidate(expectation.candidate);
  if (!verification_failed_) {
    ++next_expectation_;
  }
}

void FakeApplicationRunner::HandleStop(RunnerCallKind kind,
                                       core::ApplicationStopRequest&& request) noexcept {
  static_assert(std::is_nothrow_constructible_v<RunnerRequest, core::ApplicationStopRequest&&>);
  static_assert(std::is_nothrow_move_constructible_v<RunnerObservation>);
  static_assert(std::is_nothrow_destructible_v<RunnerObservation>);
  static_assert(noexcept(std::declval<std::optional<RunnerObservation>&>() =
                             std::declval<RunnerObservation&&>()));
  if (verification_failed_ || next_expectation_ >= expectations_.size()) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  auto& expectation = expectations_[next_expectation_];
  const auto* expected = std::get_if<core::ApplicationStopRequest>(&expectation.expected);
  if (expectation.kind != kind || expected == nullptr || request != *expected) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  observations_[observation_count_] = RunnerObservation{kind, RunnerRequest{std::move(request)}};
  ++observation_count_;
  StageCandidate(expectation.candidate);
  if (!verification_failed_) {
    ++next_expectation_;
  }
}

void FakeApplicationRunner::StageCandidate(
    std::optional<core::RawCandidateEvent>& candidate) noexcept {
  static_assert(std::is_nothrow_move_constructible_v<core::RawCandidateEvent>);
  static_assert(std::is_nothrow_destructible_v<core::RawCandidateEvent>);
  static_assert(noexcept(std::declval<std::optional<core::RawCandidateEvent>&>() =
                             std::declval<core::RawCandidateEvent&&>()));
  if (!candidate.has_value()) {
    return;
  }
  if (candidate_write_ >= candidates_.size()) {
    verification_failed_ = true;
    return;
  }
  candidates_[candidate_write_] = std::move(*candidate);
  candidate.reset();
  ++candidate_write_;
  ++pending_candidates_;
}

std::vector<RunnerObservation> FakeApplicationRunner::CopyObservations() const {
  std::vector<RunnerObservation> result;
  result.reserve(observation_count_);
  for (std::size_t index = 0; index < observation_count_; ++index) {
    result.push_back(*observations_[index]);
  }
  return result;
}

std::optional<core::RawCandidateEvent> FakeApplicationRunner::TakeNextCandidate() {
  if (pending_candidates_ == 0) {
    return std::nullopt;
  }
  auto result = std::move(candidates_[candidate_read_]);
  candidates_[candidate_read_].reset();
  ++candidate_read_;
  --pending_candidates_;
  return result;
}

bool FakeApplicationRunner::CancelNextCandidate() noexcept {
  if (pending_candidates_ == 0) {
    verification_failed_ = true;
    return false;
  }
  candidates_[candidate_read_].reset();
  ++candidate_read_;
  --pending_candidates_;
  return true;
}

std::size_t FakeApplicationRunner::discarded_request_count() const noexcept {
  return discarded_request_count_;
}

bool FakeApplicationRunner::verification_failed() const noexcept { return verification_failed_; }

bool FakeApplicationRunner::Verify() noexcept {
  if (next_expectation_ != expectations_.size() || pending_candidates_ != 0) {
    verification_failed_ = true;
  }
  return !verification_failed_;
}

SessionExpectation ExpectSessionRetain(core::SessionRetainRequest expected) {
  auto candidate = MakeSessionRetainedCandidate(expected);
  return SessionExpectation{std::move(expected), std::move(candidate)};
}

FakeSessionRetainer::FakeSessionRetainer(std::vector<SessionExpectation> expectations,
                                         std::size_t candidate_capacity)
    : expectations_(std::move(expectations)),
      observations_(expectations_.size()),
      candidates_(candidate_capacity) {
  const auto candidate_count =
      std::count_if(expectations_.begin(), expectations_.end(),
                    [](const auto& expectation) { return expectation.candidate.has_value(); });
  verification_failed_ =
      static_cast<std::size_t>(candidate_count) > candidate_capacity ||
      !std::all_of(expectations_.begin(), expectations_.end(), IsValidSessionExpectation);
}

void FakeSessionRetainer::HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept {
  static_assert(std::is_nothrow_move_constructible_v<core::SessionRetainRequest>);
  static_assert(std::is_nothrow_destructible_v<core::SessionRetainRequest>);
  static_assert(std::is_nothrow_move_constructible_v<core::RawCandidateEvent>);
  static_assert(std::is_nothrow_destructible_v<core::RawCandidateEvent>);
  static_assert(noexcept(std::declval<std::optional<core::SessionRetainRequest>&>() =
                             std::declval<core::SessionRetainRequest&&>()));
  static_assert(noexcept(std::declval<std::optional<core::RawCandidateEvent>&>() =
                             std::declval<core::RawCandidateEvent&&>()));
  if (verification_failed_ || next_expectation_ >= expectations_.size()) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  auto& expectation = expectations_[next_expectation_];
  if (request != expectation.expected) {
    verification_failed_ = true;
    ConsumeTransferredRequest(std::move(request));
    ++discarded_request_count_;
    return;
  }
  observations_[observation_count_] = std::move(request);
  ++observation_count_;
  if (expectation.candidate.has_value()) {
    if (candidate_write_ >= candidates_.size()) {
      verification_failed_ = true;
      return;
    }
    candidates_[candidate_write_] = std::move(*expectation.candidate);
    expectation.candidate.reset();
    ++candidate_write_;
    ++pending_candidates_;
  }
  ++next_expectation_;
}

std::vector<core::SessionRetainRequest> FakeSessionRetainer::CopyObservations() const {
  std::vector<core::SessionRetainRequest> result;
  result.reserve(observation_count_);
  for (std::size_t index = 0; index < observation_count_; ++index) {
    result.push_back(*observations_[index]);
  }
  return result;
}

std::optional<core::RawCandidateEvent> FakeSessionRetainer::TakeNextCandidate() {
  if (pending_candidates_ == 0) {
    return std::nullopt;
  }
  auto result = std::move(candidates_[candidate_read_]);
  candidates_[candidate_read_].reset();
  ++candidate_read_;
  --pending_candidates_;
  return result;
}

bool FakeSessionRetainer::CancelNextCandidate() noexcept {
  if (pending_candidates_ == 0) {
    verification_failed_ = true;
    return false;
  }
  candidates_[candidate_read_].reset();
  ++candidate_read_;
  --pending_candidates_;
  return true;
}

std::size_t FakeSessionRetainer::discarded_request_count() const noexcept {
  return discarded_request_count_;
}

bool FakeSessionRetainer::verification_failed() const noexcept { return verification_failed_; }

bool FakeSessionRetainer::Verify() noexcept {
  if (next_expectation_ != expectations_.size() || pending_candidates_ != 0) {
    verification_failed_ = true;
  }
  return !verification_failed_;
}

FakeIdentitySource::FakeIdentitySource(
    std::vector<core::JobSessionIdentityResult> job_session_script,
    std::vector<core::WorkerIdentityResult> worker_script,
    std::vector<core::LaunchOperationIdentityResult> launch_script)
    : job_session_script_(std::move(job_session_script)),
      worker_script_(std::move(worker_script)),
      launch_script_(std::move(launch_script)) {
  verification_failed_ =
      !ValidateIdentityScript<core::JobSessionIdentityResult, core::GeneratedJobSessionIdentity>(
          job_session_script_, '7') ||
      !ValidateIdentityScript<core::WorkerIdentityResult, core::GeneratedWorkerIdentity>(
          worker_script_, '4') ||
      !ValidateLaunchIdentityScript(launch_script_);
}

core::JobSessionIdentityResult FakeIdentitySource::GenerateJobSessionIdentity() {
  if (verification_failed_ || next_job_session_ >= job_session_script_.size()) {
    verification_failed_ = true;
    return core::IdentitySourceExhausted{};
  }
  auto result = job_session_script_[next_job_session_];
  ++next_job_session_;
  return result;
}

core::WorkerIdentityResult FakeIdentitySource::GenerateWorkerIdentity() {
  if (verification_failed_ || next_worker_ >= worker_script_.size()) {
    verification_failed_ = true;
    return core::IdentitySourceExhausted{};
  }
  auto result = worker_script_[next_worker_];
  ++next_worker_;
  return result;
}

core::LaunchOperationIdentityResult FakeIdentitySource::GenerateLaunchOperationIdentity() {
  if (verification_failed_ || next_launch_ >= launch_script_.size()) {
    verification_failed_ = true;
    return core::IdentitySourceExhausted{};
  }
  auto result = launch_script_[next_launch_];
  ++next_launch_;
  return result;
}

bool FakeIdentitySource::verification_failed() const noexcept { return verification_failed_; }

bool FakeIdentitySource::Verify() noexcept {
  if (next_job_session_ != job_session_script_.size() || next_worker_ != worker_script_.size() ||
      next_launch_ != launch_script_.size()) {
    verification_failed_ = true;
  }
  return !verification_failed_;
}

PassiveEffectObserver::PassiveEffectObserver(std::size_t capacity) : observations_(capacity) {}

bool PassiveEffectObserver::Observe(core::EffectId effect_id) noexcept {
  static_assert(noexcept(std::declval<std::optional<EffectObservation>&>() =
                             std::declval<EffectObservation&&>()));
  if (verification_failed_ || effect_id == core::EffectId::kInvalid ||
      observation_count_ >= observations_.size()) {
    verification_failed_ = true;
    return false;
  }
  observations_[observation_count_] =
      EffectObservation{static_cast<std::uint64_t>(observation_count_), effect_id};
  ++observation_count_;
  return true;
}

std::vector<EffectObservation> PassiveEffectObserver::CopyObservations() const {
  std::vector<EffectObservation> result;
  result.reserve(observation_count_);
  for (std::size_t index = 0; index < observation_count_; ++index) {
    result.push_back(*observations_[index]);
  }
  return result;
}

bool PassiveEffectObserver::verification_failed() const noexcept { return verification_failed_; }

}  // namespace sitometron::test
