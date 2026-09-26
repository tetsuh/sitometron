#include "file_journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>
#include <variant>

namespace sitometron::spike {
namespace {

using core::EventType;
using nlohmann::json;

std::string_view PhaseName(core::TimeoutPhase phase) {
  switch (phase) {
    case core::TimeoutPhase::kPreparation:
      return "preparation";
    case core::TimeoutPhase::kExecution:
      return "execution";
    case core::TimeoutPhase::kCooperativeStop:
      return "cooperative_stop";
    case core::TimeoutPhase::kProcessExitConfirmation:
      return "process_exit_confirmation";
    default:
      return "invalid";
  }
}

std::string_view CompletionModeName(core::CompletionMode mode) {
  switch (mode) {
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

std::string_view OutcomeName(core::TerminalOutcome outcome) {
  switch (outcome) {
    case core::TerminalOutcome::kSucceeded:
      return "succeeded";
    case core::TerminalOutcome::kFailed:
      return "failed";
    case core::TerminalOutcome::kCancelled:
      return "cancelled";
    case core::TerminalOutcome::kTerminated:
      return "terminated";
    case core::TerminalOutcome::kTimedOut:
      return "timed_out";
    default:
      return "invalid";
  }
}

json PayloadJson(const core::EventPayload& payload) {
  return std::visit(
      [](const auto& value) -> json {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::EmptyPayload>) {
          return json::object();
        } else if constexpr (std::is_same_v<Value, core::JobCreatedPayload> ||
                             std::is_same_v<Value, core::SessionPayload>) {
          return {{"session_id", value.session_id.value}};
        } else if constexpr (std::is_same_v<Value, core::ResourcesCommittedPayload>) {
          return {{"allocation_id", value.allocation_id.value},
                  {"allocation_digest", value.allocation_digest.value},
                  {"resolved_allocation",
                   {{"schema_id", value.schema_id.value},
                    {"schema_version", value.schema_version},
                    {"payload_utf8", value.payload_utf8}}}};
        } else if constexpr (std::is_same_v<Value, core::WorkerLaunchIntentPayload>) {
          return {{"operation_id", value.operation_id.value},
                  {"application",
                   {{"application_id", value.application_id.value},
                    {"version", value.application_version},
                    {"bundle_sha256", value.bundle_sha256.value}}},
                  {"allocation_id", value.allocation_id.value},
                  {"allocation_digest", value.allocation_digest.value},
                  {"worker_id", value.worker_id.value}};
        } else if constexpr (std::is_same_v<Value, core::WorkerLaunchObservedPayload>) {
          return {{"operation_id", value.operation_id.value},
                  {"outcome", value.started ? "started" : "failed"}};
        } else if constexpr (std::is_same_v<Value, core::WorkerRunningPayload>) {
          return {{"worker_id", value.worker_id.value}};
        } else if constexpr (std::is_same_v<Value, core::PrincipalPayload>) {
          return {{"principal_subject", value.principal_subject}};
        } else if constexpr (std::is_same_v<Value, core::TimeoutExpiredPayload>) {
          return {{"phase", PhaseName(value.phase)}, {"timer_generation", value.timer_generation}};
        } else if constexpr (std::is_same_v<Value, core::WorkerEventPayload>) {
          return {{"worker_id", value.worker_id.value}, {"event_sequence", value.event_sequence}};
        } else if constexpr (std::is_same_v<Value, core::ProcessExitConfirmedPayload>) {
          return {{"completion_mode", CompletionModeName(value.completion_mode)},
                  {"launch_operation_id", value.launch_operation_id.value}};
        } else if constexpr (std::is_same_v<Value, core::TerminalOutcomePayload>) {
          return {{"outcome", OutcomeName(value.outcome)}};
        } else if constexpr (std::is_same_v<Value, core::ResourcesReleasedPayload>) {
          return {{"allocation_id", value.allocation_id.value},
                  {"allocation_digest", value.allocation_digest.value}};
        } else if constexpr (std::is_same_v<Value, core::CleanupStatusPayload>) {
          return {{"status",
                   value.status == core::CleanupStatus::kCompleted ? "completed" : "incomplete"}};
        } else {
          static_assert(std::is_same_v<Value, core::LateWorkerEventPayload>);
          return {{"original_event_type", core::ToString(value.original_event_type)},
                  {"worker_id", value.worker_id.value},
                  {"event_sequence", value.event_sequence}};
        }
      },
      payload);
}

}  // namespace

FileJournal::FileJournal(std::string path) : path_(std::move(path)) {}

FileJournal::~FileJournal() {
  if (fd_ >= 0) ::close(fd_);
}

bool FileJournal::Open(std::string& error) {
  {
    std::ifstream existing(path_);
    std::string line;
    while (std::getline(existing, line)) {
      ++lines_on_open_;
      try {
        const auto record = json::parse(line);
        if (record.contains("sequence") && record["sequence"].is_number_unsigned())
          last_sequence_ = std::max(last_sequence_, record["sequence"].get<std::uint64_t>());
      } catch (const json::exception& e) {
        error =
            "journal line " + std::to_string(lines_on_open_) + " is not valid JSON: " + e.what();
        return false;
      }
    }
  }
  {
    // O_APPEND concatenates onto the last line, so an existing file must end with LF.
    std::ifstream tail(path_, std::ios::binary | std::ios::ate);
    if (tail && tail.tellg() > 0) {
      tail.seekg(-1, std::ios::end);
      char last = 0;
      if (!tail.get(last) || last != '\n') {
        error = "journal does not end with a newline; refusing to append to a torn record";
        return false;
      }
    }
  }
  fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

json FileJournal::ToJson(const core::LogicalJobEvent& event) {
  return {{"schema_version", event.schema_version},
          {"sequence", event.sequence},
          {"event_type", core::ToString(event.event_type)},
          {"recorded_at", event.recorded_at.rfc3339},
          {"job_id", event.job_id.value},
          {"payload", PayloadJson(event.payload)}};
}

core::LogicalCommitResult FileJournal::Commit(const core::LogicalJobEvent& event) noexcept {
  try {
    std::lock_guard lock(mutex_);
    if (fd_ < 0) return core::LogicalCommitResult::kDefiniteFailure;
    const std::string line = ToJson(event).dump() + '\n';
    std::size_t written = 0;
    while (written < line.size()) {
      const auto count = ::write(fd_, line.data() + written, line.size() - written);
      if (count < 0) {
        if (errno == EINTR) continue;
        // Nothing of this record reached the file: the writer may treat it as never attempted.
        // Anything after the first byte is unknown because a torn line may already be durable.
        return written == 0 ? core::LogicalCommitResult::kDefiniteFailure
                            : core::LogicalCommitResult::kOutcomeUnknown;
      }
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(fd_) != 0) return core::LogicalCommitResult::kOutcomeUnknown;
    ++committed_;
    return core::LogicalCommitResult::kCommitted;
  } catch (...) {
    return core::LogicalCommitResult::kOutcomeUnknown;
  }
}

std::size_t FileJournal::committed_count() const noexcept {
  std::lock_guard lock(mutex_);
  return committed_;
}

}  // namespace sitometron::spike
