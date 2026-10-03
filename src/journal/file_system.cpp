#include "sitometron/journal/file_system.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sitometron/journal/maintenance.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h must precede sddl.h.
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace sitometron::journal {
namespace {

// Directories that do not exist yet, from the outermost down to `path` inclusive.
std::vector<std::filesystem::path> MissingDirectories(const std::filesystem::path& path) {
  std::vector<std::filesystem::path> missing;
  std::error_code code;
  auto current = path;
  while (!current.empty() && !std::filesystem::exists(current, code)) {
    missing.push_back(current);
    auto parent = current.parent_path();
    if (parent == current) break;
    current = std::move(parent);
  }
  std::reverse(missing.begin(), missing.end());
  return missing;
}

// The directory named by `directory` without "." / ".." components or a trailing separator, so
// the Journal directory itself (not a trailing empty component) is the last element created.
std::filesystem::path NormalizedDirectory(const std::string& directory) {
  auto path = std::filesystem::path(directory).lexically_normal();
  if (path.filename().empty() && path.has_parent_path() && path != path.root_path()) {
    path = path.parent_path();
  }
  return path;
}

std::filesystem::path ParentOf(const std::filesystem::path& path) {
  return path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
}

std::optional<std::vector<std::string>> ListRegularFiles(const std::string& directory,
                                                         IoError& error) {
  std::error_code code;
  std::vector<std::string> names;
  for (std::filesystem::directory_iterator it(directory, code), end; !code && it != end;
       it.increment(code)) {
    std::error_code status_code;
    if (it->is_regular_file(status_code) && !status_code) {
      names.push_back(it->path().filename().string());
    }
  }
  if (code) {
    error = IoError::kOther;
    return std::nullopt;
  }
  return names;
}

// The kind of entry at `path`, without following a symbolic link.
std::optional<EntryKind> EntryAt(const std::string& path, IoError& error) {
  std::error_code code;
  const auto status = std::filesystem::symlink_status(std::filesystem::path(path), code);
  if (status.type() == std::filesystem::file_type::not_found) return EntryKind::kNone;
  if (code) {
    error = IoError::kOther;
    return std::nullopt;
  }
  switch (status.type()) {
    case std::filesystem::file_type::regular:
      return EntryKind::kRegularFile;
    case std::filesystem::file_type::directory:
      return EntryKind::kDirectory;
    case std::filesystem::file_type::symlink:
      return EntryKind::kSymbolicLink;
    default:
      return EntryKind::kOther;
  }
}

// The number of hard links to the file at `path`.
std::optional<std::uintmax_t> HardLinkCount(const std::string& path, IoError& error) {
  std::error_code code;
  const auto count = std::filesystem::hard_link_count(std::filesystem::path(path), code);
  if (code) {
    error = IoError::kOther;
    return std::nullopt;
  }
  return count;
}

std::optional<std::string> ReadWholeFile(const std::string& path, IoError& error) {
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input) {
    error = IoError::kOther;
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad()) {
    error = IoError::kOther;
    return std::nullopt;
  }
  return content;
}

#if defined(_WIN32)

HANDLE ToHandle(FileHandle file) { return reinterpret_cast<HANDLE>(file.value); }
FileHandle FromHandle(HANDLE handle) { return FileHandle{reinterpret_cast<std::intptr_t>(handle)}; }

IoError FromLastError() {
  switch (GetLastError()) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return IoError::kNoSpace;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return IoError::kExists;
    case ERROR_LOCK_VIOLATION:
    case ERROR_SHARING_VIOLATION:
      return IoError::kLocked;
    default:
      return IoError::kOther;
  }
}

bool IsNtfs(HANDLE handle) {
  std::array<wchar_t, MAX_PATH + 1> name{};
  if (!GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, name.data(),
                                     static_cast<DWORD>(name.size()))) {
    return false;
  }
  return std::wstring_view(name.data()) == L"NTFS";
}

