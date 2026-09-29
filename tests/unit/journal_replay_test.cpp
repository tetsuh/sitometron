#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "job_orchestrator.hpp"
#include "memory_file_system.hpp"
#include "reducer_snapshot_json.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/record_codec.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::test {
namespace {
using Json = nlohmann::json;
using namespace sitometron::core;
using namespace sitometron::journal;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "journal_replay: " << message << '\n';
  return 1;
}

// "<id> <what>: <detail>" without temporary string concatenations.
struct Failure {
  std::string_view id;
  std::string_view what;
  std::string_view detail;
};
std::string Why(const Failure& failure) {
  std::string message(failure.id);
  message.append(" ").append(failure.what).append(": ").append(failure.detail);
  return message;
}

const std::string k_dir = "/journal";

std::string SegmentPath(std::uint64_t first) {
  return JoinPath(k_dir, SegmentJournal::SegmentName(first));
}

using Ordered = nlohmann::ordered_json;

// A fixture Journal event read with its member order preserved is its canonical record.
std::optional<LogicalJobEvent> EventFromFixture(const Ordered& fixture, std::string& error) {
  const auto decoded = DecodeRecord(fixture.dump() + "\n");
  if (decoded.status != DecodeStatus::kDecoded) {
    error = decoded.detail;
    return std::nullopt;
  }
  return decoded.event;
}

std::string Job(int n) {
  std::string suffix = std::to_string(n);
  return "01890f3e-7b00-7abc-8abc-" + std::string(12 - suffix.size(), '0') + suffix;
}

LogicalJobEvent Created(std::uint64_t sequence, int job) {
  return LogicalJobEvent{1,
                         sequence,
                         EventType::kJobCreated,
                         DiagnosticTimestamp{"2026-09-28T01:02:03Z"},
                         Uuid{Job(job)},
                         JobCreatedPayload{Uuid{Job(job)}}};
}

LogicalJobEvent Cancelled(std::uint64_t sequence, int job) {
  return LogicalJobEvent{1,
                         sequence,
                         EventType::kCancelAccepted,
                         DiagnosticTimestamp{"2026-09-28T01:02:04Z"},
                         Uuid{Job(job)},
                         PrincipalPayload{"operator@example"}};
}

std::string Bytes(const LogicalJobEvent& event) { return EncodeRecord(event).bytes; }

MemoryFileSystem Journal(const std::vector<std::pair<std::uint64_t, std::string>>& segments) {
  MemoryFileSystem fs;
  fs.directories.insert(k_dir);
  for (const auto& [first, content] : segments) fs.files[SegmentPath(first)] = content;
  return fs;
}

ReplayResult Replay(MemoryFileSystem& fs, std::size_t max_jobs = 8) {
  return ReplayJournal(fs, k_dir, ReplayOptions{max_jobs});
}

bool Refused(const ReplayResult& result, ReplayStatus status, std::string_view code) {
  return result.status == status && result.detail.find(code) != std::string::npos;
}

// Port fakes for an orchestrator writing to a SegmentJournal: they record nothing and fail nothing.
struct FakePorts {
  class Clock final : public ClockPort {
   public:
    ClockReading Read() override {
      return {DiagnosticTimestamp{"2026-09-28T01:02:03.456Z"}, MonotonicInstant{++tick_}};
    }

   private:
    std::uint64_t tick_ = 0;
  } clock;
  class Runner final : public ApplicationRunnerPort {
   public:
    void HandoffLaunch(ApplicationLaunchRequest&&) noexcept override {}
    void HandoffCooperativeStop(ApplicationStopRequest&&) noexcept override {}
    void HandoffForcedStop(ApplicationStopRequest&&) noexcept override {}
  } runner;
  class Session final : public SessionRetainerPort {
   public:
    void HandoffRetainSameIdentity(SessionRetainRequest&&) noexcept override {}
  } session;
  class Identity final : public IdentitySourcePort {
   public:
    JobSessionIdentityResult GenerateJobSessionIdentity() override {
      return GeneratedJobSessionIdentity{Uuid{Job(++jobs_)}};
    }
    WorkerIdentityResult GenerateWorkerIdentity() override {
      return GeneratedWorkerIdentity{Uuid{"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"}};
    }
    LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
      return GeneratedLaunchOperationIdentity{StableId{"launch-1"}};
    }

