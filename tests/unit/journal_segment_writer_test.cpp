#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "job_orchestrator.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h must precede aclapi.h.
#include <aclapi.h>
#endif
#include "journal_test_events.hpp"
#include "memory_file_system.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/file_system.hpp"
#include "sitometron/journal/record_codec.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::test {
namespace {
using namespace sitometron::core;
using namespace sitometron::journal;
using core::internal::Completion;
using core::internal::IngressCode;
using core::internal::JobOrchestrator;
using core::internal::WriterPhase;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "journal_segment_writer: " << message << '\n';
  return 1;
}

const std::string k_dir = "/journal";

LogicalJobEvent Event(std::uint64_t sequence) {
  return LogicalJobEvent{1,
                         sequence,
                         EventType::kCancelAccepted,
                         DiagnosticTimestamp{"2026-09-27T01:02:03.456Z"},
                         Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
                         PrincipalPayload{"operator@example"}};
}

std::string Segment(std::uint64_t first) {
  return JoinPath(k_dir, SegmentJournal::SegmentName(first));
}

int OpenFresh(SegmentJournal& journal, std::uint64_t expected_next = 1) {
  const auto opened = journal.Open(k_dir);
  return Check(opened.ok && opened.next_sequence == expected_next,
               "open: " + opened.detail + " next=" + std::to_string(opened.next_sequence));
}

int SegmentCreationDurable() {
  int result = 0;
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted, "first commit");
    const auto& log = fs.log;
    const auto create = std::find(log.begin(), log.end(), "create " + Segment(1));
    const auto file_sync = std::find(log.begin(), log.end(), "sync " + Segment(1));
    const auto dir_sync = std::find(log.begin(), log.end(), "syncdir " + k_dir);
    const auto first_write = std::find_if(
        log.begin(), log.end(), [](const std::string& e) { return e.rfind("write", 0) == 0; });
    result |= Check(create != log.end() && file_sync != log.end() && dir_sync != log.end() &&
                        first_write != log.end() && create < file_sync && file_sync < dir_sync &&
                        dir_sync < first_write,
                    "segment file and directory entry synced before the first record");
    result |= Check(fs.durable_entries.count(Segment(1)) == 1, "segment entry durable");
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    fs.fail_next_directory_sync = true;
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure,
                    "failed directory sync is a definite failure");
    result |= Check(fs.CountOps("write") == 0 && fs.files[Segment(1)].empty(),
                    "no record written after a failed directory sync");
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    fs.fail_next_create = true;
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure,
                    "failed segment creation is a definite failure");
  }
  return result;
}

int DiskSyncOrder() {
  int result = 0;
  MemoryFileSystem fs;
  SegmentJournal journal(fs);
  result |= OpenFresh(journal);
  fs.short_write_limit = 7;
  fs.interrupted_writes = 2;
  result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted,
                  "commit through interrupted and partial writes");
  const auto expected = EncodeRecord(Event(1)).bytes;
  result |= Check(fs.files[Segment(1)] == expected, "complete record including LF was written");
  const auto last_write = std::find_if(fs.log.rbegin(), fs.log.rend(), [](const std::string& e) {
    return e.rfind("write", 0) == 0;
  });
  const auto last_sync = std::find(fs.log.rbegin(), fs.log.rend(), "sync " + Segment(1));
  result |=
      Check(last_write != fs.log.rend() && last_sync != fs.log.rend() && last_sync < last_write,
            "data sync follows the last byte of the record");
  const auto syncs = fs.CountOps("sync " + Segment(1));
  result |= Check(journal.Commit(Event(2)) == LogicalCommitResult::kCommitted, "second commit");
  result |= Check(fs.CountOps("sync " + Segment(1)) == syncs + 1, "one data sync per record");
  return result;
}

