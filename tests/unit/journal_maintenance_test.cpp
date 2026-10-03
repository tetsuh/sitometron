#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "journal_test_events.hpp"
#include "memory_file_system.hpp"
#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/maintenance.hpp"
#include "sitometron/journal/replay.hpp"
#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::test {
namespace {
using namespace sitometron::core;
using namespace sitometron::journal;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "journal_maintenance: " << message << '\n';
  return 1;
}

const std::string k_dir = "/journal";
const std::string k_archive = JoinPath(k_dir, k_archive_directory);

std::string Name(std::uint64_t first) { return SegmentJournal::SegmentName(first); }
std::string SegmentPath(std::uint64_t first) { return JoinPath(k_dir, Name(first)); }
std::string ArchivePath(std::uint64_t first) { return JoinPath(k_archive, Name(first)); }

MemoryFileSystem Journal(const std::vector<std::pair<std::uint64_t, std::string>>& segments) {
  MemoryFileSystem fs;
  fs.directories.insert(k_dir);
  for (const auto& [first, content] : segments) fs.files[SegmentPath(first)] = content;
  return fs;
}

ReplayResult Replay(MaintenanceFileSystem& fs, std::size_t max_jobs) {
  return ReplayJournal(fs, k_dir, ReplayOptions{max_jobs});
}
ReplayResult Replay(MaintenanceFileSystem& fs) {
  return Replay(fs, std::numeric_limits<std::size_t>::max());
}

PruneResult Prune(MemoryFileSystem& fs, bool dry_run = false) {
  PruneOptions options;
  options.dry_run = dry_run;
  return PruneClosedPrefix(fs, k_dir, options);
}

std::vector<Uuid> Jobs(std::initializer_list<int> numbers) {
  std::vector<Uuid> ids;
  ids.reserve(numbers.size());
  for (const int n : numbers) ids.push_back(Uuid{Job(n)});
  return ids;
}

std::vector<Uuid> Ids(const std::vector<Snapshot>& snapshots) {
  std::vector<Uuid> ids;
  ids.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) ids.push_back(snapshot.job_id);
  return ids;
}

// True when an operation after log position `from` changed the file system.
// A directory sync, an entry check, or a link count changes no byte, so none counts.
bool Changed(const MemoryFileSystem& fs, std::size_t from) {
  for (std::size_t i = from; i < fs.log.size(); ++i) {
    const auto op = fs.log[i].substr(0, fs.log[i].find(' '));
    if (op != "list" && op != "read" && op != "lock" && op != "close" && op != "syncdir" &&
        op != "entry" && op != "links") {
      return true;
    }
  }
  return false;
}

// Position of the first log entry equal to `entry` at or after `from`, or npos.
std::size_t Find(const MemoryFileSystem& fs, std::string_view entry, std::size_t from = 0) {
  for (std::size_t i = from; i < fs.log.size(); ++i) {
    if (fs.log[i] == entry) return i;
  }
  return std::string::npos;
}

// Every file handle, including the Journal lock, is released and the lock can be taken again.
bool Released(MemoryFileSystem& fs) {
  if (fs.OpenHandles() != 0) return false;
  IoError error = IoError::kNone;
  const auto lock = fs.Lock(JoinPath(k_dir, "journal.lock"), error);
  if (!lock.has_value()) return false;
  fs.Close(*lock);
  return true;
}

// Three segments: closed Job 1, closed Job 2, and Job 3 still admitted in the highest segment.
MemoryFileSystem ClosedPrefixJournal() {
  return Journal({{1, ClosedJob(1, 1)}, {8, ClosedJob(8, 2)}, {15, Bytes(Created(15, 3))}});
}

int PrunePrefixOnly() {
  int result = 0;
  auto fs = ClosedPrefixJournal();
  const auto before_files = fs.files;
  const auto before = Replay(fs);

  // A dry run reports the prefix and changes nothing.
  auto mark = fs.log.size();
  const auto planned = Prune(fs, true);
  result |= Check(planned.status == MaintenanceStatus::kPlanned, "dry run: " + planned.detail);
  result |= Check(planned.segments == std::vector<std::string>{Name(1), Name(8)} &&
                      planned.records == 14 && planned.jobs == Jobs({1, 2}) &&
                      planned.first_retained_sequence == 15 && planned.next_sequence == 16 &&
                      planned.detail.find(" in " + k_dir) != std::string::npos,
                  "dry run reports segments 1 and 8, 14 records, Jobs 1 and 2, retained from 15");
  result |= Check(!Changed(fs, mark) && fs.files == before_files, "a dry run changes nothing");
  result |= Check(Released(fs), "the dry run releases the lock");

  // Apply moves exactly the same prefix, lowest first, into archive/.
  mark = fs.log.size();
  const auto pruned = Prune(fs);
  result |= Check(pruned.status == MaintenanceStatus::kDone &&
                      pruned.detail.find(" in " + k_dir) != std::string::npos,
                  "prune: " + pruned.detail);
  result |= Check(pruned.segments == planned.segments && pruned.records == planned.records &&
                      pruned.jobs == planned.jobs &&
                      pruned.first_retained_sequence == planned.first_retained_sequence &&
                      pruned.next_sequence == planned.next_sequence,
                  "the applied prefix equals the dry run's");
  result |= Check(fs.files.count(SegmentPath(1)) == 0 && fs.files.count(SegmentPath(8)) == 0 &&
                      fs.files.at(ArchivePath(1)) == before_files.at(SegmentPath(1)) &&
                      fs.files.at(ArchivePath(8)) == before_files.at(SegmentPath(8)),
                  "segments 1 and 8 moved byte-for-byte into archive/");
  result |= Check(fs.files.at(SegmentPath(15)) == before_files.at(SegmentPath(15)),
                  "the retained segment is unchanged");
  const auto first_move = Find(fs, "rename " + SegmentPath(1) + " " + ArchivePath(1), mark);
  const auto second_move = Find(fs, "rename " + SegmentPath(8) + " " + ArchivePath(8), mark);
  const auto ensured = Find(fs, "mkdir " + k_archive, mark);
  result |= Check(ensured < first_move && first_move < second_move,
                  "archive/ is ensured first, then segments move lowest first");
  result |= Check(Find(fs, "syncdir " + k_dir, ensured) < first_move,
                  "the Journal directory is synced after archive/ is ensured and before any move");
  result |= Check(Find(fs, "syncdir " + k_archive, first_move) < second_move &&
                      Find(fs, "syncdir " + k_dir, first_move) < second_move &&
                      Find(fs, "syncdir " + k_archive, second_move) != std::string::npos &&
                      Find(fs, "syncdir " + k_dir, second_move) != std::string::npos,
                  "both directories are synced after every move");
  result |= Check(fs.durable_entries.count(ArchivePath(1)) != 0 &&
                      fs.durable_entries.count(ArchivePath(8)) != 0,
                  "each moved name is durable in archive/");
  result |= Check(Released(fs), "prune releases the lock");

  // Replay afterwards starts at the first retained segment with the same next sequence and
  // replays only the retained Job; archive/ is never read as segments.
  const auto after = Replay(fs);
  result |= Check(after.status == ReplayStatus::kReplayed &&
                      after.next_sequence == before.next_sequence && after.records == 1 &&
                      Ids(after.jobs) == Jobs({3}) && after.segments.size() == 1 &&
                      after.segments[0].first == 15,
                  "replay after pruning starts at 15, keeps next sequence 16, and has Job 3 only");

  // Running again finds nothing: the highest non-empty segment always stays.
  mark = fs.log.size();
  const auto again = Prune(fs);
  result |= Check(again.status == MaintenanceStatus::kNothingToDo && again.segments.empty() &&
                      !Changed(fs, mark),
                  "a second prune has nothing to do: " + again.detail);

  // A Journal over the daemon's resident capacity is still prunable, and then starts.
  auto crowded = Journal({{1, ClosedJob(1, 1)},
                          {8, ClosedJob(8, 2)},
                          {15, Bytes(Created(15, 3)) + Bytes(Created(16, 4))}});
  result |= Check(Replay(crowded, 2).status == ReplayStatus::kCapacityExceeded,
                  "four Jobs exceed a capacity of two");
  const auto relieved = Prune(crowded);
  result |= Check(relieved.status == MaintenanceStatus::kDone && relieved.jobs == Jobs({1, 2}),
                  "capacity overflow does not block pruning: " + relieved.detail);
  result |= Check(Replay(crowded, 2).status == ReplayStatus::kReplayed,
                  "after pruning the Journal fits a capacity of two");
  return result;
}

