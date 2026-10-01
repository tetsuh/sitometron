#include "sitometron/journal/maintenance.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitometron/journal/replay.hpp"

namespace sitometron::journal {
namespace {

constexpr std::string_view lock_name = "journal.lock";

// A tool validates structure, not the daemon's resident capacity: a Journal over capacity is one
// reason to prune (ADR-0006 Section 8).
ReplayResult ReplayAll(FileSystem& file_system, const std::string& directory) {
  return ReplayJournal(file_system, directory,
                       ReplayOptions{std::numeric_limits<std::size_t>::max()});
}

// Holds journal.lock for one maintenance run and releases it on every exit.
class JournalLock {
 public:
  JournalLock(FileSystem& file_system, const std::string& directory) : file_system_(file_system) {
    handle_ = file_system_.Lock(JoinPath(directory, lock_name), error_);
  }
  ~JournalLock() {
    if (handle_.has_value()) file_system_.Close(*handle_);
  }
  JournalLock(const JournalLock&) = delete;
  JournalLock& operator=(const JournalLock&) = delete;

  // The refusal when the lock was not taken, or nullopt.
  [[nodiscard]] std::optional<std::string> Refusal(const std::string& directory) const {
    if (handle_.has_value()) return std::nullopt;
    if (error_ == IoError::kLocked) return "journal_locked: " + directory;
    return "journal_unreadable: cannot lock " + directory;
  }

 private:
  FileSystem& file_system_;
  IoError error_ = IoError::kNone;
  std::optional<FileHandle> handle_;
};

// Closes an open file on every exit.
class OpenFile {
 public:
  OpenFile(FileSystem& file_system, FileHandle handle)
      : file_system_(file_system), handle_(handle) {}
  ~OpenFile() { file_system_.Close(handle_); }
  OpenFile(const OpenFile&) = delete;
  OpenFile& operator=(const OpenFile&) = delete;
  [[nodiscard]] FileHandle Handle() const { return handle_; }

 private:
  FileSystem& file_system_;
  FileHandle handle_;
};

std::string Failed(std::string_view step, std::string_view path) {
  std::string detail = "journal_maintenance_failed: ";
  detail.append(step).append(" ").append(path);
  return detail;
}

// Appends `bytes` to `path` (creating it when `create` is set) and makes the data durable. Returns
// the failure detail naming the step, or nullopt.
std::optional<std::string> AppendDurably(MaintenanceFileSystem& file_system,
                                         const std::string& path, std::string_view bytes,
                                         bool create) {
  IoError error = IoError::kNone;
  const auto handle = file_system.OpenAppend(path, create, error);
  if (!handle.has_value()) return Failed(create ? "create" : "open", path);
  const OpenFile file(file_system, *handle);
  while (!bytes.empty()) {
    const auto outcome = file_system.Write(file.Handle(), bytes);
    if (outcome.error == IoError::kInterrupted && outcome.written == 0) continue;
    if (outcome.error != IoError::kNone || outcome.written == 0) return Failed("write", path);
    bytes.remove_prefix(std::min(outcome.written, bytes.size()));
  }
  if (file_system.SyncData(file.Handle()) != IoError::kNone) return Failed("sync", path);
  return std::nullopt;
}

// A replay result carries segment names but not the Journal directory; every maintenance result
// names its location, so the directory is appended.
std::string In(std::string_view detail, std::string_view directory) {
  return std::string(detail).append(" in ").append(directory);
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

class Quarantine {
 public:
  Quarantine(MaintenanceFileSystem& file_system, const std::string& directory)
      : file_system_(file_system), directory_(directory) {}

  QuarantineResult Run() {
    const JournalLock lock(file_system_, directory_);
    if (auto refusal = lock.Refusal(directory_)) {
      return Finish(MaintenanceStatus::kRefused, *refusal);
    }
    const auto replayed = ReplayAll(file_system_, directory_);
    if (replayed.status == ReplayStatus::kReplayed) {
      result_.next_sequence = replayed.next_sequence;
      return Finish(MaintenanceStatus::kNothingToDo, "journal_clean: " + directory_);
    }
    if (replayed.status != ReplayStatus::kTornTail) {
      return Finish(MaintenanceStatus::kRefused, In(replayed.detail, directory_));
    }
    result_.segment = replayed.torn_segment;
    result_.offset = replayed.torn_offset;
    result_.quarantine = replayed.torn_segment + ".torn-" + std::to_string(replayed.torn_offset);
    const auto segment_path = JoinPath(directory_, result_.segment);
    IoError error = IoError::kNone;
    const auto content = file_system_.ReadAll(segment_path, error);
    if (!content.has_value() || content->size() <= result_.offset) {
      return Finish(MaintenanceStatus::kFailed, Failed("read", segment_path));
    }
    const std::string_view tail = std::string_view(*content).substr(result_.offset);
    result_.bytes = tail.size();
    if (auto failure = WriteQuarantine(tail)) return *failure;
    // Only now, with every torn byte durable in the quarantine file, is the segment cut.
    if (file_system_.Truncate(segment_path, result_.offset) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("truncate", segment_path));
    }
    const auto after = ReplayAll(file_system_, directory_);
    if (after.status != ReplayStatus::kReplayed &&
        after.status != ReplayStatus::kSequenceExhausted) {
      return Finish(MaintenanceStatus::kFailed, Failed("verify", In(after.detail, directory_)));
    }
    result_.next_sequence = after.next_sequence;
    std::string detail = "journal_quarantined: ";
    detail.append(result_.segment)
        .append(" at byte ")
        .append(std::to_string(result_.offset))
        .append(" into ")
        .append(result_.quarantine);
    return Finish(MaintenanceStatus::kDone, std::move(detail));
  }

  QuarantineResult Finish(MaintenanceStatus status, std::string detail) {
    result_.status = status;
    result_.detail = std::move(detail);
    return result_;
  }

 private:
  // Makes the quarantine file hold exactly `tail`, durably, with a durable directory entry. A file
  // left by an interrupted run is completed when it holds a prefix of `tail`.
  std::optional<QuarantineResult> WriteQuarantine(std::string_view tail) {
    const auto path = JoinPath(directory_, result_.quarantine);
    IoError error = IoError::kNone;
    const auto names = file_system_.List(directory_, error);
    if (!names.has_value()) return Finish(MaintenanceStatus::kFailed, Failed("list", directory_));
    std::optional<std::string> failure;
    if (Contains(*names, result_.quarantine)) {
      const auto existing = file_system_.ReadAll(path, error);
      if (!existing.has_value()) return Finish(MaintenanceStatus::kFailed, Failed("read", path));
      if (existing->size() > tail.size() || !tail.starts_with(*existing)) {
        return Finish(MaintenanceStatus::kRefused,
                      "journal_quarantine_conflict: " + path + " exists with different bytes");
      }
      // Identical bytes may still be only in the page cache: sync them before the cut.
      failure = AppendDurably(file_system_, path, tail.substr(existing->size()), false);
    } else {
      failure = AppendDurably(file_system_, path, tail, true);
    }
    if (failure.has_value()) return Finish(MaintenanceStatus::kFailed, *failure);
    if (file_system_.SyncDirectory(directory_) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("sync directory", directory_));
    }
    return std::nullopt;
  }

