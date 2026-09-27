#ifndef SITOMETRON_JOURNAL_SEGMENT_JOURNAL_HPP_
#define SITOMETRON_JOURNAL_SEGMENT_JOURNAL_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/file_system.hpp"

// Durable JobJournal segment writer (Accepted ADR-0006, Sections 2-4 and 10).
//
// Records are appended to `journal-<first sequence, 20 digits>.ndjson` segments in one directory.
// Commit() encodes the event, writes every byte including the LF, completes a data sync, and only
// then returns kCommitted. A new segment's file and directory entry are synced before its first
// record. After the first non-committed result the journal is poisoned: every later Commit()
// returns kDefiniteFailure without I/O.
namespace sitometron::journal {

struct SegmentJournalOptions {
  // Rotate before a record would make the active segment exceed this size.
  std::uint64_t segment_limit_bytes = 67108864;
};

struct OpenResult {
  bool ok = false;
  std::string detail;               // reason when ok is false
  std::uint64_t next_sequence = 0;  // first sequence the next Commit() must carry
  std::string active_segment;       // file name of the segment the next record goes to, if any
};

class SegmentJournal final : public core::JobJournalPort {
 public:
  explicit SegmentJournal(FileSystem& file_system, SegmentJournalOptions options = {});
  ~SegmentJournal() override;
  SegmentJournal(const SegmentJournal&) = delete;
  SegmentJournal& operator=(const SegmentJournal&) = delete;

  // Creates or opens the Journal directory, takes its exclusive lock, and locates the active
  // segment. Full startup validation and replay belong to the next Phase 0B increment; Open()
  // refuses only states it cannot append to (lock held, unreadable directory, torn or
  // unparsable last record of the active segment, mis-named segment).
  [[nodiscard]] OpenResult Open(const std::string& directory);

  [[nodiscard]] core::LogicalCommitResult Commit(
      const core::LogicalJobEvent& event) noexcept override;

  [[nodiscard]] bool Poisoned() const;
  [[nodiscard]] std::uint64_t NextSequence() const;

  // Segment file name for the record with `first_sequence`.
  [[nodiscard]] static std::string SegmentName(std::uint64_t first_sequence);

 private:
  struct Located {
    std::string error;   // non-empty when the directory cannot be appended to
    std::uint64_t next;  // first sequence of the next record
    std::string active;  // segment to append to; empty when the next record starts a new one
    std::uint64_t active_size = 0;  // current size of `active`
  };
  Located Locate(const std::string& directory);
  core::LogicalCommitResult CommitLocked(const core::LogicalJobEvent& event);
  core::LogicalCommitResult StartSegment(std::uint64_t first_sequence);
  core::LogicalCommitResult MakeActiveDurable();
  void CloseActive() noexcept;

  FileSystem& file_system_;
  SegmentJournalOptions options_;
  mutable std::mutex mutex_;
  std::string directory_;
  std::optional<FileHandle> lock_;
  std::optional<FileHandle> active_;
  std::uint64_t active_size_ = 0;
  // False while the active segment was adopted empty at Open() and its file and directory entry
  // have not been re-synced by this process (ADR-0006 §3).
  bool active_durable_ = true;
  std::uint64_t next_sequence_ = 0;
  bool opened_ = false;
  std::atomic<bool> poisoned_{false};
  // True once the current Commit() has issued its first write of record bytes.
  std::atomic<bool> record_started_{false};
};

}  // namespace sitometron::journal

#endif  // SITOMETRON_JOURNAL_SEGMENT_JOURNAL_HPP_