int CommitResultClassification() {
  int result = 0;
  struct Case {
    std::string name;
    LogicalCommitResult expected;
    void (*arrange)(MemoryFileSystem&);
    bool partial_bytes;
  };
  const std::vector<Case> cases{
      {"failure before first byte", LogicalCommitResult::kDefiniteFailure,
       [](MemoryFileSystem& fs) { fs.next_write_fault = {{0, IoError::kOther}}; }, false},
      {"no space before first byte", LogicalCommitResult::kDefiniteFailure,
       [](MemoryFileSystem& fs) { fs.next_write_fault = {{0, IoError::kNoSpace}}; }, false},
      {"error after partial write", LogicalCommitResult::kOutcomeUnknown,
       [](MemoryFileSystem& fs) { fs.next_write_fault = {{5, IoError::kOther}}; }, true},
      {"no space after partial write", LogicalCommitResult::kOutcomeUnknown,
       [](MemoryFileSystem& fs) { fs.next_write_fault = {{5, IoError::kNoSpace}}; }, true},
      {"data sync failure", LogicalCommitResult::kOutcomeUnknown,
       [](MemoryFileSystem& fs) { fs.fail_next_sync = true; }, true},
  };
  for (const auto& c : cases) {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted, c.name + " setup");
    const auto syncs_before = fs.CountOps("sync ");
    c.arrange(fs);
    const auto outcome = journal.Commit(Event(2));
    result |= Check(outcome == c.expected,
                    c.name + ": result " + std::to_string(static_cast<int>(outcome)));
    if (c.name == "data sync failure") {
      result |=
          Check(fs.CountOps("sync ") == syncs_before + 1, "a failed data sync is never retried");
    }
    const auto first = EncodeRecord(Event(1)).bytes;
    result |= Check((fs.files[Segment(1)].size() > first.size()) == c.partial_bytes,
                    c.name + ": partial bytes present iff outcome unknown");
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    auto bad = Event(1);
    bad.payload = WorkerRunningPayload{Uuid{"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"}};
    result |= Check(
        journal.Commit(bad) == LogicalCommitResult::kDefiniteFailure && fs.CountOps("write") == 0,
        "schema-invalid event is a definite failure with no I/O");
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    auto big = Event(1);
    big.recorded_at.rfc3339 = "2026-09-27T01:02:03." + std::string(max_record_bytes, '1') + "Z";
    result |= Check(
        journal.Commit(big) == LogicalCommitResult::kDefiniteFailure && fs.CountOps("write") == 0,
        "oversize event is a definite failure with no I/O");
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    result |= Check(journal.Commit(Event(2)) == LogicalCommitResult::kDefiniteFailure &&
                        fs.CountOps("write") == 0,
                    "a sequence gap is a definite failure with no I/O");
  }
  {
    // An exception before the first record byte is a definite failure; once writing began it is
    // outcome-unknown (ADR-0006 §4); the writing calls cannot throw.
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    fs.throw_on_create = true;
    result |= Check(
        journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure && journal.Poisoned(),
        "exception while creating the segment is a definite failure");
    // A second Commit() that arrives while the first one fails must observe the poison: the
    // failure is handled and latched before the lock is released.
    MemoryFileSystem racing;
    SegmentJournal shared(racing);
    result |= OpenFresh(shared);
    std::thread second;
    std::atomic<LogicalCommitResult> second_outcome{LogicalCommitResult::kCommitted};
    racing.throw_on_create = true;
    racing.on_throwing_create = [&] {
      second = std::thread([&] { second_outcome.store(shared.Commit(Event(1))); });
    };
    result |= Check(shared.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure,
                    "throwing commit is definite");
    if (second.joinable()) second.join();
    result |= Check(second_outcome.load() == LogicalCommitResult::kDefiniteFailure &&
                        racing.CountOps("write") == 0 && racing.files.count(Segment(1)) == 0,
                    "a concurrent commit after the failure sees the poison and performs no I/O");
    // Every call made after a record's first byte is noexcept, so an exception cannot leave the
    // number of accepted bytes unknown (ADR-0006 §4).
    static_assert(noexcept(std::declval<FileSystem&>().Write(FileHandle{}, std::string_view{})));
    static_assert(noexcept(std::declval<FileSystem&>().SyncData(FileHandle{})));
    static_assert(noexcept(std::declval<FileSystem&>().SyncDirectory(std::string{})));
    static_assert(noexcept(std::declval<FileSystem&>().Close(FileHandle{})));
  }
  {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |=
        Check(journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure && fs.log.empty(),
              "commit before Open is a definite failure with no I/O");
    result |= Check(journal.Poisoned(), "commit before Open poisons the instance");
    result |= OpenFresh(journal);
    const auto ops = fs.log.size();
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure &&
                        fs.log.size() == ops && journal.Poisoned(),
                    "a later Open does not clear the poison; commits stay definite with no I/O");
  }
  return result;
}