  MaintenanceFileSystem& file_system_;
  const std::string& directory_;
  QuarantineResult result_;
};

class Prune {
 public:
  Prune(MaintenanceFileSystem& file_system, const std::string& directory,
        const PruneOptions& options)
      : file_system_(file_system), directory_(directory), options_(options) {}

  PruneResult Run() {
    const JournalLock lock(file_system_, directory_);
    if (auto refusal = lock.Refusal(directory_)) {
      return Finish(MaintenanceStatus::kRefused, *refusal);
    }
    const auto replayed = ReplayAll(file_system_, directory_);
    if (replayed.status != ReplayStatus::kReplayed) {
      return Finish(MaintenanceStatus::kRefused, In(replayed.detail, directory_));
    }
    result_.next_sequence = replayed.next_sequence;
    const auto length = PrunableLength(replayed);
    if (length == 0) {
      return Finish(
          MaintenanceStatus::kNothingToDo,
          In("journal_nothing_to_prune: no sealed prefix holds only closed Jobs", directory_));
    }
    for (std::size_t s = 0; s < length; ++s) {
      result_.segments.push_back(replayed.segments[s].name);
      result_.records += replayed.segments[s].records;
    }
    for (std::size_t j = 0; j < replayed.jobs.size(); ++j) {
      if (replayed.spans[j].first_segment < length) result_.jobs.push_back(replayed.jobs[j].job_id);
    }
    result_.first_retained_sequence = replayed.segments[length].first;
    if (options_.dry_run) {
      return Finish(MaintenanceStatus::kPlanned, Summary("journal_prune_planned"));
    }
    if (auto failure = Archive()) return *failure;
    if (const auto after = ReplayAll(file_system_, directory_);
        after.status != ReplayStatus::kReplayed || after.next_sequence != result_.next_sequence ||
        after.records + result_.records != replayed.records) {
      return Finish(MaintenanceStatus::kFailed, Failed("verify", In(after.detail, directory_)));
    }
    return Finish(MaintenanceStatus::kDone, Summary("journal_pruned"));
  }

  PruneResult Finish(MaintenanceStatus status, std::string detail) {
    result_.status = status;
    result_.detail = std::move(detail);
    if (status == MaintenanceStatus::kRefused || status == MaintenanceStatus::kFailed) {
      // Nothing is reported as moved or planned; a failed run is continued by running again.
      result_.segments.clear();
      result_.jobs.clear();
      result_.records = 0;
    }
    return result_;
  }