int PruneRefusesOpenJobs() {
  int result = 0;
  // Job 1 is unresolved in the first segment: nothing is prunable.
  auto first =
      Journal({{1, Bytes(Created(1, 1))}, {2, ClosedJob(2, 2)}, {9, Bytes(Created(9, 3))}});
  auto mark = first.log.size();
  const auto none = Prune(first);
  result |= Check(none.status == MaintenanceStatus::kNothingToDo && none.segments.empty() &&
                      none.jobs.empty() && !Changed(first, mark) && first.CountOps("mkdir") == 0,
                  "an unresolved Job in the first segment blocks every prefix: " + none.detail);
  result |= Check(none.detail.find("journal_nothing_to_prune") != std::string::npos &&
                      none.detail.find(" in " + k_dir) != std::string::npos,
                  "the result names journal_nothing_to_prune and the Journal: " + none.detail);

  // Job 2 is unresolved in the middle: the prefix stops before it, even though Job 3 is closed.
  auto middle = Journal({{1, ClosedJob(1, 1)},
                         {8, Bytes(Created(8, 2))},
                         {9, ClosedJob(9, 3)},
                         {16, Bytes(Created(16, 4))}});
  const auto stopped = Prune(middle);
  result |=
      Check(stopped.status == MaintenanceStatus::kDone &&
                stopped.segments == std::vector<std::string>{Name(1)} && stopped.jobs == Jobs({1}),
            "the prefix stops before the unresolved Job 2: " + stopped.detail);
  result |=
      Check(middle.files.count(SegmentPath(8)) != 0 && middle.files.count(SegmentPath(9)) != 0,
            "the segment of Job 2 and every later one stay");
  const auto retained = Replay(middle);
  result |= Check(retained.status == ReplayStatus::kReplayed &&
                      Ids(retained.jobs) == Jobs({2, 3, 4}) && retained.unresolved == Jobs({2, 4}),
                  "retained Jobs are listed and the pruned Job 1 is absent");

  // Job 1 closes in the second segment: its first segment alone is not a prefix, both are.
  auto spanning = Journal({{1, ClosedJobPart(1, 1, 0, 4)},
                           {5, ClosedJobPart(1, 1, 4, 7) + ClosedJob(8, 2)},
                           {15, Bytes(Created(15, 3))}});
  const auto whole = Prune(spanning);
  result |= Check(whole.status == MaintenanceStatus::kDone &&
                      whole.segments == std::vector<std::string>{Name(1), Name(5)} &&
                      whole.jobs == Jobs({1, 2}) && whole.records == 14,
                  "a Job spanning two segments is pruned only with both: " + whole.detail);

  // Job 1 closes in the highest non-empty segment: its earlier records stay with it.
  auto late = Journal({{1, ClosedJobPart(1, 1, 0, 4)}, {5, ClosedJobPart(1, 1, 4, 7)}});
  mark = late.log.size();
  const auto kept = Prune(late);
  result |=
      Check(kept.status == MaintenanceStatus::kNothingToDo && !Changed(late, mark),
            "a Job with a record in a retained segment keeps its earlier ones: " + kept.detail);
  return result;
}

int PruneKeepsLastRecord() {
  int result = 0;
  // Every Job is closed, but the highest non-empty segment stays.
  auto closed = Journal({{1, ClosedJob(1, 1)}, {8, ClosedJob(8, 2)}});
  const auto pruned = Prune(closed);
  result |= Check(pruned.status == MaintenanceStatus::kDone &&
                      pruned.segments == std::vector<std::string>{Name(1)} &&
                      pruned.first_retained_sequence == 8 && pruned.next_sequence == 15,
                  "only segment 1 moves when every Job is closed: " + pruned.detail);
  const auto after = Replay(closed);
  result |= Check(after.status == ReplayStatus::kReplayed && after.next_sequence == 15 &&
                      Ids(after.jobs) == Jobs({2}) && after.unresolved.empty(),
                  "the last committed record stays and the next sequence is 15");

  // An empty active segment and the non-empty segment before it both stay.
  auto active = Journal({{1, ClosedJob(1, 1)}, {8, ClosedJob(8, 2)}, {15, ""}});
  const auto kept = Prune(active);
  result |= Check(kept.status == MaintenanceStatus::kDone &&
                      kept.segments == std::vector<std::string>{Name(1)} &&
                      active.files.count(SegmentPath(8)) != 0 &&
                      active.files.at(SegmentPath(15)).empty() && kept.next_sequence == 15,
                  "the empty active segment and segment 8 stay: " + kept.detail);
  const auto resumed = Replay(active);
  result |= Check(resumed.status == ReplayStatus::kReplayed && resumed.next_sequence == 15,
                  "the empty active segment still names the next sequence: " + resumed.detail);

  // Journals without a sealed segment before the last record have nothing to prune.
  for (auto [label, fs] : std::vector<std::pair<std::string, MemoryFileSystem>>{
           {"an empty Journal", Journal({})},
           {"a fresh empty segment", Journal({{1, ""}})},
           {"a single segment", Journal({{1, ClosedJob(1, 1)}})}}) {
    const auto mark = fs.log.size();
    const auto nothing = Prune(fs);
    result |= Check(nothing.status == MaintenanceStatus::kNothingToDo && !Changed(fs, mark),
                    label + " has nothing to prune: " + nothing.detail);
  }

  // A refused Journal is reported and nothing moves; a torn tail must be quarantined first.
  auto torn = Journal({{1, ClosedJob(1, 1)}, {8, ClosedJob(8, 2) + "{\"partial"}});
  auto mark = torn.log.size();
  const auto refused = Prune(torn);
  result |=
      Check(refused.status == MaintenanceStatus::kRefused &&
                refused.detail.find("journal_torn_tail") != std::string::npos &&
                refused.detail.find(" in " + k_dir) != std::string::npos && !Changed(torn, mark),
            "a torn tail refuses pruning unchanged: " + refused.detail);
  auto corrupt = Journal({{1, ClosedJob(1, 1)}, {8, "not json\n"}});
  mark = corrupt.log.size();
  const auto bad = Prune(corrupt);
  result |=
      Check(bad.status == MaintenanceStatus::kRefused &&
                bad.detail.find("journal_corrupt") != std::string::npos &&
                bad.detail.find(" in " + k_dir) != std::string::npos && !Changed(corrupt, mark),
            "corruption refuses pruning unchanged: " + bad.detail);
  return result;
}

const std::string k_tail = "{\"partial";