   private:
    int jobs_ = 0;
  } identity;
};

int ReproducesVectors(const Json& vectors, const Ordered& ordered) {
  int result = 0;
  std::size_t replayed = 0;
  std::map<std::string, Json> applied_by_event;
  std::vector<std::tuple<std::string, std::string, Json>> command_results;
  const auto& cases = vectors.at("case_vectors");
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& vector = cases[index];
    const auto& expected = vector.at("expected");
    if (expected.at("journal_event").is_null()) continue;
    const auto id = vector.at("vector_id").get<std::string>();
    std::string error;
    const auto record = EventFromFixture(
        ordered.at("case_vectors")[index].at("expected").at("journal_event"), error);
    result |= Check(record.has_value(), Why({id, "fixture decodes", error}));
    if (!record) continue;
    const auto& initial = vector.at("initial_snapshot");
    const std::optional<Snapshot> before =
        initial.is_null() ? std::nullopt : std::optional<Snapshot>{SnapshotFrom(initial)};
    const auto after = ReplayRecord(before, *record, error);
    result |= Check(after.has_value(), Why({id, "replays", error}));
    ++replayed;
    // An event vector pins the snapshot after applying its Journal event. A command vector's own
    // next_snapshot is the pre-application decision state (the command changes nothing until its
    // event is applied), so it is checked against the event vector with the same initial snapshot
    // and Journal event instead.
    if (after && !expected.at("next_snapshot").is_null()) {
      const auto key = vector.at("initial_snapshot").dump() + expected.at("journal_event").dump();
      if (vector.at("matrix") == "event") {
        applied_by_event[key] = expected.at("next_snapshot");
        result |= Check(SnapshotJson(*after) == expected.at("next_snapshot"),
                        id + " snapshot (actual=" + SnapshotJson(*after).dump() + ")");
      } else {
        command_results.emplace_back(id, key, SnapshotJson(*after));
      }
    }
  }
  std::size_t matched = 0;
  for (const auto& [id, key, actual] : command_results) {
    const auto found = applied_by_event.find(key);
    result |= Check(found != applied_by_event.end(), id + " has an event vector for its record");
    if (found == applied_by_event.end()) continue;
    ++matched;
    result |=
        Check(actual == found->second,
              id + " applied snapshot equals the event vector (actual=" + actual.dump() + ")");
  }
  result |=
      Check(matched >= 8, "command vectors matched to event vectors: " + std::to_string(matched));
  result |= Check(replayed > 100, "replayed " + std::to_string(replayed) + " case vectors");
  const auto& sequences = vectors.at("sequence_vectors");
  for (std::size_t index = 0; index < sequences.size(); ++index) {
    const auto& sequence = sequences[index];
    const auto id = sequence.at("vector_id").get<std::string>();
    std::optional<Snapshot> current = SnapshotFrom(sequence.at("initial_snapshot"));
    const auto& steps = sequence.at("steps");
    for (std::size_t step = 0; step < steps.size(); ++step) {
      if (steps[step].at("expected").at("journal_event").is_null()) continue;
      std::string error;
      const auto record = EventFromFixture(ordered.at("sequence_vectors")[index]
                                               .at("steps")[step]
                                               .at("expected")
                                               .at("journal_event"),
                                           error);
      result |= Check(record.has_value(), Why({id, "step decodes", error}));
      if (!record) break;
      current = ReplayRecord(current, *record, error);
      result |= Check(current.has_value(), Why({id, "step replays", error}));
      if (!current) break;
    }
    if (current) {
      result |= Check(SnapshotJson(*current) == sequence.at("expected_final_snapshot"),
                      id + " final snapshot (actual=" + SnapshotJson(*current).dump() + ")");
    }
  }
  // A Journal written by the orchestrator through SegmentJournal replays to its own snapshots.
  MemoryFileSystem fs;
  {
    FakePorts fakes;
    SegmentJournal journal(fs);
    result |= Check(journal.Open(k_dir).ok, "orchestrator journal opens");
    core::internal::Config config;
    config.max_jobs = 4;
    config.normal_capacity = 4;
    config.trace_capacity = config.total_capacity();
    config.completion_capacity = config.total_capacity();
    config.handoff_capacity = 4;
    config.ack_capacity = 4;
    config.callback_registration_capacity = 4;
    config.ports = {&fakes.clock, &journal, &fakes.runner, &fakes.session, &fakes.identity};
    core::internal::JobOrchestrator orchestrator(config);
    std::vector<Uuid> created;
    for (int i = 0; i < 3; ++i) {
      const auto admitted = orchestrator.Create();
      (void)orchestrator.WaitUntil(admitted.ingress_sequence,
                                   core::internal::WriterPhase::kTurnFinished);
      (void)orchestrator.TakeCompletion(admitted.ingress_sequence);
      if (const auto id = orchestrator.LastCreated()) created.push_back(*id);
    }
    const auto cancel = orchestrator.SubmitCommand(
        Command{1, CommandType::kCancel, created.at(1), "operator@example"});
    (void)orchestrator.WaitUntil(cancel.ingress_sequence,
                                 core::internal::WriterPhase::kTurnFinished);
    std::vector<Json> live;
    for (const auto& id : created) {
      const auto snapshot = orchestrator.SnapshotFor(id);
      live.push_back(snapshot ? SnapshotJson(*snapshot) : Json());
    }
    (void)orchestrator.BeginShutdown();
    const auto replayed_journal = Replay(fs);
    result |= Check(replayed_journal.status == ReplayStatus::kReplayed,
                    "orchestrator Journal replays: " + replayed_journal.detail);
    result |= Check(replayed_journal.jobs.size() == live.size(), "one snapshot per created Job");
    for (std::size_t i = 0; i < live.size() && i < replayed_journal.jobs.size(); ++i) {
      result |= Check(SnapshotJson(replayed_journal.jobs[i]) == live[i],
                      "replayed snapshot equals the orchestrator's for Job " + std::to_string(i));
    }
  }
  return result;
}