// Owner-only access (ADR-0006 §2): a protected DACL whose single ACE grants full control to the
// object's owner (OWNER RIGHTS, S-1-3-4) and is inherited by files created in the directory.
class OwnerOnlySecurity {
 public:
  OwnerOnlySecurity() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
      descriptor_ = descriptor;
      attributes_.nLength = sizeof(attributes_);
      attributes_.lpSecurityDescriptor = descriptor_;
      attributes_.bInheritHandle = FALSE;
    }
  }
  ~OwnerOnlySecurity() {
    if (descriptor_ != nullptr) LocalFree(descriptor_);
  }
  OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
  OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;
  // nullptr when the descriptor could not be built; callers must then refuse to create.
  SECURITY_ATTRIBUTES* Attributes() { return descriptor_ != nullptr ? &attributes_ : nullptr; }

 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};
};

// The platform operations shared by the daemon's FileSystem and the offline
// MaintenanceFileSystem. `Base` is one of the two interfaces.
template <class Base>
class WindowsFiles : public Base {
 public:
  IoError EnsureDirectory(const std::string& directory) override {
    std::error_code code;
    const auto path = NormalizedDirectory(directory);
    if (std::filesystem::is_directory(path, code)) return IoError::kNone;
    OwnerOnlySecurity security;
    if (security.Attributes() == nullptr) return IoError::kUnsupported;
    // Create each missing directory from the outermost down, and make every new entry durable in
    // its parent before any record can live below it. Only the Journal directory itself gets the
    // owner-only DACL; created ancestors keep default permissions.
    for (const auto& missing : MissingDirectories(path)) {
      if (SECURITY_ATTRIBUTES* attributes = missing == path ? security.Attributes() : nullptr;
          !CreateDirectoryW(missing.c_str(), attributes) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        return IoError::kOther;
      }
      if (const auto synced = SyncDirectory(ParentOf(missing).string()); synced != IoError::kNone) {
        return synced;
      }
    }
    return std::filesystem::is_directory(path, code) ? IoError::kNone : IoError::kOther;
  }
  std::optional<FileHandle> Lock(const std::string& path, IoError& error) override {
    OwnerOnlySecurity security;
    if (security.Attributes() == nullptr) {
      error = IoError::kUnsupported;
      return std::nullopt;
    }
    const HANDLE handle =
        CreateFileW(std::filesystem::path(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, security.Attributes(), OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      error = FromLastError();
      return std::nullopt;
    }
    if (OVERLAPPED overlapped = {}; !LockFileEx(
            handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
      error = IoError::kLocked;
      CloseHandle(handle);
      return std::nullopt;
    }
    return FromHandle(handle);
  }
  std::optional<std::vector<std::string>> List(const std::string& directory,
                                               IoError& error) override {
    return ListRegularFiles(directory, error);
  }
  std::optional<std::string> ReadAll(const std::string& path, IoError& error) override {
    return ReadWholeFile(path, error);
  }
  std::optional<FileHandle> OpenAppend(const std::string& path, bool create_new,
                                       IoError& error) override {
    // FlushFileBuffers requires GENERIC_WRITE, so the handle is opened for writing and positioned
    // at the end; the single writer keeps every later write at the end. New segments get the
    // owner-only DACL explicitly.
    OwnerOnlySecurity security;
    if (create_new && security.Attributes() == nullptr) {
      error = IoError::kUnsupported;
      return std::nullopt;
    }
    const HANDLE handle =
        CreateFileW(std::filesystem::path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                    create_new ? security.Attributes() : nullptr,
                    create_new ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      error = FromLastError();
      return std::nullopt;
    }
    if (const LARGE_INTEGER zero = {}; !SetFilePointerEx(handle, zero, nullptr, FILE_END)) {
      error = IoError::kOther;
      CloseHandle(handle);
      return std::nullopt;
    }
    return FromHandle(handle);
  }
  WriteOutcome Write(FileHandle file, std::string_view bytes) noexcept override {
    const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1U << 30U));
    DWORD written = 0;
    if (!WriteFile(ToHandle(file), bytes.data(), request, &written, nullptr)) {
      return {written, FromLastError()};
    }
    return {written, IoError::kNone};
  }
  IoError SyncData(FileHandle file) noexcept override {
    return FlushFileBuffers(ToHandle(file)) ? IoError::kNone : IoError::kOther;
  }
  IoError SyncDirectory(const std::string& directory) noexcept override {
    // The narrow-to-wide path conversion allocates and may throw; this call must not.
    std::wstring wide;
    try {
      wide = std::filesystem::path(directory).wstring();
    } catch (const std::exception&) {
      return IoError::kOther;
    }
    const HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return IoError::kUnsupported;
    IoError result = IoError::kNone;
    if (!IsNtfs(handle)) {
      result = IoError::kUnsupported;
    } else if (!FlushFileBuffers(handle)) {
      result = IoError::kOther;
    }
    CloseHandle(handle);
    return result;
  }
  void Close(FileHandle file) noexcept override { CloseHandle(ToHandle(file)); }
};