int AdapterPoisonedAfterFailure() {
  int result = 0;
  for (const bool unknown : {false, true}) {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    if (unknown) {
      fs.fail_next_sync = true;
    } else {
      fs.next_write_fault = {{0, IoError::kOther}};
    }
    result |= Check(journal.Commit(Event(1)) != LogicalCommitResult::kCommitted, "first fails");
    const auto ops = fs.log.size();
    result |= Check(journal.Poisoned(), "journal is poisoned");
    for (std::uint64_t sequence : {1U, 2U}) {
      result |= Check(journal.Commit(Event(sequence)) == LogicalCommitResult::kDefiniteFailure,
                      "poisoned commit is a definite failure");
    }
    result |= Check(fs.log.size() == ops, "poisoned commits perform no I/O");
  }
  {
    // Poison does not survive a restart: a new process reopens and continues.
    MemoryFileSystem fs;
    {
      SegmentJournal journal(fs);
      result |= OpenFresh(journal);
      result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted, "commit 1");
      fs.next_write_fault = {{0, IoError::kOther}};
      result |= Check(journal.Commit(Event(2)) == LogicalCommitResult::kDefiniteFailure, "fail 2");
    }
    SegmentJournal reopened(fs);
    result |= OpenFresh(reopened, 2);
    result |=
        Check(!reopened.Poisoned() && reopened.Commit(Event(2)) == LogicalCommitResult::kCommitted,
              "restart is not poisoned and reuses the never-written sequence");
  }
  return result;
}

int SegmentRotation() {
  int result = 0;
  MemoryFileSystem fs;
  const auto record = EncodeRecord(Event(1)).bytes.size();
  SegmentJournal journal(fs, SegmentJournalOptions{record * 2});
  result |= OpenFresh(journal);
  for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
    result |= Check(journal.Commit(Event(sequence)) == LogicalCommitResult::kCommitted,
                    "commit " + std::to_string(sequence));
  }
  result |= Check(fs.files.count(Segment(1)) == 1 && fs.files.count(Segment(3)) == 1 &&
                      fs.files.count(Segment(5)) == 1 && fs.files.size() == 3,
                  "segments start at 1, 3, 5");
  result |=
      Check(fs.files[Segment(1)].size() == record * 2 && fs.files[Segment(5)].size() == record,
            "records never split across segments; limit reached exactly");
  result |= Check(fs.OpenHandles() == 2, "old segment handles are closed (lock + active remain)");
  result |=
      Check(SegmentJournal::SegmentName(1) == "journal-00000000000000000001.ndjson" &&
                SegmentJournal::SegmentName(UINT64_MAX) == "journal-18446744073709551615.ndjson",
            "segment names are 20 zero-padded digits");
  {
    // A record larger than the limit still gets its own segment.
    MemoryFileSystem tiny_fs;
    SegmentJournal tiny(tiny_fs, SegmentJournalOptions{1});
    result |= OpenFresh(tiny);
    result |= Check(tiny.Commit(Event(1)) == LogicalCommitResult::kCommitted &&
                        tiny.Commit(Event(2)) == LogicalCommitResult::kCommitted &&
                        tiny_fs.files.count(Segment(2)) == 1,
                    "a record over the segment limit is written alone");
  }
  return result;
}