int TornTailRefusal() {
  int result = 0;
  auto fs = Journal({{1, Bytes(Created(1, 1)) + Bytes(Created(2, 2)).substr(0, 20)}});
  const auto replayed = Replay(fs);
  const auto expected = "journal_torn_tail: " + SegmentJournal::SegmentName(1) + " at byte " +
                        std::to_string(Bytes(Created(1, 1)).size());
  result |= Check(replayed.status == ReplayStatus::kTornTail && replayed.detail == expected,
                  "torn tail names the segment and exact byte offset: " + replayed.detail);
  return result;
}

int CorruptionRefusal() {
  int result = 0;
  struct Case {
    std::string name;
    std::vector<std::pair<std::uint64_t, std::string>> segments;
    std::string location;  // the part of the refusal detail that pins where the problem is
  };
  const auto seg1 = SegmentJournal::SegmentName(1);
  const auto first_record = std::to_string(Bytes(Created(1, 1)).size());
  const std::vector<Case> cases{
      {"undecodable line mid-Journal",
       {{1, Bytes(Created(1, 1)) + "{\"not\":\"a record\"}\n" + Bytes(Created(3, 3))}},
       seg1 + " byte " + first_record + ":"},
      {"torn line in a sealed segment",
       {{1, Bytes(Created(1, 1)).substr(0, 30) + "\n"}, {2, Bytes(Created(2, 2))}},
       seg1 + " byte 0:"},
      {"sequence gap",
       {{1, Bytes(Created(1, 1)) + Bytes(Created(3, 3))}},
       "expected sequence 2 at " + seg1 + " sequence 3"},
      {"repeated sequence",
       {{1, Bytes(Created(1, 1)) + Bytes(Created(1, 2))}},
       "expected sequence 2 at " + seg1 + " sequence 1"},
      {"gap across segments",
       {{1, Bytes(Created(1, 1))}, {3, Bytes(Created(3, 3))}},
       "segment " + SegmentJournal::SegmentName(3) + " is not named for sequence 2"},
      {"record for an absent Job", {{1, Bytes(Cancelled(1, 1))}}, " at " + seg1 + " sequence 1"},
      {"duplicate job_created",
       {{1, Bytes(Created(1, 1)) + Bytes(Created(2, 1))}},
       " at " + seg1 + " sequence 2"},
      {"reducer rejects the record",
       {{1, Bytes(Created(1, 1)) +
                Bytes(LogicalJobEvent{1, 2, EventType::kTerminalOutcomeCommitted,
                                      DiagnosticTimestamp{"2026-09-28T01:02:05Z"}, Uuid{Job(1)},
                                      TerminalOutcomePayload{TerminalOutcome::kSucceeded}})}},
       " at " + seg1 + " sequence 2"},
      {"empty non-highest segment",
       {{1, ""}, {2, Bytes(Created(2, 2))}},
       "empty segment " + seg1 + " is not the highest"},
  };
  for (const auto& c : cases) {
    auto fs = Journal(c.segments);
    const auto replayed = Replay(fs);
    result |= Check(Refused(replayed, ReplayStatus::kCorrupt, "journal_corrupt") &&
                        replayed.detail.find(c.location) != std::string::npos,
                    c.name + " (expected location '" + c.location + "'): " + replayed.detail);
  }
  return result;
}