class WindowsFileSystem final : public WindowsFiles<FileSystem> {};

class WindowsMaintenanceFileSystem final : public WindowsFiles<MaintenanceFileSystem> {
 public:
  IoError Truncate(const std::string& path, std::uint64_t size) override {
    const HANDLE handle =
        CreateFileW(std::filesystem::path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return FromLastError();
    const auto cut = [handle, size]() {
      LARGE_INTEGER current;
      current.QuadPart = 0;  // the member GetFileSizeEx fills and this function reads
      if (!GetFileSizeEx(handle, &current)) return FromLastError();
      // A cut never extends the file.
      if (static_cast<std::uint64_t>(current.QuadPart) < size) return IoError::kOther;
      LARGE_INTEGER position;
      position.QuadPart = static_cast<LONGLONG>(size);
      if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        return FromLastError();
      }
      return FlushFileBuffers(handle) ? IoError::kNone : IoError::kOther;
    };
    const IoError result = cut();
    CloseHandle(handle);
    return result;
  }
  IoError RenameNoReplace(const std::string& from, const std::string& to) override {
    const auto source = std::filesystem::path(from).wstring();
    // Without MOVEFILE_REPLACE_EXISTING an existing target fails with ERROR_ALREADY_EXISTS.
    if (const auto target = std::filesystem::path(to).wstring();
        MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
      return IoError::kNone;
    }
    return FromLastError();
  }
  std::optional<EntryKind> Entry(const std::string& path, IoError& error) override {
    return EntryAt(path, error);
  }
  std::optional<std::uintmax_t> HardLinks(const std::string& path, IoError& error) override {
    return HardLinkCount(path, error);
  }
};

#else

int ToDescriptor(FileHandle file) { return static_cast<int>(file.value); }
FileHandle FromDescriptor(int descriptor) { return FileHandle{descriptor}; }

IoError FromErrno(int code) {
  switch (code) {
    case EINTR:
      return IoError::kInterrupted;
    case ENOSPC:
    case EDQUOT:
      return IoError::kNoSpace;
    case EEXIST:
      return IoError::kExists;
    case EWOULDBLOCK:
      return IoError::kLocked;
    case EINVAL:
    case ENOTSUP:
      return IoError::kUnsupported;
    default:
      return IoError::kOther;
  }
}