int RestartContinues() {
  int result = 0;
  MemoryFileSystem fs;
  {
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
      result |= Check(journal.Commit(Event(sequence)) == LogicalCommitResult::kCommitted, "commit");
    }
  }
  {
    SegmentJournal journal(fs);
    const auto opened = journal.Open(k_dir);
    result |= Check(opened.ok && opened.next_sequence == 4 &&
                        opened.active_segment == SegmentJournal::SegmentName(1),
                    "restart continues after the last record: " + opened.detail);
    result |= Check(journal.Commit(Event(4)) == LogicalCommitResult::kCommitted &&
                        fs.files[Segment(1)].size() == 4 * EncodeRecord(Event(1)).bytes.size(),
                    "restart appends to the active segment");
  }
  {
    // An empty highest segment named for the next sequence is the active segment.
    fs.files[Segment(5)] = "";
    SegmentJournal journal(fs);
    const auto opened = journal.Open(k_dir);
    result |= Check(opened.ok && opened.next_sequence == 5 &&
                        opened.active_segment == SegmentJournal::SegmentName(5),
                    "empty highest segment accepted: " + opened.detail);
    result |= Check(journal.Commit(Event(5)) == LogicalCommitResult::kCommitted &&
                        !fs.files[Segment(5)].empty(),
                    "next record goes to the empty highest segment");
  }
  {
    // The active segment's size survives a restart, so rotation still honors the limit.
    MemoryFileSystem limited;
    const auto record = EncodeRecord(Event(1)).bytes.size();
    {
      SegmentJournal journal(limited, SegmentJournalOptions{record * 2});
      result |= OpenFresh(journal);
      result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted, "limited 1");
    }
    SegmentJournal journal(limited, SegmentJournalOptions{record * 2});
    result |= OpenFresh(journal, 2);
    result |= Check(journal.Commit(Event(2)) == LogicalCommitResult::kCommitted &&
                        journal.Commit(Event(3)) == LogicalCommitResult::kCommitted &&
                        limited.files[Segment(1)].size() == record * 2 &&
                        limited.files.count(Segment(3)) == 1,
                    "restart keeps the active size and rotates at the limit");
  }
  {
    // An adopted empty segment is re-synced (file, then directory entry) before its first record,
    // because the crash that left it may have preceded its creation syncs (ADR-0006 §3).
    MemoryFileSystem crashed;
    crashed.directories.insert(k_dir);
    crashed.files[Segment(1)] = "";
    {
      SegmentJournal journal(crashed);
      result |= OpenFresh(journal, 1);
      crashed.log.clear();
      result |=
          Check(journal.Commit(Event(1)) == LogicalCommitResult::kCommitted, "adopted commit");
      const auto& log = crashed.log;
      const auto file_sync = std::find(log.begin(), log.end(), "sync " + Segment(1));
      const auto dir_sync = std::find(log.begin(), log.end(), "syncdir " + k_dir);
      const auto first_write = std::find_if(
          log.begin(), log.end(), [](const std::string& e) { return e.rfind("write", 0) == 0; });
      result |= Check(file_sync != log.end() && dir_sync != log.end() && first_write != log.end() &&
                          file_sync < dir_sync && dir_sync < first_write,
                      "adopted empty segment re-synced before its first record");
      const auto dir_syncs = crashed.CountOps("syncdir");
      result |= Check(journal.Commit(Event(2)) == LogicalCommitResult::kCommitted &&
                          crashed.CountOps("syncdir") == dir_syncs,
                      "the re-sync happens once");
    }
    MemoryFileSystem unsynced;
    unsynced.directories.insert(k_dir);
    unsynced.files[Segment(1)] = "";
    SegmentJournal journal(unsynced);
    result |= OpenFresh(journal, 1);
    unsynced.fail_next_directory_sync = true;
    result |= Check(journal.Commit(Event(1)) == LogicalCommitResult::kDefiniteFailure &&
                        unsynced.files[Segment(1)].empty() && journal.Poisoned(),
                    "failed re-sync of an adopted segment is definite and writes nothing");
  }
  {
    // A complete record whose data sync failed is outcome unknown to the process that wrote it. To
    // the next process it is committed (ADR-0006 §3): startup and replay count it, and the writer
    // continues after it.
    MemoryFileSystem fs;
    {
      SegmentJournal journal(fs);
      result |= OpenFresh(journal);
      result |= Check(journal.Commit(Created(1, 1)) == LogicalCommitResult::kCommitted, "commit 1");
      fs.fail_next_sync = true;
      result |= Check(journal.Commit(Cancelled(2, 1)) == LogicalCommitResult::kOutcomeUnknown &&
                          journal.Poisoned() &&
                          fs.files[Segment(1)] == Bytes(Created(1, 1)) + Bytes(Cancelled(2, 1)),
                      "a failed data sync leaves the complete record with an unknown outcome");
    }
    const auto replayed = ReplayJournal(fs, k_dir, ReplayOptions{1});
    result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.records == 2 &&
                        replayed.next_sequence == 3,
                    "replay counts the unknown-outcome record: " + replayed.detail);
    SegmentJournal reopened(fs);
    result |= OpenFresh(reopened, 3);
    const auto third =
        Recorded(3, 1, EventType::kSessionRetainRequested, core::SessionPayload{Uuid{Job(1)}});
    result |= Check(
        !reopened.Poisoned() && reopened.Commit(third) == LogicalCommitResult::kCommitted &&
            fs.files[Segment(1)] == Bytes(Created(1, 1)) + Bytes(Cancelled(2, 1)) + Bytes(third),
        "restart continues after the unknown-outcome record");
  }
  {
    // An exception while locating segments must not leak the directory lock (RAII in Open()).
    MemoryFileSystem throwing;
    throwing.directories.insert(k_dir);
    throwing.files[Segment(1)] = EncodeRecord(Event(1)).bytes;
    for (const bool list : {true, false}) {
      throwing.throw_on_list = list;
      throwing.throw_on_read = !list;
      SegmentJournal failing(throwing);
      const auto failed = failing.Open(k_dir);
      result |= Check(!failed.ok, std::string(list ? "list" : "read") + " exception fails Open");
      result |= Check(throwing.OpenHandles() == 0, "no handle leaks after a throwing Open");
    }
    throwing.throw_on_list = false;
    throwing.throw_on_read = false;
    SegmentJournal retry(throwing);
    result |= OpenFresh(retry, 2);
  }
  {
    // Reopening an existing Journal directory re-syncs its parent entry first and fails closed if
    // that sync fails, before any record could be committed below it.
    MemoryFileSystem existing;
    existing.directories.insert(k_dir);
    existing.fail_next_directory_sync = true;
    SegmentJournal failing(existing);
    result |= Check(!failing.Open(k_dir).ok && existing.OpenHandles() == 0,
                    "reopen fails closed when the parent entry cannot be re-synced");
    SegmentJournal reopened(existing);
    result |= OpenFresh(reopened, 1);
    const auto parent_sync = std::find(
        existing.log.begin(), existing.log.end(),
        "syncdir " + std::filesystem::path(k_dir).lexically_normal().parent_path().string());
    const auto lock = std::find_if(existing.log.begin(), existing.log.end(),
                                   [](const std::string& e) { return e.rfind("lock", 0) == 0; });
    result |=
        Check(parent_sync != existing.log.end() && lock != existing.log.end() && parent_sync < lock,
              "the parent entry is synced before the Journal is locked and used");
  }
  {
    // A segment-shaped name that is not a valid segment may hide records; the writer refuses it.
    MemoryFileSystem malformed;
    malformed.directories.insert(k_dir);
    malformed.files[JoinPath(k_dir, "journal-12.ndjson")] = EncodeRecord(Event(1)).bytes;
    SegmentJournal journal(malformed);
    result |= Check(!journal.Open(k_dir).ok && malformed.OpenHandles() == 0,
                    "a malformed segment name refuses Open without leaking the lock");
  }
  {
    MemoryFileSystem torn;
    torn.directories.insert(k_dir);
    torn.files[Segment(1)] = EncodeRecord(Event(1)).bytes + "{\"schema";
    SegmentJournal journal(torn);
    const auto refused = journal.Open(k_dir);
    result |= Check(!refused.ok, "torn tail of the active segment is refused");
    result |= Check(
        refused.detail.find("journal_torn_tail") != std::string::npos &&
            refused.detail.find("at byte " + std::to_string(EncodeRecord(Event(1)).bytes.size())) !=
                std::string::npos &&
            refused.detail.find(SegmentJournal::SegmentName(1)) != std::string::npos,
        "the refusal names journal_torn_tail, the byte offset, and the segment: " + refused.detail);
  }
  {
    MemoryFileSystem misnamed;
    misnamed.directories.insert(k_dir);
    misnamed.files[Segment(7)] = "";
    SegmentJournal journal(misnamed);
    result |= Check(!journal.Open(k_dir).ok,
                    "empty highest segment not named for the next sequence is refused");
  }
  {
    MemoryFileSystem twice;
    SegmentJournal journal(twice);
    result |= OpenFresh(journal);
    result |= Check(!journal.Open(k_dir).ok, "a second Open on the same journal is refused");
  }
  return result;
}