int SegmentNameMismatch() {
  int result = 0;
  auto fs = Journal({{5, Bytes(Created(1, 1))}});
  const auto replayed = Replay(fs);
  result |= Check(Refused(replayed, ReplayStatus::kCorrupt, "journal_corrupt"),
                  "first record differs from the segment name: " + replayed.detail);
  auto later = Journal({{1, Bytes(Created(1, 1))}, {3, Bytes(Created(2, 2))}});
  const auto second = Replay(later);
  result |= Check(Refused(second, ReplayStatus::kCorrupt, "journal_corrupt"),
                  "later segment named past its first record: " + second.detail);
  for (const std::string bad : {"journal-12.ndjson", "journal-0000000000000000000x.ndjson",
                                "journal-00000000000000000000.ndjson"}) {
    auto lone = Journal({});
    lone.files[JoinPath(k_dir, bad)] = Bytes(Created(1, 1));
    result |= Check(Refused(Replay(lone), ReplayStatus::kCorrupt, "journal_corrupt"),
                    "a lone malformed segment name is corrupt: " + bad);
    auto hidden = Journal({{1, Bytes(Created(1, 1))}});
    hidden.files[JoinPath(k_dir, bad)] = Bytes(Created(2, 2));
    result |= Check(Refused(Replay(hidden), ReplayStatus::kCorrupt, "journal_corrupt"),
                    "a malformed next segment is corrupt: " + bad);
  }
  auto unrelated = Journal({{1, Bytes(Created(1, 1))}});
  unrelated.files[JoinPath(k_dir, "journal.lock")] = "";
  unrelated.files[JoinPath(k_dir, "journal-00000000000000000002.ndjson.quarantine")] = "x";
  result |= Check(Replay(unrelated).status == ReplayStatus::kReplayed,
                  "the lock and non-segment files are ignored");
  return result;
}

int CapacityRefusal() {
  int result = 0;
  auto fs = Journal({{1, Bytes(Created(1, 1)) + Bytes(Created(2, 2)) + Bytes(Created(3, 3))}});
  result |= Check(Replay(fs, 3).status == ReplayStatus::kReplayed, "exactly max_jobs replays");
  const auto over = Replay(fs, 2);
  result |= Check(Refused(over, ReplayStatus::kCapacityExceeded, "journal_capacity_exceeded"),
                  "one Job over capacity: " + over.detail);
  return result;
}

int SequenceExhaustedRefusal() {
  int result = 0;
  auto fs = Journal({{UINT64_MAX, Bytes(Created(UINT64_MAX, 1))}});
  const auto replayed = Replay(fs);
  result |= Check(Refused(replayed, ReplayStatus::kSequenceExhausted, "journal_sequence_exhausted"),
                  "last sequence is UINT64_MAX: " + replayed.detail);
  auto torn = Journal({{UINT64_MAX, Bytes(Created(UINT64_MAX, 1)) + "{\"sch"}});
  const auto torn_after = Replay(torn);
  result |=
      Check(Refused(torn_after, ReplayStatus::kTornTail, "journal_torn_tail") &&
                torn_after.detail.find("byte") != std::string::npos,
            "a partial line after the UINT64_MAX record is a torn tail: " + torn_after.detail);
  auto extra = Journal({{UINT64_MAX, Bytes(Created(UINT64_MAX, 1)) + "{}\n"}});
  const auto after_max = Replay(extra);
  const auto max_location = SegmentJournal::SegmentName(UINT64_MAX) + " at byte " +
                            std::to_string(Bytes(Created(UINT64_MAX, 1)).size());
  result |= Check(Refused(after_max, ReplayStatus::kCorrupt, "journal_corrupt") &&
                      after_max.detail.find(max_location) != std::string::npos,
                  "a complete line after the UINT64_MAX record is corruption at its location: " +
                      after_max.detail);
  return result;
}

