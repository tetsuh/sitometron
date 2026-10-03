#ifndef SITOMETRON_JOURNAL_MAINTENANCE_HPP_
#define SITOMETRON_JOURNAL_MAINTENANCE_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/file_system.hpp"

// Offline JobJournal maintenance (Accepted ADR-0006, Sections 5 and 8; OPS-005).
//
// Each operation takes journal.lock, so it refuses while a daemon holds the Journal. It validates
// the whole Journal by replay, changes it, and replays it again. It never touches a complete record
// and never resets or reuses a sequence. The daemon's FileSystem offers no truncate or rename
// (JRN-007); only MaintenanceFileSystem does, and only these offline operations take one.
namespace sitometron::journal {

// What a directory entry is, read without following a symbolic link.
enum class EntryKind { kNone, kRegularFile, kDirectory, kSymbolicLink, kOther };

class MaintenanceFileSystem : public FileSystem {
 public:
  // Cuts the file at `path` back to `size` bytes and makes the new size durable.
  [[nodiscard]] virtual IoError Truncate(const std::string& path, std::uint64_t size) = 0;
  // Renames the file `from` to `to`. Returns kExists and changes nothing when `to` exists. The
  // caller makes both directory entries durable.
  [[nodiscard]] virtual IoError RenameNoReplace(const std::string& from, const std::string& to) = 0;
  // The kind of entry at `path`, kNone when there is none. A symbolic link is reported as one and
  // not followed, so a dangling link is kSymbolicLink. nullopt with `error` set when that cannot be
  // told.
  [[nodiscard]] virtual std::optional<EntryKind> Entry(const std::string& path, IoError& error) = 0;
  // The number of hard links to the file at `path`. nullopt with `error` set when it cannot be
  // read.
  [[nodiscard]] virtual std::optional<std::uintmax_t> HardLinks(const std::string& path,
                                                                IoError& error) = 0;
};

// The maintenance file system of the current platform. The daemon never takes it.
[[nodiscard]] MaintenanceFileSystem& SystemMaintenanceFileSystem();

enum class MaintenanceStatus {
  kDone,         // the Journal was changed as reported
  kPlanned,      // dry run: the Journal was not changed; the result reports what would change
  kNothingToDo,  // the Journal needs no change and was not changed
  kRefused,      // the Journal was not changed; `detail` names the code and location
  kFailed,       // an I/O step failed; running the operation again continues it
};

// Subdirectory of the Journal directory that receives pruned segments. Replay reads only the
// regular files directly inside the Journal directory, so it never reads this one.
inline constexpr const char* k_archive_directory = "archive";

// `segment`, `offset`, `quarantine`, and `bytes` are filled once a torn tail is found, so a later
// refusal or failure still names it. `next_sequence` is the Journal's next sequence for kDone and
// kNothingToDo, and 0 otherwise or when `sequence_exhausted` is set.
struct QuarantineResult {
  MaintenanceStatus status = MaintenanceStatus::kRefused;
  std::string detail;               // result code with its location
  std::string segment;              // segment whose torn tail was quarantined
  std::uint64_t offset = 0;         // the cut: the end of the segment's last complete record
  std::string quarantine;           // quarantine file name, next to the segment
  std::uint64_t bytes = 0;          // bytes moved from the segment into the quarantine file
  std::uint64_t next_sequence = 0;  // after the cut
  // After the cut the last record carries UINT64_MAX: no next sequence exists, and startup refuses
  // with journal_sequence_exhausted (ADR-0003, ADR-0006 Section 5).
  bool sequence_exhausted = false;
};

// Moves the torn tail of the highest segment (ADR-0006 Section 5) into
// `<segment>.torn-<offset>` next to it, makes that file durable, and cuts the segment back to its
// last complete record. A clean Journal is kNothingToDo; any other replay refusal is kRefused. When
// the highest segment still ends at the offset its quarantine file names, that cut is synced again
// first (no byte changes), since a cut whose sync failed looks the same on disk.
[[nodiscard]] QuarantineResult QuarantineTornTail(MaintenanceFileSystem& file_system,
                                                  const std::string& directory);

struct PruneOptions {
  bool dry_run = false;
};

// `segments`, `records`, `jobs`, and `first_retained_sequence` describe the prefix this run
// completed (kDone) or would complete (kPlanned); they are empty or 0 for every other status.
// `moved` always names what this run actually moved, so a kFailed run that moved some segments
// says so. `next_sequence` is 0 when the Journal could not be replayed.
struct PruneResult {
  MaintenanceStatus status = MaintenanceStatus::kRefused;
  std::string detail;                 // result code with its location
  std::vector<std::string> segments;  // prefix segments moved (planned for a dry run), lowest first
  std::uint64_t records = 0;          // records in those segments
  std::vector<core::Uuid> jobs;       // Jobs whose every record is in the prefix, in creation order
  std::uint64_t first_retained_sequence = 0;  // where replay starts after pruning
  std::uint64_t next_sequence = 0;            // unchanged by pruning
  // Segments this run actually moved into archive/, lowest first, whatever the status: a failed run
  // names what it already moved.
  std::vector<std::string> moved;
  // Segments an interrupted earlier run already moved into archive/ and this run completes.
  std::vector<std::string> resumed;
};

// Moves the longest prunable prefix of sealed segments (ADR-0006 Section 8) into the archive
// subdirectory, lowest first. Every Job with a record in the prefix has its last record there and
// is terminal, released, and cleaned up after it. The highest non-empty segment always stays. The
// retained Journal is verified by replay after the moves; moved bytes are not read back.
//
// When a Job spans a segment boundary inside the prefix, a crash or failure between moves leaves a
// Journal whose first retained segment holds records of a Job created in an archived segment. The
// daemon refuses that Journal (journal_corrupt). Running prune again resumes: the shortest run of
// archived segments directly before the Journal that makes replay succeed is read from archive/,
// and when it lies inside the prunable prefix the remaining moves complete. Deleting archive/
// before that rerun makes the Journal unrecoverable by this tool.
[[nodiscard]] PruneResult PruneClosedPrefix(MaintenanceFileSystem& file_system,
                                            const std::string& directory,
                                            const PruneOptions& options);

}  // namespace sitometron::journal

#endif  // SITOMETRON_JOURNAL_MAINTENANCE_HPP_
