#ifndef SITOMETRON_TESTS_SUPPORT_JOURNAL_TEST_EVENTS_HPP_
#define SITOMETRON_TESTS_SUPPORT_JOURNAL_TEST_EVENTS_HPP_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/record_codec.hpp"

// Canonical Journal records for tests that build Journals by hand.
namespace sitometron::test {

// A stable Job ID for test Job `n`.
inline std::string Job(int n) {
  std::string suffix = std::to_string(n);
  return "01890f3e-7b00-7abc-8abc-" + std::string(12 - suffix.size(), '0') + suffix;
}

inline core::LogicalJobEvent Created(std::uint64_t sequence, int job) {
  return core::LogicalJobEvent{1,
                               sequence,
                               core::EventType::kJobCreated,
                               core::DiagnosticTimestamp{"2026-09-28T01:02:03Z"},
                               core::Uuid{Job(job)},
                               core::JobCreatedPayload{core::Uuid{Job(job)}}};
}

inline core::LogicalJobEvent Cancelled(std::uint64_t sequence, int job) {
  return core::LogicalJobEvent{1,
                               sequence,
                               core::EventType::kCancelAccepted,
                               core::DiagnosticTimestamp{"2026-09-28T01:02:04Z"},
                               core::Uuid{Job(job)},
                               core::PrincipalPayload{"operator@example"}};
}

inline core::LogicalJobEvent Recorded(std::uint64_t sequence, int job, core::EventType type,
                                      core::EventPayload payload) {
  return core::LogicalJobEvent{1,
                               sequence,
                               type,
                               core::DiagnosticTimestamp{"2026-09-28T01:02:05Z"},
                               core::Uuid{Job(job)},
                               std::move(payload)};
}

inline std::string Bytes(const core::LogicalJobEvent& event) {
  return journal::EncodeRecord(event).bytes;
}

// Job `job` runs to the end from sequence `first`: cancelled while admitted, finalized, terminal,
// cleaned up. Seven records; the Job is resolved afterwards.
inline std::vector<core::LogicalJobEvent> ClosedJobEvents(std::uint64_t first, int job) {
  using core::EventType;
  const core::Uuid id{Job(job)};
  return {Created(first, job),
          Cancelled(first + 1, job),
          Recorded(first + 2, job, EventType::kSessionRetainRequested, core::SessionPayload{id}),
          Recorded(first + 3, job, EventType::kSessionRetained, core::SessionPayload{id}),
          Recorded(first + 4, job, EventType::kFinalizationCompleted, core::EmptyPayload{}),
          Recorded(first + 5, job, EventType::kTerminalOutcomeCommitted,
                   core::TerminalOutcomePayload{core::TerminalOutcome::kCancelled}),
          Recorded(first + 6, job, EventType::kCleanupStatusRecorded,
                   core::CleanupStatusPayload{core::CleanupStatus::kCompleted})};
}

// Records [from, to) of a Job's closed lifecycle, as bytes.
inline std::string ClosedJobPart(std::uint64_t first, int job, std::size_t from, std::size_t to) {
  const auto events = ClosedJobEvents(first, job);
  std::string bytes;
  for (std::size_t i = from; i < to && i < events.size(); ++i) bytes += Bytes(events[i]);
  return bytes;
}

inline std::string ClosedJob(std::uint64_t first, int job) {
  return ClosedJobPart(first, job, 0, 7);
}

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_JOURNAL_TEST_EVENTS_HPP_