int EmptyActiveSegment() {
  int result = 0;
  auto fs = Journal({{1, Bytes(Created(1, 1)) + Bytes(Created(2, 2))}, {3, ""}});
  const auto replayed = Replay(fs);
  result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 3,
                  "empty highest segment named for the next sequence: " + replayed.detail);
  auto misnamed = Journal({{1, Bytes(Created(1, 1)) + Bytes(Created(2, 2))}, {4, ""}});
  result |= Check(Refused(Replay(misnamed), ReplayStatus::kCorrupt, "journal_corrupt"),
                  "empty highest segment not named for the next sequence");
  auto only = Journal({{1, ""}});
  const auto fresh = Replay(only);
  result |= Check(
      fresh.status == ReplayStatus::kReplayed && fresh.next_sequence == 1 && fresh.jobs.empty(),
      "a lone empty first segment is a fresh Journal: " + fresh.detail);
  auto lone_later = Journal({{5, ""}});
  result |= Check(Refused(Replay(lone_later), ReplayStatus::kCorrupt, "journal_corrupt"),
                  "a lone empty segment not named for sequence 1 is corrupt");
  auto pruned = Journal({{5, Bytes(Created(5, 1))}, {6, ""}});
  const auto kept = Replay(pruned);
  result |= Check(
      kept.status == ReplayStatus::kReplayed && kept.next_sequence == 6,
      "a pruned Journal starting above 1 with an empty active segment replays: " + kept.detail);
  return result;
}

int DispatchesNoEffects() {
  int result = 0;
  auto fs = Journal({{1, Bytes(Created(1, 1)) + Bytes(Cancelled(2, 1))}});
  const auto replayed = Replay(fs);
  result |= Check(replayed.status == ReplayStatus::kReplayed, "replays: " + replayed.detail);
  for (const auto& entry : fs.log) {
    const auto op = entry.substr(0, entry.find(' '));
    result |= Check(op == "list" || op == "read", "replay performs only reads: " + entry);
  }
  auto throwing = Journal({{1, Bytes(Created(1, 1))}});
  throwing.throw_on_read = true;
  const auto unreadable = Replay(throwing);
  result |= Check(unreadable.status == ReplayStatus::kUnreadable && throwing.OpenHandles() == 0,
                  "a throwing read is an unreadable refusal, not a crash: " + unreadable.detail);
  return result;
}

LogicalJobEvent Recorded(std::uint64_t sequence, int job, EventType type, EventPayload payload) {
  return LogicalJobEvent{1,
                         sequence,
                         type,
                         DiagnosticTimestamp{"2026-09-28T01:02:05Z"},
                         Uuid{Job(job)},
                         std::move(payload)};
}

