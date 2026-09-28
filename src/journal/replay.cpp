#include "sitometron/journal/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sitometron/journal/record_codec.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::journal {
namespace {

constexpr std::string_view segment_prefix = "journal-";
constexpr std::string_view segment_suffix = ".ndjson";
constexpr std::size_t sequence_digits = 20;

std::optional<std::uint64_t> SegmentFirst(std::string_view name) {
  if (name.size() != segment_prefix.size() + sequence_digits + segment_suffix.size() ||
      !name.starts_with(segment_prefix) || !name.ends_with(segment_suffix)) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : name.substr(segment_prefix.size(), sequence_digits)) {
    if (c < '0' || c > '9') return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10U) return std::nullopt;
    value = value * 10U + digit;
  }
  if (value == 0) return std::nullopt;
  return value;
}

// JSON text of a record's payload, as the candidate normalizer expects it.
std::string PayloadJson(const core::LogicalJobEvent& record) {
  const auto encoded = EncodeRecord(record);
  if (encoded.status != EncodeStatus::kEncoded) return {};
  return nlohmann::json::parse(encoded.bytes).at("payload").dump();
}

core::Decision Derive(const core::Snapshot& before, const core::LogicalJobEvent& record) {
  using core::EventType;
  switch (record.event_type) {
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted: {
      const auto* principal = std::get_if<core::PrincipalPayload>(&record.payload);
      return core::DecideCommand(
          before,
          core::Command{record.schema_version,
                        record.event_type == EventType::kCancelAccepted
                            ? core::CommandType::kCancel
                            : core::CommandType::kTerminate,
                        record.job_id, principal != nullptr ? principal->principal_subject : ""});
    }
    case EventType::kTimeoutExpired:
    case EventType::kLateWorkerEvent:
      return core::DecideEvent(before, core::InternalEvent{record.schema_version, record.job_id,
                                                           record.event_type, record.payload});
    default: {
      const auto normalized = core::NormalizeCandidate(
          before, core::RawCandidateEvent{record.schema_version, record.job_id,
                                          std::string(core::ToString(record.event_type)),
                                          PayloadJson(record)});
      if (const auto* rejection = std::get_if<core::Rejection>(&normalized.value)) {
        return core::Decision{*rejection};
      }
      return core::DecideEvent(before, std::get<core::InternalEvent>(normalized.value));
    }
  }
}

std::string Location(std::string_view segment, std::uint64_t sequence) {
  return " at " + std::string(segment) + " sequence " + std::to_string(sequence);
}

struct Segment {
  std::uint64_t first = 0;
  std::string name;
};

}  // namespace

std::optional<core::Snapshot> ReplayRecord(const std::optional<core::Snapshot>& before,
                                           const core::LogicalJobEvent& record,
                                           std::string& error) {
  core::Snapshot current;
  if (record.event_type == core::EventType::kJobCreated) {
    if (before.has_value() && before->entity_exists) {
      error = "job_created for an existing Job";
      return std::nullopt;
    }
    const auto* created = std::get_if<core::JobCreatedPayload>(&record.payload);
    current = core::InitialSnapshot(record.job_id,
                                    created != nullptr ? created->session_id : record.job_id);
  } else if (!before.has_value() || !before->entity_exists) {
    error = "record for a Job that was never created";
    return std::nullopt;
  } else {
    current = *before;
  }
  const auto decision = Derive(current, record);
  const auto* proposal = std::get_if<core::PreEnvelopeProposal>(&decision.value);
  if (proposal == nullptr) {
    error = "the reducer rejects the record";
    return std::nullopt;
  }
  const core::LogicalJobEvent derived{proposal->schema_version, record.sequence,
                                      proposal->event_type,     record.recorded_at,
                                      proposal->job_id,         proposal->payload};
  const auto derived_bytes = EncodeRecord(derived);
  if (const auto record_bytes = EncodeRecord(record);
      derived_bytes.status != EncodeStatus::kEncoded ||
      record_bytes.status != EncodeStatus::kEncoded || derived_bytes.bytes != record_bytes.bytes) {
    error = "the reducer derives a different record";
    return std::nullopt;
  }
  auto applied = core::Apply(current, *proposal);
  if (applied.rejection.has_value()) {
    error = "the reducer rejects applying the record";
    return std::nullopt;
  }
  // Effects are discarded: replay dispatches nothing (ADR-0006 §6).
  return std::move(applied.snapshot);
}