int NeverRewrites() {
  int result = 0;
  MemoryFileSystem fs;
  {
    SegmentJournal journal(fs, SegmentJournalOptions{400});
    result |= OpenFresh(journal);
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
      result |= Check(journal.Commit(Event(sequence)) == LogicalCommitResult::kCommitted, "commit");
    }
  }
  const auto before = fs.files;
  {
    SegmentJournal journal(fs, SegmentJournalOptions{400});
    const auto opened = journal.Open(k_dir);
    result |= Check(opened.ok, "reopen: " + opened.detail);
    fs.next_write_fault = {{5, IoError::kOther}};
    result |= Check(journal.Commit(Event(5)) == LogicalCommitResult::kOutcomeUnknown,
                    "partial write of record 5");
  }
  for (const auto& [path, content] : before) {
    const auto& now = fs.files[path];
    result |= Check(now.size() >= content.size() && now.compare(0, content.size(), content) == 0,
                    "existing bytes of " + path + " are unchanged");
  }
  for (const auto& entry : fs.log) {
    const auto op = entry.substr(0, entry.find(' '));
    result |=
        Check(op == "mkdir" || op == "lock" || op == "list" || op == "read" || op == "create" ||
                  op == "open" || op == "write" || op == "sync" || op == "syncdir" || op == "close",
              "only append-only operations are used: " + entry);
  }
  return result;
}