int SequenceContinuation() {
  int result = 0;
  auto empty = Journal({});
  const auto none = Replay(empty);
  result |=
      Check(none.status == ReplayStatus::kReplayed && none.next_sequence == 1 && none.records == 0,
            "an empty Journal starts at 1: " + none.detail);
  auto two = Journal({{1, Bytes(Created(1, 1)) + Bytes(Created(2, 2))},
                      {3, Bytes(Created(3, 3)) + Bytes(Cancelled(4, 2))}});
  const auto replayed = Replay(two);
  result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 5 &&
                      replayed.records == 4 && replayed.jobs.size() == 3,
                  "next sequence continues across segments: " + replayed.detail);
  // Job 4 runs to the end (cancelled while admitted, finalized, terminal, cleaned up): it is
  // resolved and must not be reported.
  const std::vector<LogicalJobEvent> lifecycle{
      Created(1, 4),
      Cancelled(2, 4),
      Recorded(3, 4, EventType::kSessionRetainRequested, SessionPayload{Uuid{Job(4)}}),
      Recorded(4, 4, EventType::kSessionRetained, SessionPayload{Uuid{Job(4)}}),
      Recorded(5, 4, EventType::kFinalizationCompleted, EmptyPayload{}),
      Recorded(6, 4, EventType::kTerminalOutcomeCommitted,
               TerminalOutcomePayload{TerminalOutcome::kCancelled}),
      Recorded(7, 4, EventType::kCleanupStatusRecorded,
               CleanupStatusPayload{CleanupStatus::kCompleted}),
      Created(8, 5)};
  std::string closed_segment;
  for (const auto& event : lifecycle) closed_segment += Bytes(event);
  auto closed = Journal({{1, closed_segment}});
  const auto mixed = Replay(closed);
  result |=
      Check(mixed.status == ReplayStatus::kReplayed && mixed.jobs.size() == 2 &&
                !IsUnresolved(mixed.jobs[0]) && mixed.unresolved == std::vector<Uuid>{Uuid{Job(5)}},
            "a fully closed Job is not unresolved; only Job 5 is reported: " + mixed.detail);
  const std::vector<Uuid> expected_unresolved{Uuid{Job(1)}, Uuid{Job(2)}, Uuid{Job(3)}};
  result |= Check(replayed.unresolved == expected_unresolved,
                  "the admitted and stopping Jobs 1, 2, 3 are unresolved, in creation order");
  return result;
}

// Job `job` runs to the end from sequence `first`: cancelled while admitted, finalized, terminal,
// cleaned up. Seven records; the Job is resolved afterwards.
std::string ClosedJob(std::uint64_t first, int job) {
  const std::vector<LogicalJobEvent> lifecycle{
      Created(first, job),
      Cancelled(first + 1, job),
      Recorded(first + 2, job, EventType::kSessionRetainRequested, SessionPayload{Uuid{Job(job)}}),
      Recorded(first + 3, job, EventType::kSessionRetained, SessionPayload{Uuid{Job(job)}}),
      Recorded(first + 4, job, EventType::kFinalizationCompleted, EmptyPayload{}),
      Recorded(first + 5, job, EventType::kTerminalOutcomeCommitted,
               TerminalOutcomePayload{TerminalOutcome::kCancelled}),
      Recorded(first + 6, job, EventType::kCleanupStatusRecorded,
               CleanupStatusPayload{CleanupStatus::kCompleted})};
  std::string bytes;
  for (const auto& event : lifecycle) bytes += Bytes(event);
  return bytes;
}

// An orchestrator seeded from `replayed`, writing to `journal` (already opened on the replayed
// directory).
core::internal::Config SeededConfig(const ReplayResult& replayed, SegmentJournal& journal,
                                    FakePorts& fakes, std::size_t max_jobs) {
  core::internal::Config config;
  config.max_jobs = max_jobs;
  config.normal_capacity = 4;
  config.trace_capacity = config.total_capacity();
  config.completion_capacity = config.total_capacity();
  config.handoff_capacity = 4;
  config.ack_capacity = max_jobs < 4 ? 4 : max_jobs;
  config.callback_registration_capacity = 4;
  config.initial_journal_sequence = replayed.next_sequence;
  config.ports = {&fakes.clock, &journal, &fakes.runner, &fakes.session, &fakes.identity};
  config.replayed_jobs = replayed.jobs;
  config.unresolved_jobs = replayed.unresolved;
  return config;
}

bool WriteSide(const MemoryFileSystem& fs, std::size_t from) {
  for (std::size_t i = from; i < fs.log.size(); ++i) {
    const auto op = fs.log[i].substr(0, fs.log[i].find(' '));
    if (op != "list" && op != "read") return true;
  }
  return false;
}