bool IsUnresolved(const core::Snapshot& snapshot) noexcept {
  return !core::IsTerminalState(snapshot.state) ||
         snapshot.resource_status == core::ResourceStatus::kCommitted ||
         snapshot.cleanup_status == core::CleanupStatus::kPending;
}

namespace {

// One replay pass over a locked Journal directory. Each step returns a refusal or nullopt.
class Replayer {
 public:
  Replayer(FileSystem& file_system, const std::string& directory, const ReplayOptions& options)
      : file_system_(file_system), directory_(directory), options_(options) {}

  ReplayResult Run() {
    IoError error = IoError::kNone;
    const auto names = file_system_.List(directory_, error);
    if (!names.has_value()) return Refuse(ReplayStatus::kUnreadable, "journal_unreadable: list");
    std::vector<Segment> segments;
    for (const auto& name : *names) {
      if (const auto first = SegmentFirst(name); first.has_value()) {
        segments.emplace_back(Segment{*first, name});
      } else if (name.starts_with(segment_prefix) && name.ends_with(segment_suffix)) {
        // Looks like a segment but cannot be one: it may hide records (ADR-0006 §5). Other names,
        // such as journal.lock or quarantine files, are not segments and are ignored.
        return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: malformed segment name " + name);
      }
    }
    std::sort(segments.begin(), segments.end(),
              [](const Segment& a, const Segment& b) { return a.first < b.first; });
    expected_ = segments.empty() ? 1 : segments.front().first;
    for (std::size_t s = 0; s < segments.size(); ++s) {
      if (auto refusal = ReplaySegment(segments[s], s + 1 == segments.size())) return *refusal;
    }
    result_.status = ReplayStatus::kReplayed;
    result_.next_sequence = expected_;
    for (const auto& snapshot : result_.jobs) {
      if (IsUnresolved(snapshot)) result_.unresolved.push_back(snapshot.job_id);
    }
    return result_;
  }

  ReplayResult Refuse(ReplayStatus status, std::string detail) {
    result_.status = status;
    result_.detail = std::move(detail);
    result_.jobs.clear();
    result_.unresolved.clear();
    return result_;
  }