// The platform operations shared by the daemon's FileSystem and the offline
// MaintenanceFileSystem. `Base` is one of the two interfaces.
template <class Base>
class PosixFiles : public Base {
 public:
  IoError EnsureDirectory(const std::string& directory) override {
    std::error_code code;
    const auto path = NormalizedDirectory(directory);
    if (std::filesystem::is_directory(path, code)) return IoError::kNone;
    // Create each missing directory from the outermost down, and make every new entry durable in
    // its parent before any record can live below it. Only the Journal directory itself is 0700;
    // created ancestors keep the default mode filtered by the umask.
    for (const auto& missing : MissingDirectories(path)) {
      const mode_t mode = missing == path ? S_IRWXU : (S_IRWXU | S_IRWXG | S_IRWXO);
      if (::mkdir(missing.c_str(), mode) != 0 && errno != EEXIST) return FromErrno(errno);
      if (const auto synced = SyncDirectory(ParentOf(missing).string()); synced != IoError::kNone) {
        return synced;
      }
    }
    return std::filesystem::is_directory(path, code) ? IoError::kNone : IoError::kOther;
  }
  std::optional<FileHandle> Lock(const std::string& path, IoError& error) override {
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      error = FromErrno(errno);
      return std::nullopt;
    }
    int status = 0;
    do {
      status = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (status != 0 && errno == EINTR);
    if (status != 0) {
      error = errno == EWOULDBLOCK ? IoError::kLocked : FromErrno(errno);
      ::close(descriptor);
      return std::nullopt;
    }
    return FromDescriptor(descriptor);
  }
  std::optional<std::vector<std::string>> List(const std::string& directory,
                                               IoError& error) override {
    return ListRegularFiles(directory, error);
  }
  std::optional<std::string> ReadAll(const std::string& path, IoError& error) override {
    return ReadWholeFile(path, error);
  }
  std::optional<FileHandle> OpenAppend(const std::string& path, bool create_new,
                                       IoError& error) override {
    int flags = O_WRONLY | O_APPEND | O_CLOEXEC;
    if (create_new) flags |= O_CREAT | O_EXCL;
    int descriptor = -1;
    do {
      descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
      error = FromErrno(errno);
      return std::nullopt;
    }
    return FromDescriptor(descriptor);
  }
  WriteOutcome Write(FileHandle file, std::string_view bytes) noexcept override {
    const auto count = ::write(ToDescriptor(file), bytes.data(), bytes.size());
    if (count < 0) return {0, FromErrno(errno)};
    return {static_cast<std::size_t>(count), IoError::kNone};
  }
  IoError SyncData(FileHandle file) noexcept override {
    // A failed data sync is never retried (ADR-0006 §4), not even after EINTR.
    return ::fdatasync(ToDescriptor(file)) == 0 ? IoError::kNone : IoError::kOther;
  }
  IoError SyncDirectory(const std::string& directory) noexcept override {
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return FromErrno(errno);
    const IoError result = ::fsync(descriptor) == 0 ? IoError::kNone : FromErrno(errno);
    ::close(descriptor);
    return result == IoError::kInterrupted ? IoError::kOther : result;
  }
  void Close(FileHandle file) noexcept override { ::close(ToDescriptor(file)); }
};

class PosixFileSystem final : public PosixFiles<FileSystem> {};

class PosixMaintenanceFileSystem final : public PosixFiles<MaintenanceFileSystem> {
 public:
  IoError Truncate(const std::string& path, std::uint64_t size) override {
    int descriptor = -1;
    do {
      descriptor = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) return FromErrno(errno);
    const auto cut = [descriptor, size]() {
      struct stat status = {};
      if (::fstat(descriptor, &status) != 0) return FromErrno(errno);
      // A cut never extends the file.
      if (static_cast<std::uint64_t>(status.st_size) < size) return IoError::kOther;
      if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) return FromErrno(errno);
      // A failed data sync is never retried (ADR-0006 §4).
      return ::fdatasync(descriptor) == 0 ? IoError::kNone : IoError::kOther;
    };
    const IoError result = cut();
    ::close(descriptor);
    return result == IoError::kInterrupted ? IoError::kOther : result;
  }
  IoError RenameNoReplace(const std::string& from, const std::string& to) override {
#if defined(__linux__)
    // RENAME_NOREPLACE fails with EEXIST instead of replacing the target atomically.
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
      return IoError::kNone;
    }
    return FromErrno(errno);
#else
    (void)from;
    (void)to;
    return IoError::kUnsupported;  // no atomic no-replace rename is wired for this platform
#endif
  }
  std::optional<EntryKind> Entry(const std::string& path, IoError& error) override {
    return EntryAt(path, error);
  }
  std::optional<std::uintmax_t> HardLinks(const std::string& path, IoError& error) override {
    return HardLinkCount(path, error);
  }
};

#endif

}  // namespace

FileSystem& SystemFileSystem() {
#if defined(_WIN32)
  static WindowsFileSystem file_system;
#else
  static PosixFileSystem file_system;
#endif
  return file_system;
}

MaintenanceFileSystem& SystemMaintenanceFileSystem() {
#if defined(_WIN32)
  static WindowsMaintenanceFileSystem file_system;
#else
  static PosixMaintenanceFileSystem file_system;
#endif
  return file_system;
}

std::string JoinPath(std::string_view directory, std::string_view name) {
  return (std::filesystem::path(std::string(directory)) / std::string(name)).string();
}

}  // namespace sitometron::journal