int UnresolvedJobsBlockAdmission() {
  int result = 0;
  // Job 101 is closed; Job 102 was admitted and never resolved.
  auto fs = Journal({{1, ClosedJob(1, 101) + Bytes(Created(8, 102))}});
  const auto replayed = Replay(fs);
  result |= Check(replayed.status == ReplayStatus::kReplayed, "replays: " + replayed.detail);
  const std::vector<Uuid> expected_unresolved{Uuid{Job(102)}};
  result |= Check(replayed.unresolved == expected_unresolved, "Job 102 alone is unresolved");
  FakePorts fakes;
  SegmentJournal journal(fs);
  result |= Check(journal.Open(k_dir).ok, "journal opens");
  const auto files_before = fs.files;
  const auto log_before = fs.log.size();
  {
    core::internal::JobOrchestrator orchestrator(SeededConfig(replayed, journal, fakes, 4));
    result |= Check(!orchestrator.ready(), "readiness is false while Job 102 is unresolved");
    result |= Check(orchestrator.unresolved() == expected_unresolved,
                    "the unresolved Job is reported, and only it");
    for (std::size_t i = 0; i < replayed.jobs.size(); ++i) {
      const auto snapshot = orchestrator.SnapshotFor(replayed.jobs[i].job_id);
      result |= Check(snapshot && SnapshotJson(*snapshot) == SnapshotJson(replayed.jobs[i]),
                      "replayed snapshot " + std::to_string(i) + " is readable");
    }
    result |= Check(!orchestrator.SnapshotFor(Uuid{Job(103)}), "an unknown Job stays unknown");
    using core::internal::IngressCode;
    auto closed = [&result](const core::internal::IngressResult& admitted, const char* what) {
      result |=
          Check(admitted.code == IngressCode::kAdmissionClosed && admitted.ingress_sequence == 0,
                std::string(what) + " is refused with admission closed and no sequence");
    };
    closed(orchestrator.Create(), "Create");
    closed(orchestrator.SubmitCommand(Command{1, CommandType::kCancel, Uuid{Job(102)}, "op"}),
           "cancel");
    closed(orchestrator.SubmitCommand(Command{1, CommandType::kTerminate, Uuid{Job(102)}, "op"}),
           "terminate (critical)");
    closed(orchestrator.SubmitCommand(Command{1, CommandType::kTerminate, Uuid{Job(101)}, "op"}),
           "terminate of a closed Job");
    closed(orchestrator.SubmitCandidate(
               RawCandidateEvent{1, Uuid{Job(102)}, "finalization_completed", "{}"}),
           "candidate");
    closed(orchestrator.SubmitWorker(RawCandidateEvent{
               1, Uuid{Job(102)}, "worker_started",
               R"({"worker_id":"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f","event_sequence":1})"}),
           "worker event");
    const auto timer =
        orchestrator.SubmitTimeout(TimerNotification{Uuid{Job(102)}, TimeoutPhase::kExecution, 1});
    closed(timer.admitted, "timeout");
    result |= Check(!timer.discarded, "a timeout is refused, not silently discarded");
    closed(orchestrator.SubmitShutdown(), "shutdown marker");
    result |= Check(orchestrator.CopyIngressSequences().empty() &&
                        orchestrator.journal_attempts() == 0 && !orchestrator.failed(),
                    "nothing was sequenced or attempted, and the writer did not fail");
  }
  result |= Check(fs.files == files_before && !WriteSide(fs, log_before),
                  "nothing is appended, synced, or created while admission is closed");
  result |= Check(journal.NextSequence() == 9, "the Journal still continues at 9");
  return result;
}