 private:
  std::optional<ReplayResult> ReplaySegment(const Segment& segment, bool highest) {
    IoError error = IoError::kNone;
    const auto content = file_system_.ReadAll(JoinPath(directory_, segment.name), error);
    if (!content.has_value()) {
      return Refuse(ReplayStatus::kUnreadable, "journal_unreadable: " + segment.name);
    }
    if (segment.first != expected_) {
      return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: segment " + segment.name +
                                                " is not named for sequence " +
                                                std::to_string(expected_));
    }
    if (content->empty() && !highest) {
      return Refuse(ReplayStatus::kCorrupt,
                    "journal_corrupt: empty segment " + segment.name + " is not the highest");
    }
    // Pruning always keeps the highest non-empty segment, so a Journal without records can only be
    // a fresh one whose empty segment is named for sequence 1 (ADR-0006 §5, §8).
    if (content->empty() && result_.records == 0 && segment.first != 1) {
      return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: empty segment " + segment.name +
                                                " without records is not named for sequence 1");
    }
    // An empty highest segment is named for the next sequence, checked above (ADR-0006 §5).
    std::size_t offset = 0;
    while (offset < content->size()) {
      const auto end = content->find('\n', offset);
      if (end == std::string::npos) {
        return highest ? Refuse(ReplayStatus::kTornTail, "journal_torn_tail: " + segment.name +
                                                             " at byte " + std::to_string(offset))
                       : Refuse(ReplayStatus::kCorrupt, "journal_corrupt: " + segment.name +
                                                            " ends without LF at byte " +
                                                            std::to_string(offset));
      }
      const auto line = std::string_view(*content).substr(offset, end + 1 - offset);
      if (auto refusal = ReplayLine(segment, offset, line)) return refusal;
      offset = end + 1;
      if (last_ == UINT64_MAX) return AfterExhaustion(segment, highest, *content, offset);
    }
    return std::nullopt;
  }

  // The record at UINT64_MAX was replayed. What follows decides the refusal: nothing is
  // exhaustion; bytes without LF in the highest segment are a torn tail (reported before
  // exhaustion, like any other torn tail); anything else is corruption.
  ReplayResult AfterExhaustion(const Segment& segment, bool highest, std::string_view content,
                               std::size_t offset) {
    if (offset == content.size() && highest) {
      return Refuse(ReplayStatus::kSequenceExhausted,
                    "journal_sequence_exhausted" + Location(segment.name, last_));
    }
    if (highest && content.find('\n', offset) == std::string_view::npos) {
      return Refuse(ReplayStatus::kTornTail,
                    "journal_torn_tail: " + segment.name + " at byte " + std::to_string(offset));
    }
    return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: record after UINT64_MAX");
  }

  std::optional<ReplayResult> ReplayLine(const Segment& segment, std::size_t offset,
                                         std::string_view line) {
    const auto decoded = DecodeRecord(line);
    if (decoded.status != DecodeStatus::kDecoded) {
      return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: " + segment.name + " byte " +
                                                std::to_string(offset) + ": " + decoded.detail);
    }
    const auto& record = decoded.event;
    if (record.sequence != expected_) {
      return Refuse(ReplayStatus::kCorrupt, "journal_corrupt: expected sequence " +
                                                std::to_string(expected_) +
                                                Location(segment.name, record.sequence));
    }
    const auto found = index_of_.find(record.job_id.value);
    const bool known = found != index_of_.end();
    std::optional<core::Snapshot> before;
    if (known) before = result_.jobs[found->second];
    std::string why;
    auto after = ReplayRecord(before, record, why);
    if (!after.has_value()) {
      return Refuse(ReplayStatus::kCorrupt,
                    "journal_corrupt: " + why + Location(segment.name, record.sequence));
    }
    if (known) {
      result_.jobs[found->second] = std::move(*after);
    } else if (result_.jobs.size() >= options_.max_jobs) {
      return Refuse(ReplayStatus::kCapacityExceeded,
                    "journal_capacity_exceeded: more than " + std::to_string(options_.max_jobs) +
                        " Jobs" + Location(segment.name, record.sequence));
    } else {
      index_of_.try_emplace(record.job_id.value, result_.jobs.size());
      result_.jobs.push_back(std::move(*after));
    }
    last_ = record.sequence;
    ++result_.records;
    if (record.sequence != UINT64_MAX) expected_ = record.sequence + 1;
    return std::nullopt;
  }

  FileSystem& file_system_;
  const std::string& directory_;
  const ReplayOptions& options_;
  ReplayResult result_;
  std::map<std::string, std::size_t, std::less<>> index_of_;  // Job id -> position in jobs
  std::uint64_t expected_ = 1;
  std::uint64_t last_ = 0;
};

}  // namespace

ReplayResult ReplayJournal(FileSystem& file_system, const std::string& directory,
                           const ReplayOptions& options) {
  Replayer replayer(file_system, directory, options);
  try {
    return replayer.Run();
  } catch (const std::exception& error) {
    return replayer.Refuse(ReplayStatus::kUnreadable,
                           std::string("journal_unreadable: ") + error.what());
  } catch (...) {
    return replayer.Refuse(ReplayStatus::kUnreadable, "journal_unreadable: unexpected exception");
  }
}

}  // namespace sitometron::journal
