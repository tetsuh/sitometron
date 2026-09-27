#include "sitometron/journal/segment_journal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitometron/journal/record_codec.hpp"

namespace sitometron::journal {
namespace {

constexpr std::string_view segment_prefix = "journal-";
constexpr std::string_view segment_suffix = ".ndjson";
constexpr std::string_view lock_name = "journal.lock";
constexpr std::size_t sequence_digits = 20;

// Parses `journal-<20 digits>.ndjson`; other names are not segments.
std::optional<std::uint64_t> SegmentFirstSequence(std::string_view name) {
  if (name.size() != segment_prefix.size() + sequence_digits + segment_suffix.size() ||
      !name.starts_with(segment_prefix) || !name.ends_with(segment_suffix)) {
    return std::nullopt;
  }
  const auto digits = name.substr(segment_prefix.size(), sequence_digits);
  std::uint64_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10U) return std::nullopt;
    value = value * 10U + digit;
  }
  if (value == 0) return std::nullopt;
  return value;
}

// Sequence of the last complete record in a segment's content, or an error.
std::optional<std::uint64_t> LastSequence(std::string_view content, std::string& error) {
  if (content.empty()) return std::nullopt;
  if (content.back() != '\n') {
    error = "active segment ends with a torn record";
    return std::nullopt;
  }
  const auto body = content.substr(0, content.size() - 1);
  const auto start = body.rfind('\n');
  const auto last = content.substr(start == std::string_view::npos ? 0 : start + 1);
  const auto decoded = DecodeRecord(last);
  if (decoded.status != DecodeStatus::kDecoded) {
    error = "last record of the active segment does not decode: " + decoded.detail;
    return std::nullopt;
  }
  return decoded.event.sequence;
}

}  // namespace

SegmentJournal::SegmentJournal(FileSystem& file_system, SegmentJournalOptions options)
    : file_system_(file_system), options_(options) {}

SegmentJournal::~SegmentJournal() {
  const std::lock_guard lock(mutex_);
  CloseActive();
  if (lock_) {
    file_system_.Close(*lock_);
    lock_.reset();
  }
}

std::string SegmentJournal::SegmentName(std::uint64_t first_sequence) {
  auto digits = std::to_string(first_sequence);
  if (digits.size() < sequence_digits) digits.insert(0, sequence_digits - digits.size(), '0');
  return std::string(segment_prefix) + digits + std::string(segment_suffix);
}

SegmentJournal::Located SegmentJournal::Locate(const std::string& directory) {
  IoError error = IoError::kNone;
  const auto names = file_system_.List(directory, error);
  if (!names.has_value()) return Located{"cannot list the Journal directory", 0, {}};
  std::vector<std::pair<std::uint64_t, std::string>> segments;
  for (const auto& name : *names) {
    if (const auto first = SegmentFirstSequence(name); first.has_value()) {
      segments.emplace_back(*first, name);
    }
  }
  if (segments.empty()) return Located{{}, 1, {}};
  std::sort(segments.begin(), segments.end());

  const auto& [first, name] = segments.back();
  const auto content = file_system_.ReadAll(JoinPath(directory, name), error);
  if (!content.has_value()) return Located{"cannot read the active segment " + name, 0, {}};
  std::string detail;
  const auto last = LastSequence(*content, detail);
  if (!detail.empty()) return Located{detail + " (" + name + ")", 0, {}};
  if (last.has_value()) {
    if (*last < first)
      return Located{"active segment " + name + " holds an earlier sequence", 0, {}};
    if (*last == UINT64_MAX) return Located{"Journal sequence is exhausted", 0, {}};
    // A full active segment is left sealed; the next record starts a new one.
    if (content->size() >= options_.segment_limit_bytes) return Located{{}, *last + 1, {}, 0};
    return Located{{}, *last + 1, name, content->size()};
  }

  // An empty highest segment is valid only when named for the next sequence (ADR-0006 §5).
  std::uint64_t expected = 1;
  if (segments.size() > 1) {
    const auto& previous = segments[segments.size() - 2].second;
    const auto previous_content = file_system_.ReadAll(JoinPath(directory, previous), error);
    const auto previous_last =
        previous_content.has_value() ? LastSequence(*previous_content, detail) : std::nullopt;
    if (!detail.empty() || !previous_last.has_value()) {
      return Located{
          "segment before the empty active segment is not usable (" + previous + ")", 0, {}};
    }
    expected = *previous_last + 1;
  }
  if (first != expected) {
    return Located{
        "empty segment " + name + " is not named for the next sequence " + std::to_string(expected),
        0,
        {}};
  }
  return Located{{}, first, name, 0};
}

