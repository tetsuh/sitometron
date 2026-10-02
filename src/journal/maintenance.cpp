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
    if (replayed.status == ReplayStatus::kReplayed ||
        replayed.status == ReplayStatus::kSequenceExhausted) {
      // No torn tail is visible, but an earlier run may have cut one without making the cut
      // durable. Finish that cut before reporting the Journal.
      std::string resynced;
      if (auto failure = ResyncEarlierCut(resynced)) return *failure;
      if (replayed.status == ReplayStatus::kSequenceExhausted) {
        return Finish(MaintenanceStatus::kRefused, In(replayed.detail + resynced, directory_));
      }
      result_.next_sequence = replayed.next_sequence;
      return Finish(MaintenanceStatus::kNothingToDo, "journal_clean: " + directory_ + resynced);
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
    // A cut that reports success must have removed exactly the torn bytes: the segment is the old
    // content up to the offset, and replay finds every complete record it found before.
    if (!Holds(segment_path, std::string_view(*content).substr(0, result_.offset))) {
      return Finish(MaintenanceStatus::kFailed, Failed("verify cut", segment_path));
    }
    const auto after = ReplayAll(file_system_, directory_);
    if ((after.status != ReplayStatus::kReplayed &&
         after.status != ReplayStatus::kSequenceExhausted) ||
        after.records != replayed.records) {
      return Finish(MaintenanceStatus::kFailed, Failed("verify", In(after.detail, directory_)));
    }
    result_.sequence_exhausted = after.status == ReplayStatus::kSequenceExhausted;
    result_.next_sequence = result_.sequence_exhausted ? 0 : after.next_sequence;
    std::string detail = "journal_quarantined: ";
    detail.append(result_.segment)
        .append(" at byte ")
        .append(std::to_string(result_.offset))
        .append(" into ")
        .append(result_.quarantine);
    if (result_.sequence_exhausted) detail.append("; sequence exhausted");
    return Finish(MaintenanceStatus::kDone, In(detail, directory_));
  }

  QuarantineResult Finish(MaintenanceStatus status, std::string detail) {
    result_.status = status;
    result_.detail = std::move(detail);
    return result_;
  }

 private:
  // True when the file at `path` now holds exactly `expected`.
  bool Holds(const std::string& path, std::string_view expected) {
    IoError error = IoError::kNone;
    const auto now = file_system_.ReadAll(path, error);
    return now.has_value() && *now == expected;
  }

  // A run whose cut succeeded but whose data sync failed leaves the highest segment exactly as long
  // as the offset its quarantine file names, and replay then sees no torn tail. On disk that state
  // is indistinguishable from a completed cut, so whenever the highest segment still ends at the
  // offset of its quarantine file, the cut is repeated at the same offset with its data sync. That
  // changes no byte; it only makes the cut durable before anything is reported. `note` names the
  // segment and offset; it stays empty when no such quarantine file exists.
  std::optional<QuarantineResult> ResyncEarlierCut(std::string& note) {
    IoError error = IoError::kNone;
    const auto names = file_system_.List(directory_, error);
    if (!names.has_value()) return Finish(MaintenanceStatus::kFailed, Failed("list", directory_));
    // Replay accepted every segment-shaped name, and zero-padded names sort in sequence order.
    std::string highest;
    for (const auto& name : *names) {
      if (name.starts_with("journal-") && name.ends_with(".ndjson") && name > highest) {
        highest = name;
      }
    }
    if (highest.empty()) return std::nullopt;
    const auto path = JoinPath(directory_, highest);
    const auto content = file_system_.ReadAll(path, error);
    if (!content.has_value()) return Finish(MaintenanceStatus::kFailed, Failed("read", path));
    const auto quarantine = highest + ".torn-" + std::to_string(content->size());
    if (!Contains(*names, quarantine)) return std::nullopt;
    if (file_system_.Truncate(path, content->size()) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("truncate", path));
    }
    if (!Holds(path, *content))
      return Finish(MaintenanceStatus::kFailed, Failed("verify cut", path));
    if (file_system_.SyncDirectory(directory_) != IoError::kNone) {
      return Finish(MaintenanceStatus::kFailed, Failed("sync directory", directory_));
    }
    result_.segment = highest;
    result_.offset = content->size();
    result_.quarantine = quarantine;
    note = "; synced the cut of " + highest + " at byte " + std::to_string(content->size());
    return std::nullopt;
  }

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

bool IsSegmentName(std::string_view name) {
  return name.starts_with("journal-") && name.ends_with(".ndjson");
}

// The Journal directory as it was before an interrupted prune: its own files plus `archived`
// segments, which are read in place from archive/. Replay only lists and reads; every write-side
// call fails, so the view cannot change anything.
class MergedJournalView final : public FileSystem {
 public:
  MergedJournalView(FileSystem& base, const std::string& directory,
                    const std::vector<std::string>& archived)
      : base_(base),
        directory_(directory),
        archive_(JoinPath(directory, k_archive_directory)),
        archived_(archived) {}

