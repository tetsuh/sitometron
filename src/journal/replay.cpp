#include "sitometron/journal/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
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
  const auto record_bytes = EncodeRecord(record);
  if (derived_bytes.status != EncodeStatus::kEncoded ||
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

ReplayResult ReplayJournal(FileSystem& file_system, const std::string& directory,
                           const ReplayOptions& options) {
  ReplayResult result;
  auto refuse = [&result](ReplayStatus status, std::string detail) {
    result.status = status;
    result.detail = std::move(detail);
    result.jobs.clear();
    result.unresolved.clear();
    return result;
  };
  try {
    IoError error = IoError::kNone;
    const auto names = file_system.List(directory, error);
    if (!names.has_value()) return refuse(ReplayStatus::kUnreadable, "journal_unreadable: list");
    std::vector<Segment> segments;
    for (const auto& name : *names) {
      if (const auto first = SegmentFirst(name); first.has_value()) {
        segments.push_back(Segment{*first, name});
      }
    }
    std::sort(segments.begin(), segments.end(),
              [](const Segment& a, const Segment& b) { return a.first < b.first; });

    std::map<std::string, std::size_t> index_of;  // Job id -> position in result.jobs
    std::uint64_t expected = segments.empty() ? 1 : segments.front().first;
    std::uint64_t last = 0;
    for (std::size_t s = 0; s < segments.size(); ++s) {
      const auto& segment = segments[s];
      const bool highest = s + 1 == segments.size();
      const auto content = file_system.ReadAll(JoinPath(directory, segment.name), error);
      if (!content.has_value()) {
        return refuse(ReplayStatus::kUnreadable, "journal_unreadable: " + segment.name);
      }
      if (segment.first != expected) {
        return refuse(ReplayStatus::kCorrupt, "journal_corrupt: segment " + segment.name +
                                                  " is not named for sequence " +
                                                  std::to_string(expected));
      }
      if (content->empty()) {
        if (!highest) {
          return refuse(ReplayStatus::kCorrupt,
                        "journal_corrupt: empty segment " + segment.name + " is not the highest");
        }
        continue;  // named for the next sequence, checked above (ADR-0006 §5)
      }
      std::size_t offset = 0;
      while (offset < content->size()) {
        const auto end = content->find('\n', offset);
        if (end == std::string::npos) {
          if (highest) {
            return refuse(ReplayStatus::kTornTail, "journal_torn_tail: " + segment.name +
                                                       " at byte " + std::to_string(offset));
          }
          return refuse(ReplayStatus::kCorrupt, "journal_corrupt: " + segment.name +
                                                    " ends without LF at byte " +
                                                    std::to_string(offset));
        }
        const auto line = std::string_view(*content).substr(offset, end + 1 - offset);
        const auto decoded = DecodeRecord(line);
        if (decoded.status != DecodeStatus::kDecoded) {
          return refuse(ReplayStatus::kCorrupt, "journal_corrupt: " + segment.name + " byte " +
                                                    std::to_string(offset) + ": " + decoded.detail);
        }
        const auto& record = decoded.event;
        if (record.sequence != expected) {
          return refuse(ReplayStatus::kCorrupt, "journal_corrupt: expected sequence " +
                                                    std::to_string(expected) +
                                                    Location(segment.name, record.sequence));
        }
        const auto found = index_of.find(record.job_id.value);
        std::optional<core::Snapshot> before;
        if (found != index_of.end()) before = result.jobs[found->second];
        std::string why;
        auto after = ReplayRecord(before, record, why);
        if (!after.has_value()) {
          return refuse(ReplayStatus::kCorrupt,
                        "journal_corrupt: " + why + Location(segment.name, record.sequence));
        }
        if (found == index_of.end()) {
          if (result.jobs.size() >= options.max_jobs) {
            return refuse(ReplayStatus::kCapacityExceeded,
                          "journal_capacity_exceeded: more than " +
                              std::to_string(options.max_jobs) + " Jobs" +
                              Location(segment.name, record.sequence));
          }
          index_of.emplace(record.job_id.value, result.jobs.size());
          result.jobs.push_back(std::move(*after));
        } else {
          result.jobs[found->second] = std::move(*after);
        }
        last = record.sequence;
        ++result.records;
        offset = end + 1;
        if (record.sequence == UINT64_MAX) {
          if (offset != content->size() || !highest) {
            return refuse(ReplayStatus::kCorrupt, "journal_corrupt: record after UINT64_MAX");
          }
          return refuse(ReplayStatus::kSequenceExhausted,
                        "journal_sequence_exhausted" + Location(segment.name, last));
        }
        expected = record.sequence + 1;
      }
    }
    result.status = ReplayStatus::kReplayed;
    result.next_sequence = expected;
    for (const auto& snapshot : result.jobs) {
      if (IsUnresolved(snapshot)) result.unresolved.push_back(snapshot.job_id);
    }
    return result;
  } catch (const std::exception& error) {
    return refuse(ReplayStatus::kUnreadable, std::string("journal_unreadable: ") + error.what());
  } catch (...) {
    return refuse(ReplayStatus::kUnreadable, "journal_unreadable: unexpected exception");
  }
}

}  // namespace sitometron::journal
