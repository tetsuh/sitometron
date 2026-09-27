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
      name.substr(0, segment_prefix.size()) != segment_prefix ||
      name.substr(name.size() - segment_suffix.size()) != segment_suffix) {
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

OpenResult SegmentJournal::Open(const std::string& directory) {
  const std::lock_guard guard(mutex_);
  if (opened_ || lock_) return OpenResult{false, "journal is already open", 0, {}};
  if (file_system_.EnsureDirectory(directory) != IoError::kNone) {
    return OpenResult{false, "cannot create the Journal directory", 0, {}};
  }
  IoError error = IoError::kNone;
  auto lock = file_system_.Lock(JoinPath(directory, lock_name), error);
  if (!lock) {
    return OpenResult{false,
                      error == IoError::kLocked ? "Journal directory is locked by another owner"
                                                : "cannot lock the Journal directory",
                      0,
                      {}};
  }
  auto fail = [&](std::string detail) {
    file_system_.Close(*lock);
    return OpenResult{false, std::move(detail), 0, {}};
  };
  const auto names = file_system_.List(directory, error);
  if (!names) return fail("cannot list the Journal directory");
  std::vector<std::pair<std::uint64_t, std::string>> segments;
  for (const auto& name : *names) {
    if (const auto first = SegmentFirstSequence(name)) segments.emplace_back(*first, name);
  }
  std::sort(segments.begin(), segments.end());

  std::uint64_t next = 1;
  std::string active;
  if (!segments.empty()) {
    const auto& [first, name] = segments.back();
    const auto content = file_system_.ReadAll(JoinPath(directory, name), error);
    if (!content) return fail("cannot read the active segment " + name);
    std::string detail;
    const auto last = LastSequence(*content, detail);
    if (!detail.empty()) return fail(detail + " (" + name + ")");
    if (last) {
      if (*last < first) return fail("active segment " + name + " holds an earlier sequence");
      if (*last == UINT64_MAX) return fail("Journal sequence is exhausted");
      next = *last + 1;
      if (content->size() < options_.segment_limit_bytes) active = name;
    } else {
      // An empty highest segment is valid only when named for the next sequence (ADR-0006 §5).
      std::uint64_t expected = 1;
      if (segments.size() > 1) {
        const auto& previous = segments[segments.size() - 2];
        const auto previous_content =
            file_system_.ReadAll(JoinPath(directory, previous.second), error);
        if (!previous_content) return fail("cannot read segment " + previous.second);
        const auto previous_last = LastSequence(*previous_content, detail);
        if (!detail.empty() || !previous_last) {
          return fail("segment before the empty active segment is not usable (" + previous.second +
                      ")");
        }
        expected = *previous_last + 1;
      }
      if (first != expected) {
        return fail("empty segment " + name + " is not named for the next sequence " +
                    std::to_string(expected));
      }
      next = first;
      active = name;
    }
  }

  if (!active.empty()) {
    auto handle = file_system_.OpenAppend(JoinPath(directory, active), false, error);
    if (!handle) return fail("cannot open the active segment " + active);
    active_ = handle;
    const auto content = file_system_.ReadAll(JoinPath(directory, active), error);
    active_size_ = content ? content->size() : 0;
  }
  directory_ = directory;
  lock_ = lock;
  next_sequence_ = next;
  opened_ = true;
  poisoned_.store(false);
  return OpenResult{true, {}, next, active.empty() ? SegmentName(next) : active};
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

  const bool rotate =
      !active_ || (active_size_ > 0 && active_size_ + bytes.size() > options_.segment_limit_bytes);
  if (rotate) {
    CloseActive();
    const auto started = StartSegment(event.sequence);
    if (started != core::LogicalCommitResult::kCommitted) return started;
  }

  if (!active_) return core::LogicalCommitResult::kDefiniteFailure;
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