  IoError EnsureDirectory(const std::string& /*directory*/) override {
    return IoError::kUnsupported;
  }
  std::optional<FileHandle> Lock(const std::string& /*path*/, IoError& error) override {
    error = IoError::kUnsupported;
    return std::nullopt;
  }
  std::optional<std::vector<std::string>> List(const std::string& directory,
                                               IoError& error) override {
    auto names = base_.List(directory, error);
    if (names.has_value() && directory == directory_) {
      names->insert(names->end(), archived_.begin(), archived_.end());
    }
    return names;
  }
  std::optional<std::string> ReadAll(const std::string& path, IoError& error) override {
    for (const auto& name : archived_) {
      if (path == JoinPath(directory_, name)) return base_.ReadAll(JoinPath(archive_, name), error);
    }
    return base_.ReadAll(path, error);
  }
  std::optional<FileHandle> OpenAppend(const std::string& /*path*/, bool /*create_new*/,
                                       IoError& error) override {
    error = IoError::kUnsupported;
    return std::nullopt;
  }
  WriteOutcome Write(FileHandle /*file*/, std::string_view /*bytes*/) noexcept override {
    return {0, IoError::kUnsupported};
  }
  IoError SyncData(FileHandle /*file*/) noexcept override { return IoError::kUnsupported; }
  IoError SyncDirectory(const std::string& /*directory*/) noexcept override {
    return IoError::kUnsupported;
  }
  void Close(FileHandle /*file*/) noexcept override {}

 private:
  FileSystem& base_;
  const std::string& directory_;
  std::string archive_;
  const std::vector<std::string>& archived_;
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
    auto replayed = ReplayAll(file_system_, directory_);
    if (replayed.status != ReplayStatus::kReplayed) {
      auto resumed = ReplayInterrupted(replayed);
      if (!resumed.has_value()) {
        return Finish(MaintenanceStatus::kRefused, In(replayed.detail, directory_));
      }
      replayed = std::move(*resumed);
    }
    result_.next_sequence = replayed.next_sequence;
    const auto length = PrunableLength(replayed);
    const auto archived = result_.resumed.size();
    if (archived != 0 && length <= archived) {
      // The archived run explains the refusal but is not a prunable prefix: nothing to resume.
      result_.resumed.clear();
      return Finish(MaintenanceStatus::kRefused,
                    In("journal_corrupt: archived segments before the Journal are not a prunable "
                       "prefix",
                       directory_));
    }
    if (length == 0) {
      return Finish(
          MaintenanceStatus::kNothingToDo,
          In("journal_nothing_to_prune: no sealed prefix holds only closed Jobs", directory_));
    }
    std::uint64_t prefix_records = 0;
    for (std::size_t s = 0; s < length; ++s) {
      prefix_records += replayed.segments[s].records;
      if (s < archived) continue;  // already in archive/ from the interrupted run
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
        after.records + prefix_records != replayed.records) {
      return Finish(MaintenanceStatus::kFailed, Failed("verify", In(after.detail, directory_)));
    }
    return Finish(MaintenanceStatus::kDone, Summary("journal_pruned"));
  }

  PruneResult Finish(MaintenanceStatus status, std::string detail) {
    result_.status = status;
    result_.detail = std::move(detail);
    if (status == MaintenanceStatus::kRefused || status == MaintenanceStatus::kFailed) {
      // Nothing is reported as planned or completed. `moved` still names what this run already
      // moved, and running prune again completes it.
      result_.segments.clear();
      result_.jobs.clear();
      result_.records = 0;
      result_.first_retained_sequence = 0;
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

  // A prune interrupted between moves can leave the first retained segment holding records of a
  // Job whose creation is already in archive/; replay then refuses with "never created". The
  // shortest run of archived segments directly before the Journal that makes the whole replay
  // succeed is the interrupted prefix. Returns that replay and records the run in `resumed`, or
  // nullopt when the refusal has another cause.
  std::optional<ReplayResult> ReplayInterrupted(const ReplayResult& refused) {
    const auto interrupted = [](const ReplayResult& r) {
      return r.status == ReplayStatus::kCorrupt &&
             r.detail.find("record for a Job that was never created") != std::string::npos;
    };
    if (!interrupted(refused)) return std::nullopt;
    IoError error = IoError::kNone;
    const auto names = file_system_.List(directory_, error);
    const auto archive = JoinPath(directory_, k_archive_directory);
    const auto archived = file_system_.List(archive, error);
    if (!names.has_value() || !archived.has_value()) return std::nullopt;
    std::string first;
    for (const auto& name : *names) {
      if (IsSegmentName(name) && (first.empty() || name < first)) first = name;
    }
    // Zero-padded segment names sort in sequence order.
    std::vector<std::string> before;
    for (const auto& name : *archived) {
      if (IsSegmentName(name) && name < first) before.push_back(name);
    }
    std::sort(before.begin(), before.end());
    for (std::size_t n = 1; n <= before.size(); ++n) {
      const std::vector<std::string> run(before.end() - static_cast<std::ptrdiff_t>(n),
                                         before.end());
      MergedJournalView view(file_system_, directory_, run);
      auto merged = ReplayAll(view, directory_);
      if (merged.status == ReplayStatus::kReplayed) {
        result_.resumed = run;
        return merged;
      }
      // Only a missing creation can be explained by a longer archived run.
      if (!interrupted(merged)) return std::nullopt;
    }
    return std::nullopt;
  }

  // Moves the prefix into the archive directory, lowest first, making both directory entries
  // durable after each move. When a Job spans a boundary inside the prefix, a failure or crash
  // between moves leaves a Journal that replay refuses until prune is run again, which resumes from
  // archive/ (ReplayInterrupted).
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
      result_.moved.push_back(name);
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
    if (!result_.resumed.empty()) {
      detail.append("; completes an interrupted prune after ")
          .append(std::to_string(result_.resumed.size()))
          .append(" archived segments");
    }
    return In(detail, directory_);
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