int QuarantineTail() {
  int result = 0;
  const auto complete = ClosedJob(1, 1);
  const auto quarantine_name = Name(1) + ".torn-" + std::to_string(complete.size());
  const auto quarantine_path = JoinPath(k_dir, quarantine_name);
  {
    auto fs = Journal({{1, complete + k_tail}});
    const auto mark = fs.log.size();
    const auto moved = QuarantineTornTail(fs, k_dir);
    result |= Check(moved.status == MaintenanceStatus::kDone && moved.segment == Name(1) &&
                        moved.offset == complete.size() && moved.quarantine == quarantine_name &&
                        moved.bytes == k_tail.size() && moved.next_sequence == 8 &&
                        !moved.sequence_exhausted &&
                        moved.detail.find(" in " + k_dir) != std::string::npos,
                    "the torn tail is quarantined and the cut reported: " + moved.detail);
    result |=
        Check(fs.files.at(quarantine_path) == k_tail && fs.files.at(SegmentPath(1)) == complete,
              "the tail bytes moved and the segment ends at its last LF");
    const auto create = Find(fs, "create " + quarantine_path, mark);
    const auto sync = Find(fs, "sync " + quarantine_path, create);
    const auto directory = Find(fs, "syncdir " + k_dir, sync);
    const auto cut = Find(fs, "truncate " + SegmentPath(1) + " " + std::to_string(complete.size()));
    result |=
        Check(create < sync && sync < directory && directory < cut && cut != std::string::npos,
              "quarantine file created, data-synced, entry-synced, then the segment cut");
    result |= Check(fs.durable_entries.count(quarantine_path) != 0,
                    "the quarantine file's entry is durable");
    result |= Check(Replay(fs).status == ReplayStatus::kReplayed, "the Journal replays afterwards");
    result |= Check(Released(fs), "the lock and every handle are released");
    // A second run finds the Journal clean. The segment still ends at the quarantine offset, so the
    // cut is synced again (a failed sync would look the same); no byte changes.
    const auto settled = fs.files;
    const auto again = QuarantineTornTail(fs, k_dir);
    result |= Check(again.status == MaintenanceStatus::kNothingToDo &&
                        again.detail.find("journal_clean") != std::string::npos &&
                        again.detail.find("synced the cut of " + Name(1) + " at byte " +
                                          std::to_string(complete.size())) != std::string::npos &&
                        again.detail.find("re-synced") == std::string::npos && fs.files == settled,
                    "a second run finds the Journal clean and unchanged: " + again.detail);
  }
  {
    // The torn tail is in the highest of two segments; the sealed one is untouched.
    auto fs = Journal({{1, complete}, {8, ClosedJobPart(8, 2, 0, 2) + k_tail}});
    const auto sealed = fs.files.at(SegmentPath(1));
    const auto moved = QuarantineTornTail(fs, k_dir);
    result |= Check(moved.status == MaintenanceStatus::kDone && moved.segment == Name(8) &&
                        fs.files.at(SegmentPath(8)) == ClosedJobPart(8, 2, 0, 2) &&
                        fs.files.at(SegmentPath(1)) == sealed && moved.next_sequence == 10,
                    "only the highest segment is cut: " + moved.detail);
  }
  {
    // A torn tail at byte 0 leaves an empty highest segment named for the next sequence.
    auto fs = Journal({{1, complete}, {8, k_tail}});
    const auto moved = QuarantineTornTail(fs, k_dir);
    const auto replayed = Replay(fs);
    result |= Check(moved.status == MaintenanceStatus::kDone && moved.offset == 0 &&
                        fs.files.at(SegmentPath(8)).empty() &&
                        replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 8,
                    "a torn tail at byte 0 leaves a valid empty active segment: " + moved.detail);
    auto fresh = Journal({{1, k_tail}});
    const auto fresh_moved = QuarantineTornTail(fresh, k_dir);
    result |= Check(fresh_moved.status == MaintenanceStatus::kDone &&
                        fresh_moved.next_sequence == 1 && !fresh_moved.sequence_exhausted &&
                        Replay(fresh).status == ReplayStatus::kReplayed,
                    "a fresh Journal with only torn bytes becomes empty: " + fresh_moved.detail);
  }
  {
    // A clean Journal, corruption, and sequence exhaustion are reported and left unchanged.
    auto clean = Journal({{1, complete}});
    auto corrupt = Journal({{1, complete}, {8, "not json\n"}});
    auto exhausted = Journal({{UINT64_MAX, Bytes(Created(UINT64_MAX, 1))}});
    for (auto* fs : {&clean, &corrupt, &exhausted}) {
      const auto mark = fs->log.size();
      const auto files = fs->files;
      const auto unchanged = QuarantineTornTail(*fs, k_dir);
      result |= Check(!Changed(*fs, mark) && fs->files == files && Released(*fs),
                      "no torn tail, no change: " + unchanged.detail);
    }
    result |= Check(QuarantineTornTail(clean, k_dir).status == MaintenanceStatus::kNothingToDo,
                    "a clean Journal has nothing to do");
    const auto corrupt_result = QuarantineTornTail(corrupt, k_dir);
    result |= Check(corrupt_result.status == MaintenanceStatus::kRefused &&
                        corrupt_result.detail.find("journal_corrupt") != std::string::npos &&
                        corrupt_result.detail.find(" in " + k_dir) != std::string::npos,
                    "corruption is refused: " + corrupt_result.detail);
    const auto exhausted_result = QuarantineTornTail(exhausted, k_dir);
    result |=
        Check(exhausted_result.status == MaintenanceStatus::kRefused &&
                  exhausted_result.detail.find("journal_sequence_exhausted") != std::string::npos,
              "sequence exhaustion is refused: " + exhausted_result.detail);
  }
  {
    // A tail after the last possible sequence is quarantined; the Journal is then exhausted.
    const auto last = Bytes(Created(UINT64_MAX, 1));
    auto fs = Journal({{UINT64_MAX, last + k_tail}});
    const auto moved = QuarantineTornTail(fs, k_dir);
    result |= Check(moved.status == MaintenanceStatus::kDone &&
                        fs.files.at(JoinPath(k_dir, Name(UINT64_MAX))) == last &&
                        Replay(fs).status == ReplayStatus::kSequenceExhausted,
                    "a tail after UINT64_MAX is quarantined: " + moved.detail);
    result |= Check(
        moved.sequence_exhausted && moved.next_sequence == 0 &&
            moved.detail.find("sequence exhausted") != std::string::npos,
        "the result reports the exhausted sequence, not a next sequence of 0: " + moved.detail);
  }
  {
    // An existing quarantine file: identical is accepted, a prefix is completed, and anything
    // else is a conflict that leaves the segment untouched.
    auto identical = Journal({{1, complete + k_tail}});
    identical.files[quarantine_path] = k_tail;
    const auto accepted = QuarantineTornTail(identical, k_dir);
    result |= Check(accepted.status == MaintenanceStatus::kDone &&
                        identical.files.at(quarantine_path) == k_tail &&
                        identical.files.at(SegmentPath(1)) == complete &&
                        identical.CountOps("create") == 0 &&
                        Find(identical, "sync " + quarantine_path) != std::string::npos,
                    "an identical quarantine file is re-synced and accepted: " + accepted.detail);
    auto partial = Journal({{1, complete + k_tail}});
    partial.files[quarantine_path] = k_tail.substr(0, 3);
    const auto completed = QuarantineTornTail(partial, k_dir);
    result |= Check(completed.status == MaintenanceStatus::kDone &&
                        partial.files.at(quarantine_path) == k_tail &&
                        partial.files.at(SegmentPath(1)) == complete,
                    "a partial quarantine file is completed: " + completed.detail);
    // An existing quarantine file with another hard link (here: the segment itself, whose whole
    // content is the torn tail) would lose the torn bytes when the segment is cut: refused.
    auto aliased = Journal({{1, k_tail}});
    const auto alias_path = JoinPath(k_dir, Name(1) + ".torn-0");
    aliased.files[alias_path] = k_tail;
    aliased.hard_links[alias_path] = 2;
    aliased.hard_links[SegmentPath(1)] = 2;
    const auto alias_files = aliased.files;
    const auto refused_alias = QuarantineTornTail(aliased, k_dir);
    result |=
        Check(refused_alias.status == MaintenanceStatus::kRefused &&
                  refused_alias.detail.find("has other hard links") != std::string::npos &&
                  aliased.files == alias_files && aliased.CountOps("truncate") == 0,
              "a hard-linked quarantine file refuses before the cut: " + refused_alias.detail);
    // An existing quarantine name that is a symbolic link is never written through.
    auto linked = Journal({{1, complete + k_tail}});
    linked.files[quarantine_path] = k_tail.substr(0, 3);
    linked.symbolic_links.insert(quarantine_path);
    const auto linked_files = linked.files;
    const auto refused_link = QuarantineTornTail(linked, k_dir);
    result |= Check(refused_link.status == MaintenanceStatus::kRefused &&
                        refused_link.detail.find("is not a regular file") != std::string::npos &&
                        linked.files == linked_files && linked.CountOps("open") == 0,
                    "a linked quarantine name refuses without writing: " + refused_link.detail);
    auto different = Journal({{1, complete + k_tail}});
    different.files[quarantine_path] = "other";
    const auto mark = different.log.size();
    const auto conflict = QuarantineTornTail(different, k_dir);
    result |= Check(conflict.status == MaintenanceStatus::kRefused &&
                        conflict.detail.find("journal_quarantine_conflict") != std::string::npos &&
                        conflict.detail.find(quarantine_path) != std::string::npos &&
                        different.files.at(SegmentPath(1)) == complete + k_tail &&
                        different.files.at(quarantine_path) == "other" && !Changed(different, mark),
                    "a different quarantine file is a conflict: " + conflict.detail);
  }
  return result;
}

