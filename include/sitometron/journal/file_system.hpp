#ifndef SITOMETRON_JOURNAL_FILE_SYSTEM_HPP_
#define SITOMETRON_JOURNAL_FILE_SYSTEM_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// File-system seam of the JobJournal segment writer (Accepted ADR-0006, Sections 2-4).
//
// Every operation the writer performs goes through this interface so tests can inject faults.
// The interface deliberately offers no truncate, rename, or delete: the daemon never rewrites or
// removes Journal data (JRN-007).
namespace sitometron::journal {

// Opaque handle owned by a FileSystem implementation.
struct FileHandle {
  std::intptr_t value = -1;
  friend bool operator==(const FileHandle&, const FileHandle&) = default;
};

enum class IoError {
  kNone,
  kInterrupted,  // retry the same call
  kNoSpace,      // out of space or quota
  kExists,       // exclusive create found an existing file
  kLocked,       // the lock is held by another owner
  kUnsupported,  // the platform or file system cannot provide the required guarantee
  kOther,
};

struct WriteOutcome {
  std::size_t written = 0;  // bytes accepted by this call
  IoError error = IoError::kNone;
};

class FileSystem {
 public:
  virtual ~FileSystem() = default;

  // Creates the directory with owner-only access if it does not exist, and makes the new
  // directory's entry in its parent durable.
  [[nodiscard]] virtual IoError EnsureDirectory(const std::string& directory) = 0;
  // Takes an exclusive advisory lock on `path` (created if absent) for the handle's lifetime.
  [[nodiscard]] virtual std::optional<FileHandle> Lock(const std::string& path, IoError& error) = 0;
  // Names of the regular files directly inside `directory`.
  [[nodiscard]] virtual std::optional<std::vector<std::string>> List(const std::string& directory,
                                                                     IoError& error) = 0;
  // Entire content of a file.
  [[nodiscard]] virtual std::optional<std::string> ReadAll(const std::string& path,
                                                           IoError& error) = 0;
  // Opens `path` for appending with owner-only access. `create_new` requires that the file does
  // not exist yet.
  [[nodiscard]] virtual std::optional<FileHandle> OpenAppend(const std::string& path,
                                                             bool create_new, IoError& error) = 0;
  // One write call; it may accept fewer bytes than requested.
  [[nodiscard]] virtual WriteOutcome Write(FileHandle file, std::string_view bytes) = 0;
  // Makes the file's data durable (fdatasync / FlushFileBuffers).
  [[nodiscard]] virtual IoError SyncData(FileHandle file) = 0;
  // Makes the directory's entries durable (POSIX fsync on the directory; Windows
  // FlushFileBuffers on a FILE_FLAG_BACKUP_SEMANTICS handle, NTFS only).
  [[nodiscard]] virtual IoError SyncDirectory(const std::string& directory) = 0;
  virtual void Close(FileHandle file) noexcept = 0;
};

// The process-wide file system of the current platform.
[[nodiscard]] FileSystem& SystemFileSystem();

// Joins a directory and a file name with the platform separator.
[[nodiscard]] std::string JoinPath(std::string_view directory, std::string_view name);

}  // namespace sitometron::journal

#endif  // SITOMETRON_JOURNAL_FILE_SYSTEM_HPP_