OpenResult SegmentJournal::Open(const std::string& directory) {
  const std::lock_guard guard(mutex_);
  if (opened_ || lock_.has_value()) return OpenResult{false, "journal is already open", 0, {}};
  if (file_system_.EnsureDirectory(directory) != IoError::kNone) {
    return OpenResult{false, "cannot create the Journal directory", 0, {}};
  }
  IoError error = IoError::kNone;
  const auto lock = file_system_.Lock(JoinPath(directory, lock_name), error);
  if (!lock.has_value()) {
    return OpenResult{false,
                      error == IoError::kLocked ? "Journal directory is locked by another owner"
                                                : "cannot lock the Journal directory",
                      0,
                      {}};
  }
  const auto located = Locate(directory);
  if (!located.error.empty()) {
    file_system_.Close(*lock);
    return OpenResult{false, located.error, 0, {}};
  }
  if (!located.active.empty()) {
    const auto path = JoinPath(directory, located.active);
    const auto handle = file_system_.OpenAppend(path, false, error);
    if (!handle.has_value()) {
      file_system_.Close(*lock);
      return OpenResult{false, "cannot open the active segment " + located.active, 0, {}};
    }
    active_ = handle;
    active_size_ = located.active_size;
  }
  directory_ = directory;
  lock_ = lock;
  next_sequence_ = located.next;
  opened_ = true;
  poisoned_.store(false);
  return OpenResult{
      true, {}, located.next, located.active.empty() ? SegmentName(located.next) : located.active};
}

core::LogicalCommitResult SegmentJournal::Commit(const core::LogicalJobEvent& event) noexcept {
  try {
    const std::lock_guard guard(mutex_);
    if (!opened_ || poisoned_.load()) return core::LogicalCommitResult::kDefiniteFailure;
    const auto outcome = CommitLocked(event);
    if (outcome != core::LogicalCommitResult::kCommitted) poisoned_.store(true);
    return outcome;
  } catch (...) {
    // Nothing observable distinguishes a pre-write failure here; poison conservatively.
    poisoned_.store(true);
    return core::LogicalCommitResult::kOutcomeUnknown;
  }
}

core::LogicalCommitResult SegmentJournal::CommitLocked(const core::LogicalJobEvent& event) {
  if (event.sequence != next_sequence_) return core::LogicalCommitResult::kDefiniteFailure;
  const auto encoded = EncodeRecord(event);
  if (encoded.status != EncodeStatus::kEncoded) return core::LogicalCommitResult::kDefiniteFailure;
  const std::string_view bytes = encoded.bytes;

  if (const bool rotate =
          !active_.has_value() ||
          (active_size_ > 0 && active_size_ + bytes.size() > options_.segment_limit_bytes);
      rotate) {
    CloseActive();
    const auto started = StartSegment(event.sequence);
    if (started != core::LogicalCommitResult::kCommitted) return started;
  }

  if (!active_.has_value()) return core::LogicalCommitResult::kDefiniteFailure;
  const FileHandle file = *active_;
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto outcome = file_system_.Write(file, bytes.substr(written));
    written += outcome.written;
    active_size_ += outcome.written;
    if (outcome.error == IoError::kInterrupted) continue;
    if (outcome.error != IoError::kNone) {
      return written == 0 ? core::LogicalCommitResult::kDefiniteFailure
                          : core::LogicalCommitResult::kOutcomeUnknown;
    }
    if (outcome.written == 0) {
      return written == 0 ? core::LogicalCommitResult::kDefiniteFailure
                          : core::LogicalCommitResult::kOutcomeUnknown;
    }
  }
  if (file_system_.SyncData(file) != IoError::kNone) {
    return core::LogicalCommitResult::kOutcomeUnknown;
  }
  ++next_sequence_;
  return core::LogicalCommitResult::kCommitted;
}

core::LogicalCommitResult SegmentJournal::StartSegment(std::uint64_t first_sequence) {
  IoError error = IoError::kNone;
  const auto path = JoinPath(directory_, SegmentName(first_sequence));
  auto handle = file_system_.OpenAppend(path, true, error);
  if (!handle) return core::LogicalCommitResult::kDefiniteFailure;
  active_ = handle;
  active_size_ = 0;
  if (file_system_.SyncData(*active_) != IoError::kNone ||
      file_system_.SyncDirectory(directory_) != IoError::kNone) {
    // No byte of the record was written; the empty segment is valid at the next start.
    return core::LogicalCommitResult::kDefiniteFailure;
  }
  return core::LogicalCommitResult::kCommitted;
}

void SegmentJournal::CloseActive() noexcept {
  if (active_) {
    file_system_.Close(*active_);
    active_.reset();
  }
}

bool SegmentJournal::Poisoned() const { return poisoned_.load(); }

std::uint64_t SegmentJournal::NextSequence() const {
  const std::lock_guard guard(mutex_);
  return next_sequence_;
}

}  // namespace sitometron::journal