int RequiresLock() {
  int result = 0;
  auto fs = Journal({{1, ClosedJob(1, 1)}, {8, ClosedJob(8, 2) + k_tail}});
  IoError error = IoError::kNone;
  const auto daemon = fs.Lock(JoinPath(k_dir, "journal.lock"), error);
  result |= Check(daemon.has_value(), "the test holds the lock like a running daemon");
  const auto files = fs.files;
  const auto mark = fs.log.size();
  const auto quarantine = QuarantineTornTail(fs, k_dir);
  const auto prune = Prune(fs);
  const auto dry = Prune(fs, true);
  for (const auto& [label, status, detail] :
       std::vector<std::tuple<std::string, MaintenanceStatus, std::string>>{
           {"quarantine", quarantine.status, quarantine.detail},
           {"prune", prune.status, prune.detail},
           {"dry run", dry.status, dry.detail}}) {
    result |= Check(
        status == MaintenanceStatus::kRefused && detail.find("journal_locked") != std::string::npos,
        std::string(label).append(" refuses with journal_locked: ").append(detail));
  }
  bool only_lock_attempts = true;
  for (std::size_t i = mark; i < fs.log.size(); ++i) {
    only_lock_attempts = only_lock_attempts && fs.log[i].rfind("lock ", 0) == 0;
  }
  result |= Check(only_lock_attempts && fs.files == files,
                  "nothing but the lock attempt runs while the Journal is locked");
  if (daemon.has_value()) fs.Close(*daemon);
  result |= Check(QuarantineTornTail(fs, k_dir).status == MaintenanceStatus::kDone && Released(fs),
                  "after the daemon releases the lock, quarantine runs and releases it again");

  // A directory that cannot be listed is refused.
  MemoryFileSystem missing;
  const auto unreadable = QuarantineTornTail(missing, "/missing");
  const auto unlisted = PruneClosedPrefix(missing, "/missing", PruneOptions{});
  result |= Check(unreadable.status == MaintenanceStatus::kRefused &&
                      unreadable.detail.find("journal_unreadable") != std::string::npos &&
                      unreadable.detail.find("/missing") != std::string::npos &&
                      unlisted.status == MaintenanceStatus::kRefused &&
                      unlisted.detail.find("journal_unreadable") != std::string::npos &&
                      unlisted.detail.find("/missing") != std::string::npos && Released(missing),
                  "an unreadable directory is refused with its location: " + unreadable.detail +
                      " / " + unlisted.detail);
  return result;
}

// Runs `run` once with a fault armed on a fresh copy of `journal`, then again without faults.
struct FaultCase {
  std::string label;
  std::string step;  // word the failure detail must name
  void (*arm)(MemoryFileSystem&);
};