 private:
  // The number of leading segments in the longest prunable prefix (ADR-0006 Section 8).
  static std::size_t PrunableLength(const ReplayResult& replayed) {
    const auto& segments = replayed.segments;
    const auto non_empty = std::find_if(segments.rbegin(), segments.rend(),
                                        [](const ReplayedSegment& s) { return s.records != 0; });
    if (non_empty == segments.rend()) return 0;
    // The highest non-empty segment, and any empty one after it, always stay.
    const auto highest = static_cast<std::size_t>(std::distance(non_empty, segments.rend())) - 1;
    // A prefix of k segments (0 <= k <= highest) is blocked by a Job whose first record is in it
    // (first < k) unless the Job is closed and its last record is in it too (last < k). A
    // difference array over k marks every blocked length in one pass.
    std::vector<std::ptrdiff_t> blocked(highest + 2, 0);
    for (std::size_t j = 0; j < replayed.jobs.size(); ++j) {
      const auto first = replayed.spans[j].first_segment;
      const auto end =
          IsUnresolved(replayed.jobs[j]) ? highest + 1 : replayed.spans[j].last_segment + 1;
      if (first + 1 < end) {
        ++blocked[first + 1];
        --blocked[end];
      }
    }
    std::size_t longest = 0;
    std::ptrdiff_t running = 0;
    for (std::size_t k = 0; k <= highest; ++k) {
      running += blocked[k];
      if (running == 0) longest = k;
    }
    return longest;
  }

  // Moves the prefix into the archive directory, lowest first, making both directory entries
  // durable after each move. A failure leaves a valid, shorter pruned Journal.
  std::optional<PruneResult> Archive() {
    const auto archive = JoinPath(directory_, k_archive_directory);
    if (file_system_.EnsureDirectory(archive) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("archive directory", archive));
    }
    // EnsureDirectory syncs the entry only when it creates archive/; one left by an interrupted run
    // may not be durable yet. Sync it before any segment can live below it (ADR-0006 Section 3).
    if (file_system_.SyncDirectory(directory_) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("sync directory", directory_));
    }
    IoError error = IoError::kNone;
    const auto archived = file_system_.List(archive, error);
    if (!archived.has_value()) return Finish(MaintenanceStatus::kFailed, Failed("list", archive));
    for (const auto& name : result_.segments) {
      if (Contains(*archived, name)) {
        return Finish(MaintenanceStatus::kRefused,
                      "journal_archive_conflict: " + JoinPath(archive, name) + " already exists");
      }
    }
    for (const auto& name : result_.segments) {
      if (const auto from = JoinPath(directory_, name);
          file_system_.RenameNoReplace(from, JoinPath(archive, name)) != IoError::kNone) {
        return Finish(MaintenanceStatus::kFailed, Failed("rename", from));
      }
      if (file_system_.SyncDirectory(archive) != IoError::kNone) {
        return Finish(MaintenanceStatus::kFailed, Failed("sync directory", archive));
      }
      if (file_system_.SyncDirectory(directory_) != IoError::kNone) {
        return Finish(MaintenanceStatus::kFailed, Failed("sync directory", directory_));
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string Summary(std::string_view code) const {
    std::string detail(code);
    detail.append(": ")
        .append(std::to_string(result_.segments.size()))
        .append(" segments, ")
        .append(std::to_string(result_.records))
        .append(" records, ")
        .append(std::to_string(result_.jobs.size()))
        .append(" Jobs; replay starts at ")
        .append(std::to_string(result_.first_retained_sequence));
    return detail;
  }

  MaintenanceFileSystem& file_system_;
  const std::string& directory_;
  const PruneOptions& options_;
  PruneResult result_;
};

}  // namespace

QuarantineResult QuarantineTornTail(MaintenanceFileSystem& file_system,
                                    const std::string& directory) {
  Quarantine quarantine(file_system, directory);
  try {
    return quarantine.Run();
  } catch (const std::exception& error) {
    return quarantine.Finish(
        MaintenanceStatus::kFailed,
        In(std::string("journal_maintenance_failed: ") + error.what(), directory));
  } catch (...) {
    return quarantine.Finish(MaintenanceStatus::kFailed,
                             In("journal_maintenance_failed: unexpected exception", directory));
  }
}

PruneResult PruneClosedPrefix(MaintenanceFileSystem& file_system, const std::string& directory,
                              const PruneOptions& options) {
  Prune prune(file_system, directory, options);
  try {
    return prune.Run();
  } catch (const std::exception& error) {
    return prune.Finish(MaintenanceStatus::kFailed,
                        In(std::string("journal_maintenance_failed: ") + error.what(), directory));
  } catch (...) {
    return prune.Finish(MaintenanceStatus::kFailed,
                        In("journal_maintenance_failed: unexpected exception", directory));
  }
}

}  // namespace sitometron::journal
