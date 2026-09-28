#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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

int ReproducesVectors(const Json& vectors, const Ordered& ordered) {
  int result = 0;
  std::size_t replayed = 0;
  const auto& cases = vectors.at("case_vectors");
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& vector = cases[index];
    const auto& expected = vector.at("expected");
    if (expected.at("journal_event").is_null()) continue;
    const auto id = vector.at("vector_id").get<std::string>();
    std::string error;
    const auto record = EventFromFixture(
        ordered.at("case_vectors")[index].at("expected").at("journal_event"), error);
    result |= Check(record.has_value(), id + " fixture decodes: " + error);
    if (!record) continue;
    const auto& initial = vector.at("initial_snapshot");
    const std::optional<Snapshot> before =
        initial.is_null() ? std::nullopt : std::optional<Snapshot>{SnapshotFrom(initial)};
    const auto after = ReplayRecord(before, *record, error);
    result |= Check(after.has_value(), id + " replays: " + error);
    ++replayed;
    // Like the reducer vector test, command vectors pin the decision only; the event matrix pins
    // the applied snapshot of the same Journal event.
    if (after && vector.at("matrix") == "event" && !expected.at("next_snapshot").is_null()) {
      result |= Check(SnapshotJson(*after) == expected.at("next_snapshot"),
                      id + " snapshot (actual=" + SnapshotJson(*after).dump() + ")");
    }
  }
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
      result |= Check(record.has_value(), id + " step decodes: " + error);
      if (!record) break;
      current = ReplayRecord(current, *record, error);
      result |= Check(current.has_value(), id + " step replays: " + error);
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
    config.ports = {&clock, &journal, &runner, &session, &identity};
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
  result |= Check(Refused(replayed, ReplayStatus::kTornTail, "journal_torn_tail") &&
                      replayed.detail.find(SegmentJournal::SegmentName(1)) != std::string::npos,
                  "torn tail of the last segment: " + replayed.detail);
  return result;
}

int CorruptionRefusal() {
  int result = 0;
  struct Case {
    std::string name;
    std::vector<std::pair<std::uint64_t, std::string>> segments;
  };
  const std::vector<Case> cases{
      {"undecodable line mid-Journal",
       {{1, Bytes(Created(1, 1)) + "{\"not\":\"a record\"}\n" + Bytes(Created(3, 3))}}},
      {"torn line in a sealed segment",
       {{1, Bytes(Created(1, 1)).substr(0, 30) + "\n"}, {2, Bytes(Created(2, 2))}}},
      {"sequence gap", {{1, Bytes(Created(1, 1)) + Bytes(Created(3, 3))}}},
      {"repeated sequence", {{1, Bytes(Created(1, 1)) + Bytes(Created(1, 2))}}},
      {"gap across segments", {{1, Bytes(Created(1, 1))}, {3, Bytes(Created(3, 3))}}},
      {"record for an absent Job", {{1, Bytes(Cancelled(1, 1))}}},
      {"duplicate job_created", {{1, Bytes(Created(1, 1)) + Bytes(Created(2, 1))}}},
      {"reducer rejects the record",
       {{1, Bytes(Created(1, 1)) +
                Bytes(LogicalJobEvent{1, 2, EventType::kTerminalOutcomeCommitted,
                                      DiagnosticTimestamp{"2026-09-28T01:02:05Z"}, Uuid{Job(1)},
                                      TerminalOutcomePayload{TerminalOutcome::kSucceeded}})}}},
      {"empty non-highest segment", {{1, ""}, {2, Bytes(Created(2, 2))}}},
  };
  for (const auto& c : cases) {
    auto fs = Journal(c.segments);
    const auto replayed = Replay(fs);
    result |= Check(Refused(replayed, ReplayStatus::kCorrupt, "journal_corrupt"),
                    c.name + ": " + replayed.detail);
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
  result |= Check(replayed.unresolved.size() == 3, "admitted and stopping Jobs are unresolved: " +
                                                       std::to_string(replayed.unresolved.size()));
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
  std::cerr << "unknown check " << check << '\n';
  return 2;
} catch (const std::exception& error) {
  std::cerr << "journal_replay: unexpected exception: " << error.what() << '\n';
  return 1;
} catch (...) {
  std::cerr << "journal_replay: unexpected non-standard exception\n";
  return 1;
}