int QuarantineFaults() {
  int result = 0;
  const auto complete = ClosedJob(1, 1);
  const std::vector<FaultCase> cases{
      {"create", "create", [](MemoryFileSystem& fs) { fs.fail_next_create = true; }},
      {"short write", "write",
       [](MemoryFileSystem& fs) { fs.next_write_fault = MemoryFileSystem::WriteFault{3}; }},
      {"data sync", "sync", [](MemoryFileSystem& fs) { fs.fail_next_sync = true; }},
      {"directory sync", "sync", [](MemoryFileSystem& fs) { fs.fail_next_directory_sync = true; }},
      {"truncate", "truncate", [](MemoryFileSystem& fs) { fs.fail_next_truncate = true; }},
  };
  for (const auto& fault : cases) {
    auto fs = Journal({{1, complete + k_tail}});
    fault.arm(fs);
    const auto failed = QuarantineTornTail(fs, k_dir);
    result |= Check(failed.status == MaintenanceStatus::kFailed &&
                        failed.detail.find("journal_maintenance_failed") != std::string::npos &&
                        failed.detail.find(fault.step) != std::string::npos,
                    fault.label + ": the failure names its step: " + failed.detail);
    result |= Check(fs.files.at(SegmentPath(1)).rfind(complete, 0) == 0 && Released(fs),
                    fault.label + ": no complete record is touched and the lock is released");
    const auto retried = QuarantineTornTail(fs, k_dir);
    result |= Check(
        retried.status == MaintenanceStatus::kDone && fs.files.at(SegmentPath(1)) == complete &&
            fs.files.at(JoinPath(k_dir, Name(1) + ".torn-" + std::to_string(complete.size()))) ==
                k_tail,
        fault.label + ": a second run finishes: " + retried.detail);
  }
  {
    // The cut succeeds but its data sync fails: replay then sees no torn tail, so the rerun cuts
    // again at the quarantine file's offset before it reports the Journal clean.
    auto cut = Journal({{1, complete + k_tail}});
    cut.fail_after_next_truncate = true;
    const auto failed = QuarantineTornTail(cut, k_dir);
    result |= Check(failed.status == MaintenanceStatus::kFailed &&
                        failed.detail.find("truncate") != std::string::npos &&
                        cut.files.at(SegmentPath(1)) == complete && Released(cut),
                    "a failed sync after the cut is a failure: " + failed.detail);
    const auto cut_mark = cut.log.size();
    const auto resynced = QuarantineTornTail(cut, k_dir);
    const auto again =
        Find(cut, "truncate " + SegmentPath(1) + " " + std::to_string(complete.size()), cut_mark);
    result |= Check(resynced.status == MaintenanceStatus::kNothingToDo &&
                        resynced.detail.find("journal_clean") != std::string::npos &&
                        resynced.detail.find("synced the cut of " + Name(1)) != std::string::npos &&
                        again != std::string::npos &&
                        Find(cut, "syncdir " + k_dir, again) != std::string::npos,
                    "the rerun cuts and syncs again before reporting clean: " + resynced.detail);
    // The same after a tail behind UINT64_MAX: the cut is finished, and the refusal stays.
    const auto last = Bytes(Created(UINT64_MAX, 1));
    auto exhausted = Journal({{UINT64_MAX, last + k_tail}});
    exhausted.fail_after_next_truncate = true;
    (void)QuarantineTornTail(exhausted, k_dir);
    const auto exhausted_mark = exhausted.log.size();
    const auto refused = QuarantineTornTail(exhausted, k_dir);
    result |= Check(refused.status == MaintenanceStatus::kRefused &&
                        refused.detail.find("journal_sequence_exhausted") != std::string::npos &&
                        refused.detail.find("synced the cut of") != std::string::npos &&
                        Find(exhausted,
                             "truncate " + JoinPath(k_dir, Name(UINT64_MAX)) + " " +
                                 std::to_string(last.size()),
                             exhausted_mark) != std::string::npos,
                    "an exhausted Journal's earlier cut is synced again: " + refused.detail);
  }
  {
    // A cut that reports success but removes a complete record is never reported as done, although
    // replay of the shorter segment is valid.
    const auto six = ClosedJobPart(1, 1, 0, 6);
    auto early = Journal({{1, complete + k_tail}});
    early.next_truncate_to = six.size();
    const auto lost = QuarantineTornTail(early, k_dir);
    result |= Check(lost.status == MaintenanceStatus::kFailed &&
                        lost.detail.find("verify cut") != std::string::npos && Released(early),
                    "a cut before the last complete record is a failure: " + lost.detail);
    // The same check guards the re-sync of an earlier cut.
    auto earlier = Journal({{1, complete + k_tail}});
    earlier.fail_after_next_truncate = true;
    (void)QuarantineTornTail(earlier, k_dir);
    earlier.next_truncate_to = six.size();
    const auto resync_lost = QuarantineTornTail(earlier, k_dir);
    result |= Check(resync_lost.status == MaintenanceStatus::kFailed &&
                        resync_lost.detail.find("verify cut") != std::string::npos,
                    "a re-sync that removes a complete record is a failure: " + resync_lost.detail);
  }
  {
    // A cut that reports success without cutting is caught by the verification replay.
    auto fs = Journal({{1, complete + k_tail}});
    fs.ignore_next_truncate = true;
    const auto unverified = QuarantineTornTail(fs, k_dir);
    result |= Check(unverified.status == MaintenanceStatus::kFailed &&
                        unverified.detail.find("verify") != std::string::npos && Released(fs),
                    "an unverified cut is a failure: " + unverified.detail);
  }
  {
    // A read that throws is reported, and the lock is still released.
    auto fs = Journal({{1, complete + k_tail}});
    fs.throw_on_read = true;
    const auto thrown = QuarantineTornTail(fs, k_dir);
    // Replay catches the exception itself and reports the Journal unreadable.
    result |=
        Check(thrown.status == MaintenanceStatus::kRefused &&
                  thrown.detail.find("journal_unreadable") != std::string::npos && Released(fs),
              "an exception is reported and releases the lock: " + thrown.detail);
    // A create that throws reaches the operation's own boundary: kFailed with the Journal named.
    auto creating = Journal({{1, complete + k_tail}});
    creating.throw_on_create = true;
    const auto failed = QuarantineTornTail(creating, k_dir);
    result |=
        Check(failed.status == MaintenanceStatus::kFailed &&
                  failed.detail.find("journal_maintenance_failed") != std::string::npos &&
                  failed.detail.find(" in " + k_dir) != std::string::npos && Released(creating),
              "a throwing create is a failure that names the Journal: " + failed.detail);
  }
  return result;
}