int ResolvedJobsSeedWriter() {
  int result = 0;
  auto fs = Journal({{1, ClosedJob(1, 101) + ClosedJob(8, 102)}});
  const auto replayed = Replay(fs);
  result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.unresolved.empty() &&
                      replayed.next_sequence == 15,
                  "two closed Jobs replay with nothing unresolved: " + replayed.detail);
  FakePorts fakes;
  SegmentJournal journal(fs);
  result |= Check(journal.Open(k_dir).ok, "journal opens");
  {
    core::internal::JobOrchestrator orchestrator(SeededConfig(replayed, journal, fakes, 3));
    result |= Check(orchestrator.ready() && orchestrator.unresolved().empty(),
                    "a Journal of closed Jobs starts ready");
    for (const auto& job : replayed.jobs) {
      const auto snapshot = orchestrator.SnapshotFor(job.job_id);
      result |= Check(snapshot && SnapshotJson(*snapshot) == SnapshotJson(job),
                      "seeded Job " + job.job_id.value + " is readable");
    }
    // Terminating a seeded closed Job reaches the reducer and is rejected there.
    const auto terminate =
        orchestrator.SubmitCommand(Command{1, CommandType::kTerminate, Uuid{Job(101)}, "op"});
    result |= Check(terminate.code == core::internal::IngressCode::kAdmitted,
                    "terminate of a seeded Job is admitted");
    (void)orchestrator.WaitUntil(terminate.ingress_sequence,
                                 core::internal::WriterPhase::kTurnFinished);
    const auto rejected = orchestrator.TakeCompletion(terminate.ingress_sequence);
    result |=
        Check(rejected && rejected->code == core::internal::Completion::Code::kReducerRejection &&
                  !orchestrator.failed(),
              "terminate of a seeded closed Job is a reducer rejection");
    // The first new record continues at 15; the two seeded Jobs fill two of three slots.
    const auto first = orchestrator.Create();
    (void)orchestrator.WaitUntil(first.ingress_sequence,
                                 core::internal::WriterPhase::kTurnFinished);
    const auto created = orchestrator.TakeCompletion(first.ingress_sequence);
    result |= Check(created && created->code == core::internal::Completion::Code::kSuccess &&
                        journal.NextSequence() == 16,
                    "the first new record is sequence 15");
    const auto over = orchestrator.Create();
    result |= Check(over.code == core::internal::IngressCode::kResidentLimit,
                    "seeded Jobs count toward max_jobs");
  }
  const auto after = Replay(fs);
  result |= Check(after.status == ReplayStatus::kReplayed && after.jobs.size() == 3 &&
                      after.next_sequence == 16,
                  "the continued Journal replays: " + after.detail);
  // Invalid seeds are refused at construction.
  auto refused = [&](core::internal::Config config, const char* what) {
    bool threw = false;
    try {
      core::internal::JobOrchestrator orchestrator(std::move(config));
    } catch (const std::exception&) {
      threw = true;
    }
    result |= Check(threw, std::string(what) + " is refused at construction");
  };
  auto too_many = SeededConfig(replayed, journal, fakes, 1);
  refused(too_many, "more seeded Jobs than max_jobs");
  auto duplicate = SeededConfig(replayed, journal, fakes, 3);
  duplicate.replayed_jobs.push_back(replayed.jobs[0]);
  refused(duplicate, "a duplicate seeded Job");
  auto absent = SeededConfig(replayed, journal, fakes, 3);
  absent.replayed_jobs[0].entity_exists = false;
  refused(absent, "a seeded snapshot that does not exist");
  auto unknown = SeededConfig(replayed, journal, fakes, 3);
  unknown.unresolved_jobs.push_back(Uuid{Job(999)});
  refused(unknown, "an unresolved ID that was not seeded");
  return result;
}
}  // namespace
}  // namespace sitometron::test

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::cerr << "usage: sitometron_journal_replay_tests <job-reducer-vectors.json> <check>\n";
    return 2;
  }
  std::ifstream input(argv[1]);
  const auto ordered = nlohmann::ordered_json::parse(input);
  const auto vectors = nlohmann::json::parse(ordered.dump());
  using namespace sitometron::test;
  const std::string_view check = argv[2];
  if (check == "journal_replay_reproduces_vectors") return ReproducesVectors(vectors, ordered);
  if (check == "journal_startup_torn_tail_refusal") return TornTailRefusal();
  if (check == "journal_startup_corruption_refusal") return CorruptionRefusal();
  if (check == "journal_startup_segment_name_mismatch") return SegmentNameMismatch();
  if (check == "journal_startup_capacity_refusal") return CapacityRefusal();
  if (check == "journal_startup_sequence_exhausted_refusal") return SequenceExhaustedRefusal();
  if (check == "journal_startup_empty_active_segment") return EmptyActiveSegment();
  if (check == "journal_replay_dispatches_no_effects") return DispatchesNoEffects();
  if (check == "journal_replay_sequence_continuation") return SequenceContinuation();
  if (check == "journal_unresolved_jobs_block_admission") return UnresolvedJobsBlockAdmission();
  if (check == "journal_resolved_jobs_seed_writer") return ResolvedJobsSeedWriter();
  std::cerr << "unknown check " << check << '\n';
  return 2;
} catch (const std::exception& error) {
  std::cerr << "journal_replay: unexpected exception: " << error.what() << '\n';
  return 1;
} catch (...) {
  std::cerr << "journal_replay: unexpected non-standard exception\n";
  return 1;
}
