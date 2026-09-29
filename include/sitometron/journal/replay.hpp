#ifndef SITOMETRON_JOURNAL_REPLAY_HPP_
#define SITOMETRON_JOURNAL_REPLAY_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/core/job_reducer.hpp"
#include "sitometron/journal/file_system.hpp"

// JobJournal startup validation and replay (Accepted ADR-0006, Sections 5 and 6).
//
// ReplayJournal reads every segment of a locked Journal directory, validates it, and rebuilds each
// Job's snapshot by re-deriving every record through the pure reducer. It never writes, syncs,
// locks, or creates anything, and it dispatches no effect.
namespace sitometron::journal {

enum class ReplayStatus {
  kReplayed,
  kTornTail,           // journal_torn_tail
  kCorrupt,            // journal_corrupt
  kCapacityExceeded,   // journal_capacity_exceeded
  kSequenceExhausted,  // journal_sequence_exhausted
  kUnreadable,         // the directory or a segment could not be listed or read
};

struct ReplayOptions {
  // The resident Job capacity of the writer that will start from the result. There is no default:
  // zero would refuse every non-empty Journal.
  explicit ReplayOptions(std::size_t capacity) : max_jobs(capacity) {}
  std::size_t max_jobs;
};

struct ReplayResult {
  ReplayStatus status = ReplayStatus::kCorrupt;
  std::string detail;                  // refusal code and location when status is not kReplayed
  std::uint64_t next_sequence = 0;     // last replayed sequence plus one; 1 for an empty Journal
  std::uint64_t records = 0;           // records replayed
  std::vector<core::Snapshot> jobs;    // replayed snapshots in creation order
  std::vector<core::Uuid> unresolved;  // Jobs that are non-terminal or not fully cleaned up
};

// Replays one record onto `before` (the Job's current snapshot, or nullopt when the Job does not
// exist yet). Returns the next snapshot, or nullopt with `error` set when the record is not what
// the reducer derives from `before`.
[[nodiscard]] std::optional<core::Snapshot> ReplayRecord(
    const std::optional<core::Snapshot>& before, const core::LogicalJobEvent& record,
    std::string& error);

// True when a replayed Job still needs resolution before new work may be admitted (ADR-0006 §6).
[[nodiscard]] bool IsUnresolved(const core::Snapshot& snapshot) noexcept;

[[nodiscard]] ReplayResult ReplayJournal(FileSystem& file_system, const std::string& directory,
                                         const ReplayOptions& options);

}  // namespace sitometron::journal

#endif  // SITOMETRON_JOURNAL_REPLAY_HPP_