int PruneFaults() {
  int result = 0;
  const std::vector<FaultCase> cases{
      {"archive directory", "archive", [](MemoryFileSystem& fs) { fs.fail_next_mkdir = true; }},
      {"rename", "rename", [](MemoryFileSystem& fs) { fs.fail_next_rename = true; }},
      {"directory sync", "sync", [](MemoryFileSystem& fs) { fs.fail_next_directory_sync = true; }},
  };
  for (const auto& fault : cases) {
    auto fs = ClosedPrefixJournal();
    fault.arm(fs);
    const auto failed = Prune(fs);
    result |= Check(
        failed.status == MaintenanceStatus::kFailed &&
            failed.detail.find("journal_maintenance_failed") != std::string::npos &&
            failed.detail.find(fault.step) != std::string::npos && failed.segments.empty() &&
            failed.jobs.empty(),
        fault.label + ": the failure names its step and reports nothing moved: " + failed.detail);
    const auto between = Replay(fs);
    result |= Check(
        between.status == ReplayStatus::kReplayed && between.next_sequence == 16 && Released(fs),
        fault.label + ": the Journal replays after the failure: " + between.detail);
    const auto retried = Prune(fs);
    result |=
        Check(retried.status == MaintenanceStatus::kDone && fs.files.count(ArchivePath(1)) != 0 &&
                  fs.files.count(ArchivePath(8)) != 0 && fs.files.count(SegmentPath(15)) != 0,
              fault.label + ": a second run finishes the prefix: " + retried.detail);
  }
  {
    // A crash between the two moves leaves a shorter pruned Journal; the rerun moves the rest. The
    // second directory sync is the first one after a move (the first makes archive/ durable).
    auto fs = ClosedPrefixJournal();
    fs.directory_syncs_until_failure = 2;
    (void)Prune(fs);
    const bool first_moved = fs.files.count(ArchivePath(1)) != 0;
    const bool second_stayed = fs.files.count(SegmentPath(8)) != 0;
    const auto partial = Replay(fs);
    result |= Check(first_moved && second_stayed && partial.status == ReplayStatus::kReplayed &&
                        partial.segments.size() == 2 && partial.segments[0].first == 8,
                    "after the first move the Journal replays from segment 8");
    const auto rest = Prune(fs);
    result |= Check(rest.status == MaintenanceStatus::kDone &&
                        rest.segments == std::vector<std::string>{Name(8)},
                    "the rerun moves only segment 8: " + rest.detail);
  }
  {
    // The last move's directory sync fails: the Journal already looks pruned, so the rerun has
    // nothing to move, but it syncs both directories before it reports the prefix settled.
    for (const std::size_t failing_sync : {4U, 5U}) {
      auto fs = ClosedPrefixJournal();
      fs.directory_syncs_until_failure = failing_sync;
      const auto failed = Prune(fs);
      const auto mark = fs.log.size();
      const auto settled = Prune(fs);
      result |=
          Check(failed.status == MaintenanceStatus::kFailed &&
                    failed.moved == std::vector<std::string>{Name(1), Name(8)} &&
                    settled.status == MaintenanceStatus::kNothingToDo &&
                    Find(fs, "syncdir " + k_archive, mark) != std::string::npos &&
                    Find(fs, "syncdir " + k_dir, mark) != std::string::npos,
                "a failed sync after the last move is synced again before nothing to prune: " +
                    settled.detail);
    }
    // The kind of archive/ cannot be read: that is a failure, not "no earlier move".
    auto unchecked = ClosedPrefixJournal();
    unchecked.directory_syncs_until_failure = 5;
    (void)Prune(unchecked);
    unchecked.unreadable_entries.insert(k_archive);
    const auto blind = Prune(unchecked);
    result |= Check(blind.status == MaintenanceStatus::kFailed &&
                        blind.detail.find("check " + k_archive) != std::string::npos,
                    "an archive/ whose kind cannot be read is a failure: " + blind.detail);
    // And a failure of that sync is reported.
    auto fs = ClosedPrefixJournal();
    fs.directory_syncs_until_failure = 5;
    (void)Prune(fs);
    fs.fail_next_directory_sync = true;
    const auto unsynced = Prune(fs);
    result |= Check(unsynced.status == MaintenanceStatus::kFailed &&
                        unsynced.detail.find("sync directory") != std::string::npos,
                    "a failed sync of earlier moves is a failure: " + unsynced.detail);
  }
  {
    // A move that reports success without moving is caught by the verification replay.
    auto fs = ClosedPrefixJournal();
    fs.ignore_next_rename = true;
    const auto unverified = Prune(fs);
    result |= Check(unverified.status == MaintenanceStatus::kFailed &&
                        unverified.detail.find("verify") != std::string::npos && Released(fs),
                    "an unverified move is a failure: " + unverified.detail);
  }
  {
    // A rename that throws reaches the operation's own boundary: kFailed with the Journal named.
    auto fs = ClosedPrefixJournal();
    fs.throw_on_rename = true;
    const auto thrown = Prune(fs);
    result |= Check(thrown.status == MaintenanceStatus::kFailed &&
                        thrown.detail.find("journal_maintenance_failed") != std::string::npos &&
                        thrown.detail.find(" in " + k_dir) != std::string::npos &&
                        thrown.segments.empty() && Released(fs),
                    "a throwing rename is a failure that names the Journal: " + thrown.detail);
  }
  {
    // archive/ left by an interrupted run: its entry is synced again, and a failure moves nothing.
    auto fs = ClosedPrefixJournal();
    fs.directories.insert(k_archive);
    fs.fail_next_directory_sync = true;
    const auto unsynced = Prune(fs);
    result |= Check(unsynced.status == MaintenanceStatus::kFailed &&
                        unsynced.detail.find("sync directory " + k_dir) != std::string::npos &&
                        fs.CountOps("rename") == 0 && Released(fs),
                    "an unsynced archive/ entry refuses before any move: " + unsynced.detail);
    const auto mark = fs.log.size();
    const auto retried = Prune(fs);
    const auto synced = Find(fs, "syncdir " + k_dir, mark);
    result |=
        Check(retried.status == MaintenanceStatus::kDone && synced != std::string::npos &&
                  synced < Find(fs, "rename " + SegmentPath(1) + " " + ArchivePath(1), mark),
              "the rerun syncs the Journal directory before its first move: " + retried.detail);
  }
  {
    // An archive/ that is a symbolic link is refused before anything moves.
    auto fs = ClosedPrefixJournal();
    fs.directories.insert(k_archive);
    fs.symbolic_links.insert(k_archive);
    const auto files = fs.files;
    const auto linked = Prune(fs);
    result |= Check(linked.status == MaintenanceStatus::kRefused &&
                        linked.detail.find("is not a directory") != std::string::npos &&
                        fs.CountOps("rename") == 0 && fs.CountOps("mkdir") == 0 &&
                        fs.files == files && Released(fs),
                    "a linked archive/ refuses before any move: " + linked.detail);
  }
  {
    // A non-regular entry at a destination (a directory or a dangling link) is a conflict too,
    // found before the first move.
    auto fs = ClosedPrefixJournal();
    fs.other_entries.insert(ArchivePath(8));
    const auto files = fs.files;
    const auto conflict = Prune(fs);
    result |= Check(conflict.status == MaintenanceStatus::kRefused &&
                        conflict.detail.find("journal_archive_conflict") != std::string::npos &&
                        fs.CountOps("rename") == 0 && fs.files == files && Released(fs),
                    "a non-regular archived entry refuses before any move: " + conflict.detail);
  }
  {
    // An archived name that already exists is a conflict, and nothing moves.
    auto fs = ClosedPrefixJournal();
    fs.files[ArchivePath(8)] = "older copy";
    const auto files = fs.files;
    const auto conflict = Prune(fs);
    result |= Check(conflict.status == MaintenanceStatus::kRefused &&
                        conflict.detail.find("journal_archive_conflict") != std::string::npos &&
                        conflict.segments.empty() && conflict.jobs.empty() &&
                        conflict.records == 0 && conflict.first_retained_sequence == 0 &&
                        fs.CountOps("rename") == 0 && fs.files == files && Released(fs),
                    "an existing archived name refuses before any move: " + conflict.detail);
  }
  return result;
}

// Job 1 spans segments 1 and 5, Job 2 closes in segment 5, Job 3 stays open in segment 15: the
// prunable prefix is segments 1 and 5, and no boundary inside it is prunable on its own.
MemoryFileSystem SpanningPrefixJournal() {
  return Journal({{1, ClosedJobPart(1, 1, 0, 4)},
                  {5, ClosedJobPart(1, 1, 4, 7) + ClosedJob(8, 2)},
                  {15, Bytes(Created(15, 3))}});
}