int DirectoryExclusiveLock() {
  int result = 0;
  const auto root = std::filesystem::temp_directory_path() /
                    ("sitometron-journal-lock-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  {
    auto& fs = SystemFileSystem();
    SegmentJournal first(fs);
    const auto opened = first.Open(root.string());
    result |= Check(opened.ok && opened.next_sequence == 1, "first open: " + opened.detail);
    SegmentJournal second(fs);
    result |= Check(!second.Open(root.string()).ok, "second open is refused while locked");
    result |=
        Check(first.Commit(Event(1)) == LogicalCommitResult::kCommitted, "real file system commit");
#if defined(_WIN32)
    for (const auto& target : {root, root / SegmentJournal::SegmentName(1),
                               root / std::filesystem::path("journal.lock")}) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      PACL dacl = nullptr;
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      const bool read =
          GetNamedSecurityInfoW(target.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &dacl, nullptr, &descriptor) == ERROR_SUCCESS;
      const bool is_protected =
          read && GetSecurityDescriptorControl(descriptor, &control, &revision) &&
          (control & SE_DACL_PROTECTED) != 0 && dacl != nullptr && dacl->AceCount == 1;
      if (descriptor != nullptr) LocalFree(descriptor);
      result |= Check(is_protected, "owner-only protected DACL on " + target.string());
    }
#endif
  }
  {
    // Missing ancestors are created (and synced) on the way to the Journal directory.
    SegmentJournal nested(SystemFileSystem());
    const auto deep = root / "nested" / "deeper" / "journal";
    const auto opened = nested.Open(deep.string());
    result |= Check(opened.ok && std::filesystem::is_directory(deep) &&
                        nested.Commit(Event(1)) == LogicalCommitResult::kCommitted,
                    "journal under missing ancestors opens and commits: " + opened.detail);
  }
  {
    // A trailing separator or "." must not demote the Journal directory to an ancestor created
    // with default permissions: it is still the owner-only final directory.
    for (const auto& spelling : {(root / "slash" / "journal").string() +
                                     std::string(1, std::filesystem::path::preferred_separator),
                                 (root / "dot" / "journal" / ".").string()}) {
      SegmentJournal spelled(SystemFileSystem());
      const auto opened = spelled.Open(spelling);
      result |= Check(opened.ok, "open " + spelling + ": " + opened.detail);
      auto journal_dir = std::filesystem::path(spelling).lexically_normal();
      if (journal_dir.filename().empty()) journal_dir = journal_dir.parent_path();
#if defined(_WIN32)
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      PACL dacl = nullptr;
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      const bool read =
          GetNamedSecurityInfoW(journal_dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &dacl, nullptr, &descriptor) == ERROR_SUCCESS;
      const bool is_protected =
          read && GetSecurityDescriptorControl(descriptor, &control, &revision) &&
          (control & SE_DACL_PROTECTED) != 0 && dacl != nullptr && dacl->AceCount == 1;
      if (descriptor != nullptr) LocalFree(descriptor);
      result |= Check(is_protected, "owner-only DACL for " + spelling);
#else
      const auto permissions = std::filesystem::status(journal_dir).permissions();
      result |= Check(permissions == std::filesystem::perms::owner_all,
                      "Journal directory is 0700 for " + spelling);
#endif
    }
  }
  {
    // The noexcept sync of a directory that cannot be opened reports an error instead of throwing.
    const auto missing = (root / "does-not-exist").string();
    result |= Check(SystemFileSystem().SyncDirectory(missing) != IoError::kNone,
                    "syncing a missing directory fails as a value");
  }
  {
    SegmentJournal third(SystemFileSystem());
    const auto opened = third.Open(root.string());
    result |= Check(opened.ok && opened.next_sequence == 2,
                    "lock is released on destruction and the sequence continues: " + opened.detail);
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return result;
}