// A prune interrupted between moves of a prefix that a Job spans leaves a Journal the daemon
// refuses; running prune again completes the interrupted prune from archive/.
int PruneInterruptedSpanning() {
  int result = 0;
  const std::vector<FaultCase> cases{
      {"archive sync after the first move", "sync directory",
       [](MemoryFileSystem& fs) { fs.directory_syncs_until_failure = 2; }},
      {"Journal sync after the first move", "sync directory",
       [](MemoryFileSystem& fs) { fs.directory_syncs_until_failure = 3; }},
      {"second rename", "rename", [](MemoryFileSystem& fs) { fs.renames_until_failure = 2; }},
  };
  for (const auto& fault : cases) {
    auto fs = SpanningPrefixJournal();
    const auto original = fs.files;
    fault.arm(fs);
    const auto failed = Prune(fs);
    result |=
        Check(failed.status == MaintenanceStatus::kFailed &&
                  failed.detail.find(fault.step) != std::string::npos &&
                  failed.moved == std::vector<std::string>{Name(1)} &&
                  fs.files.count(ArchivePath(1)) != 0 && Released(fs),
              fault.label + ": the failure reports the one segment it moved: " + failed.detail);
    result |= Check(Replay(fs).status == ReplayStatus::kCorrupt,
                    fault.label + ": the daemon refuses the interrupted Journal");
    const auto planned = Prune(fs, true);
    result |=
        Check(planned.status == MaintenanceStatus::kPlanned &&
                  planned.segments == std::vector<std::string>{Name(5)} &&
                  planned.resumed == std::vector<std::string>{Name(1)} &&
                  planned.jobs == Jobs({1, 2}) && planned.first_retained_sequence == 15,
              fault.label + ": a dry run plans to finish the interrupted prune: " + planned.detail);
    const auto finished = Prune(fs);
    const auto after = Replay(fs);
    result |= Check(finished.status == MaintenanceStatus::kDone &&
                        finished.moved == std::vector<std::string>{Name(5)} &&
                        finished.resumed == std::vector<std::string>{Name(1)} &&
                        finished.detail.find("interrupted") != std::string::npos &&
                        fs.files.at(ArchivePath(1)) == original.at(SegmentPath(1)) &&
                        fs.files.at(ArchivePath(5)) == original.at(SegmentPath(5)) &&
                        after.status == ReplayStatus::kReplayed && after.next_sequence == 16 &&
                        Ids(after.jobs) == Jobs({3}) && Released(fs),
                    fault.label + ": running prune again finishes it: " + finished.detail);
  }
  {
    // Only the archived segments the interrupted prune needs are read: an older archived prefix
    // that the operator already deleted does not matter.
    auto fs = Journal({{1, ClosedJob(1, 1)},
                       {8, ClosedJobPart(8, 2, 0, 4)},
                       {12, ClosedJobPart(8, 2, 4, 7) + ClosedJob(15, 3)},
                       {22, Bytes(Created(22, 4))}});
    fs.renames_until_failure = 3;  // moves segments 1 and 8, then fails on 12
    (void)Prune(fs);
    // With segment 1 still archived, only segment 8 is the interrupted run: the shortest archived
    // run that explains the refusal is resumed, not every older archived segment.
    auto kept = fs;
    const auto planned = Prune(kept, true);
    result |= Check(planned.status == MaintenanceStatus::kPlanned &&
                        planned.resumed == std::vector<std::string>{Name(8)},
                    "only the interrupted run is resumed: " + planned.detail);
    fs.files.erase(ArchivePath(1));
    const auto finished = Prune(fs);
    result |= Check(finished.status == MaintenanceStatus::kDone &&
                        finished.resumed == std::vector<std::string>{Name(8)} &&
                        finished.moved == std::vector<std::string>{Name(12)} &&
                        Replay(fs).status == ReplayStatus::kReplayed,
                    "resume uses only the archived run before the Journal: " + finished.detail);
  }
  {
    // The resuming run is itself interrupted: Job 1 spans segments 1, 3, and 5. The first run moves
    // segment 1, the resume moves segment 3 and fails, and a third run resumes from both.
    auto fs = Journal({{1, ClosedJobPart(1, 1, 0, 2)},
                       {3, ClosedJobPart(1, 1, 2, 4)},
                       {5, ClosedJobPart(1, 1, 4, 7) + ClosedJob(8, 2)},
                       {15, Bytes(Created(15, 3))}});
    fs.renames_until_failure = 2;
    const auto first = Prune(fs);
    fs.renames_until_failure = 2;
    const auto second = Prune(fs);
    const auto third = Prune(fs);
    result |=
        Check(first.status == MaintenanceStatus::kFailed &&
                  first.moved == std::vector<std::string>{Name(1)} &&
                  second.status == MaintenanceStatus::kFailed &&
                  second.moved == std::vector<std::string>{Name(3)} &&
                  third.status == MaintenanceStatus::kDone &&
                  third.resumed == std::vector<std::string>{Name(1), Name(3)} &&
                  third.moved == std::vector<std::string>{Name(5)} &&
                  Replay(fs).status == ReplayStatus::kReplayed && Ids(Replay(fs).jobs) == Jobs({3}),
              "an interrupted resume is resumed again: " + third.detail);
  }
  {
    // An archived run that explains the refusal but holds an unresolved Job is not a prunable
    // prefix: it is refused, not reported as nothing to prune, and nothing moves.
    auto fs = Journal({{2, Bytes(Cancelled(2, 1))}, {3, Bytes(Created(3, 2))}});
    fs.directories.insert(k_archive);
    fs.files[ArchivePath(1)] = Bytes(Created(1, 1));
    const auto mark = fs.log.size();
    const auto refused = Prune(fs);
    result |= Check(refused.status == MaintenanceStatus::kRefused &&
                        refused.detail.find("not a prunable prefix") != std::string::npos &&
                        refused.resumed.empty() && !Changed(fs, mark),
                    "an archived run with an unresolved Job is refused: " + refused.detail);
  }
  {
    // A Journal that refuses for another reason is not resumed: nothing moves.
    auto fs =
        Journal({{5, ClosedJobPart(1, 1, 4, 7) + ClosedJob(8, 2)}, {15, Bytes(Created(15, 3))}});
    const auto mark = fs.log.size();
    const auto refused = Prune(fs);
    result |=
        Check(refused.status == MaintenanceStatus::kRefused &&
                  refused.detail.find("journal_corrupt") != std::string::npos &&
                  refused.moved.empty() && fs.CountOps("rename") == 0 && !Changed(fs, mark),
              "a Journal without a matching archived run is refused unchanged: " + refused.detail);
  }
  return result;
}

int MaintenanceFaults() { return QuarantineFaults() | PruneFaults() | PruneInterruptedSpanning(); }