// Minimal ports for driving the private orchestrator with a real SegmentJournal.
class FixedClock final : public ClockPort {
 public:
  ClockReading Read() override {
    return ClockReading{DiagnosticTimestamp{"2026-09-27T01:02:03.456Z"}, MonotonicInstant{++tick_}};
  }

 private:
  std::uint64_t tick_ = 0;
};
class QuietRunner final : public ApplicationRunnerPort {
 public:
  void HandoffLaunch(ApplicationLaunchRequest&&) noexcept override {}
  void HandoffCooperativeStop(ApplicationStopRequest&&) noexcept override {}
  void HandoffForcedStop(ApplicationStopRequest&&) noexcept override {}
};
class QuietSession final : public SessionRetainerPort {
 public:
  void HandoffRetainSameIdentity(SessionRetainRequest&&) noexcept override {}
};
class FixedIdentity final : public IdentitySourcePort {
 public:
  JobSessionIdentityResult GenerateJobSessionIdentity() override {
    return GeneratedJobSessionIdentity{
        Uuid{std::string("01890f3e-7b00-7abc-8abc-00000000000") + std::to_string(++jobs_ % 10)}};
  }
  WorkerIdentityResult GenerateWorkerIdentity() override {
    return GeneratedWorkerIdentity{Uuid{"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"}};
  }
  LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override {
    return GeneratedLaunchOperationIdentity{StableId{"launch-1"}};
  }

 private:
  int jobs_ = 0;
};