// Real file system: cut, no-replace rename, and both operations end to end.
int SystemFileSystemCheck() {
  int result = 0;
  const auto root = std::filesystem::temp_directory_path() /
                    ("sitometron-journal-maintenance-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto& fs = SystemMaintenanceFileSystem();
  const auto write = [](const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
  };
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  };
  {
    result |= Check(fs.EnsureDirectory(root.string()) == IoError::kNone, "create the directory");
    const auto a = root / "a.bin";
    const auto b = root / "b.bin";
    const auto c = root / "c.bin";
    write(a, "abcdef");
    result |= Check(fs.Truncate(a.string(), 3) == IoError::kNone && read(a) == "abc",
                    "truncate cuts the file back to three bytes");
    result |= Check(fs.RenameNoReplace(a.string(), b.string()) == IoError::kNone &&
                        !std::filesystem::exists(a) && read(b) == "abc",
                    "rename moves the file");
    write(c, "other");
    IoError checked = IoError::kNone;
    result |= Check(fs.Entry((root / "no-such-directory").string(), checked) == EntryKind::kNone,
                    "a missing directory is EntryKind::kNone");
    result |= Check(fs.RenameNoReplace(c.string(), b.string()) == IoError::kExists &&
                        read(b) == "abc" && read(c) == "other",
                    "rename refuses to replace an existing file and changes nothing");
    std::filesystem::remove(b);
    std::filesystem::remove(c);
  }
  {
    // Both operations end to end on segments written by the adapter's naming rule.
    write(root / Name(1), ClosedJob(1, 1));
    write(root / Name(8), ClosedJob(8, 2));
    write(root / Name(15), Bytes(Created(15, 3)) + k_tail);
    {
      // A running daemon holds journal.lock; maintenance is refused and changes nothing.
      IoError error = IoError::kNone;
      const auto daemon = SystemFileSystem().Lock((root / "journal.lock").string(), error);
      const auto locked = QuarantineTornTail(fs, root.string());
      result |= Check(daemon.has_value() && locked.status == MaintenanceStatus::kRefused &&
                          locked.detail.find("journal_locked") != std::string::npos &&
                          read(root / Name(15)) == Bytes(Created(15, 3)) + k_tail,
                      "a held lock refuses maintenance on the real file system: " + locked.detail);
      if (daemon.has_value()) SystemFileSystem().Close(*daemon);
    }
    // A trailing separator names the same Journal.
    const auto spelled = (root / "").string();
    const auto moved = QuarantineTornTail(fs, spelled);
    result |= Check(moved.status == MaintenanceStatus::kDone &&
                        read(root / Name(15)) == Bytes(Created(15, 3)) &&
                        read(root / moved.quarantine) == k_tail,
                    "quarantine on the real file system: " + moved.detail);
    const auto pruned = PruneClosedPrefix(fs, spelled, PruneOptions{});
    result |= Check(pruned.status == MaintenanceStatus::kDone &&
                        std::filesystem::exists(root / k_archive_directory / Name(1)) &&
                        std::filesystem::exists(root / k_archive_directory / Name(8)) &&
                        !std::filesystem::exists(root / Name(1)),
                    "prune on the real file system: " + pruned.detail);
    const auto replayed = ReplayJournal(SystemFileSystem(), root.string(), ReplayOptions{1});
    result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 16 &&
                        replayed.jobs.size() == 1,
                    "the pruned Journal replays from segment 15: " + replayed.detail);
  }
  {
    // A relative directory, spelled bare and with "./" and a trailing separator, names the same
    // Journal: both operations resolve it against the working directory.
    const auto name = root.filename().string() + "-relative";
    const std::filesystem::path relative(name);
    result |= Check(fs.EnsureDirectory(name) == IoError::kNone, "create the relative directory");
    write(relative / Name(1), ClosedJob(1, 1));
    write(relative / Name(8), ClosedJob(8, 2));
    write(relative / Name(15), Bytes(Created(15, 3)) + k_tail);
    const auto moved = QuarantineTornTail(fs, name);
    result |= Check(moved.status == MaintenanceStatus::kDone &&
                        read(relative / Name(15)) == Bytes(Created(15, 3)) &&
                        read(relative / moved.quarantine) == k_tail,
                    "quarantine through a bare relative path: " + moved.detail);
    const auto dotted = (std::filesystem::path(".") / name / "").string();
    const auto pruned = PruneClosedPrefix(fs, dotted, PruneOptions{});
    result |= Check(pruned.status == MaintenanceStatus::kDone &&
                        pruned.segments == std::vector<std::string>{Name(1), Name(8)} &&
                        std::filesystem::exists(relative / k_archive_directory / Name(1)) &&
                        std::filesystem::exists(relative / k_archive_directory / Name(8)) &&
                        !std::filesystem::exists(relative / Name(1)),
                    "prune through ./<relative>/: " + pruned.detail);
    const auto replayed = ReplayJournal(SystemFileSystem(), name, ReplayOptions{1});
    result |= Check(replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 16 &&
                        replayed.jobs.size() == 1,
                    "the relative Journal replays from segment 15: " + replayed.detail);
    std::error_code ignored;
    std::filesystem::remove_all(relative, ignored);
  }
  {
    // A prune interrupted after its first move resumes on the real file system: Job 1 spans
    // segments 1 and 5.
    const auto spanning = root / "spanning";
    result |=
        Check(fs.EnsureDirectory(spanning.string()) == IoError::kNone &&
                  fs.EnsureDirectory((spanning / k_archive_directory).string()) == IoError::kNone,
              "create the spanning Journal and its archive");
    write(spanning / Name(1), ClosedJobPart(1, 1, 0, 4));
    write(spanning / Name(5), ClosedJobPart(1, 1, 4, 7) + ClosedJob(8, 2));
    write(spanning / Name(15), Bytes(Created(15, 3)));
    result |= Check(
        fs.RenameNoReplace((spanning / Name(1)).string(),
                           (spanning / k_archive_directory / Name(1)).string()) == IoError::kNone,
        "move the first segment as an interrupted prune would");
    const auto resumed = PruneClosedPrefix(fs, spanning.string(), PruneOptions{});
    const auto replayed = ReplayJournal(SystemFileSystem(), spanning.string(), ReplayOptions{1});
    result |= Check(resumed.status == MaintenanceStatus::kDone &&
                        resumed.resumed == std::vector<std::string>{Name(1)} &&
                        resumed.moved == std::vector<std::string>{Name(5)} &&
                        replayed.status == ReplayStatus::kReplayed && replayed.next_sequence == 16,
                    "an interrupted prune resumes on the real file system: " + resumed.detail);
  }
  {
    // A directory, and on POSIX a dangling symbolic link, at an archive destination is a conflict
    // found before any move on the real file system.
    const auto blocked = root / "blocked";
    result |=
        Check(fs.EnsureDirectory(blocked.string()) == IoError::kNone &&
                  fs.EnsureDirectory((blocked / k_archive_directory).string()) == IoError::kNone,
              "create the blocked Journal and its archive");
    write(blocked / Name(1), ClosedJob(1, 1));
    write(blocked / Name(8), ClosedJob(8, 2));
    write(blocked / Name(15), Bytes(Created(15, 3)));
    std::filesystem::create_directory(blocked / k_archive_directory / Name(8));
    const auto refused = PruneClosedPrefix(fs, blocked.string(), PruneOptions{});
    result |=
        Check(refused.status == MaintenanceStatus::kRefused &&
                  refused.detail.find("journal_archive_conflict") != std::string::npos &&
                  refused.moved.empty() && std::filesystem::exists(blocked / Name(1)),
              "a directory at an archive destination refuses before any move: " + refused.detail);
#if !defined(_WIN32)
    std::filesystem::remove(blocked / k_archive_directory / Name(8));
    std::filesystem::create_symlink(root / "nowhere", blocked / k_archive_directory / Name(8));
    const auto dangling = PruneClosedPrefix(fs, blocked.string(), PruneOptions{});
    result |= Check(
        dangling.status == MaintenanceStatus::kRefused && dangling.moved.empty() &&
            std::filesystem::exists(blocked / Name(1)),
        "a dangling link at an archive destination refuses before any move: " + dangling.detail);
    // archive/ itself a link to a directory outside the Journal: nothing moves there.
    const auto outside = root / "outside";
    std::filesystem::create_directory(outside);
    std::filesystem::remove_all(blocked / k_archive_directory);
    std::filesystem::create_directory_symlink(outside, blocked / k_archive_directory);
    const auto linked = PruneClosedPrefix(fs, blocked.string(), PruneOptions{});
    result |=
        Check(linked.status == MaintenanceStatus::kRefused && linked.moved.empty() &&
                  std::filesystem::is_empty(outside) && std::filesystem::exists(blocked / Name(1)),
              "a linked archive/ refuses on the real file system: " + linked.detail);
#endif
  }
  {
    // The quarantine name is a hard link to a segment whose torn tail starts at byte 0: refused,
    // and the segment keeps its bytes.
    const auto aliased = root / "aliased";
    result |=
        Check(fs.EnsureDirectory(aliased.string()) == IoError::kNone, "create the aliased Journal");
    write(aliased / Name(1), k_tail);
    std::error_code linked;
    std::filesystem::create_hard_link(aliased / Name(1), aliased / (Name(1) + ".torn-0"), linked);
    const auto refused = QuarantineTornTail(fs, aliased.string());
    result |=
        Check(!linked && refused.status == MaintenanceStatus::kRefused &&
                  refused.detail.find("has other hard links") != std::string::npos &&
                  read(aliased / Name(1)) == k_tail,
              "a hard-linked quarantine file refuses on the real file system: " + refused.detail);
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return result;
}

}  // namespace
}  // namespace sitometron::test

int main(int argc, char** argv) try {
  if (argc != 2) {
    std::cerr << "usage: sitometron_journal_maintenance_tests <check>\n";
    return 2;
  }
  using namespace sitometron::test;
  const std::string_view check = argv[1];
  if (check == "journal_prune_prefix_only") return PrunePrefixOnly();
  if (check == "journal_prune_refuses_open_jobs") return PruneRefusesOpenJobs();
  if (check == "journal_prune_keeps_last_record") return PruneKeepsLastRecord();
  if (check == "journal_quarantine_torn_tail") return QuarantineTail();
  if (check == "journal_maintenance_requires_lock") return RequiresLock();
  if (check == "journal_maintenance_faults") return MaintenanceFaults();
  if (check == "journal_maintenance_system_file_system") return SystemFileSystemCheck();
  std::cerr << "unknown check " << check << '\n';
  return 2;
} catch (const std::exception& error) {
  std::cerr << "journal_maintenance: unexpected exception: " << error.what() << '\n';
  return 1;
} catch (...) {
  std::cerr << "journal_maintenance: unexpected non-standard exception\n";
  return 1;
}