int PhysicalCommitFailureFailClosed() {
  int result = 0;
  for (const bool unknown : {false, true}) {
    MemoryFileSystem fs;
    SegmentJournal journal(fs);
    result |= OpenFresh(journal);
    FixedClock clock;
    QuietRunner runner;
    QuietSession session;
    FixedIdentity identity;
    core::internal::Config config;
    config.max_jobs = 2;
    config.normal_capacity = 4;
    config.trace_capacity = config.total_capacity();
    config.completion_capacity = config.total_capacity();
    config.handoff_capacity = 4;
    config.ack_capacity = 4;
    config.callback_registration_capacity = 4;
    config.initial_journal_sequence = 1;
    config.ports.clock = &clock;
    config.ports.journal = &journal;
    config.ports.runner = &runner;
    config.ports.session = &session;
    config.ports.identity = &identity;
    JobOrchestrator orchestrator(config);
    const auto first = orchestrator.Create();
    result |= Check(first.code == IngressCode::kAdmitted, "first create admitted");
    result |= Check(orchestrator.WaitUntil(first.ingress_sequence, WriterPhase::kTurnFinished),
                    "first turn finishes");
    const auto ok = orchestrator.TakeCompletion(first.ingress_sequence);
    result |= Check(ok && ok->code == Completion::Code::kSuccess, "first job_created committed");
    if (unknown) {
      fs.fail_next_sync = true;
    } else {
      fs.next_write_fault = {{0, IoError::kNoSpace}};
    }
    const auto files_before = fs.files;
    const auto first_job = orchestrator.LastCreated();
    const auto first_snapshot = first_job ? orchestrator.SnapshotFor(*first_job) : std::nullopt;
    const auto trace_before = orchestrator.CopyTrace();
    const auto second = orchestrator.Create();
    result |= Check(second.code == IngressCode::kAdmitted, "second create admitted");
    (void)orchestrator.WaitUntil(second.ingress_sequence, WriterPhase::kTurnFinished);
    const auto failed = orchestrator.TakeCompletion(second.ingress_sequence);
    result |= Check(failed && failed->code == Completion::Code::kServiceFailed,
                    "physical commit failure yields service_failed");
    result |= Check(journal.Poisoned(), "journal poisoned after the physical failure");
    const auto third = orchestrator.Create();
    result |= Check(third.code == IngressCode::kServiceFailed,
                    "the writer stays failed closed after the physical failure");
    const auto segment = fs.files[Segment(1)];
    const auto committed = files_before.at(Segment(1));
    result |= Check(segment.compare(0, committed.size(), committed) == 0 &&
                        (unknown ? segment.size() > committed.size() : segment == committed),
                    "only the failed record's bytes (if any) follow the committed record");
    // Nothing from the failed turn was applied: no new resident, the first Job's snapshot is
    // unchanged, and the trace gained the failed attempt but no commit or activation.
    // LastCreated() reports the identity generated for the latest Create(), committed or not.
    const auto second_job = orchestrator.LastCreated();
    result |= Check(second_job.has_value() && second_job != first_job,
                    "the failed Create() generated a distinct identity");
    result |= Check(second_job.has_value() && !orchestrator.SnapshotFor(*second_job).has_value(),
                    "failed creation leaves no snapshot for the new Job");
    const auto first_after = first_job ? orchestrator.SnapshotFor(*first_job) : std::nullopt;
    result |= Check(first_snapshot.has_value() && first_after.has_value() &&
                        first_after->state == first_snapshot->state &&
                        first_after->entity_exists == first_snapshot->entity_exists,
                    "the committed Job's snapshot is unchanged by the failure");
    const auto trace_after = orchestrator.CopyTrace();
    bool applied_after_failure = false;
    for (std::size_t i = trace_before.size(); i < trace_after.size(); ++i) {
      applied_after_failure = applied_after_failure ||
                              trace_after[i].kind == core::internal::TraceKind::kJournalCommitted ||
                              trace_after[i].kind == core::internal::TraceKind::kSnapshotActivated;
    }
    result |= Check(trace_after.size() > trace_before.size() && !applied_after_failure,
                    "the failed turn records an attempt but no commit or snapshot activation");
    (void)orchestrator.BeginShutdown();
  }
  return result;
}
}  // namespace
}  // namespace sitometron::test

int main(int argc, char** argv) try {
  if (argc != 2) {
    std::cerr << "usage: sitometron_journal_segment_tests <check>\n";
    return 2;
  }
  using namespace sitometron::test;
  const std::string_view check = argv[1];
  if (check == "journal_segment_creation_durable") return SegmentCreationDurable();
  if (check == "job_physical_disk_sync_order") return DiskSyncOrder();
  if (check == "journal_commit_result_classification") return CommitResultClassification();
  if (check == "journal_adapter_poisoned_after_failure") return AdapterPoisonedAfterFailure();
  if (check == "job_physical_commit_failure_fail_closed") return PhysicalCommitFailureFailClosed();
  if (check == "journal_directory_exclusive_lock") return DirectoryExclusiveLock();
  if (check == "journal_daemon_never_rewrites") return NeverRewrites();
  if (check == "journal_segment_rotation") return SegmentRotation();
  if (check == "journal_segment_restart_continues") return RestartContinues();
  std::cerr << "unknown check " << check << '\n';
  return 2;
} catch (const std::exception& error) {
  std::cerr << "journal_segment_writer: unexpected exception: " << error.what() << '\n';
  return 1;
} catch (...) {
  std::cerr << "journal_segment_writer: unexpected non-standard exception\n";
  return 1;
}
