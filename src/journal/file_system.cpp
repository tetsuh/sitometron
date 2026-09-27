#include "sitometron/journal/file_system.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace sitometron::journal {
namespace {

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

class WindowsFileSystem final : public FileSystem {
 public:
  IoError EnsureDirectory(const std::string& directory) override {
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(directory), code);
    return code ? IoError::kOther : IoError::kNone;
  }
  std::optional<FileHandle> Lock(const std::string& path, IoError& error) override {
    const HANDLE handle = CreateFileW(
        std::filesystem::path(path).c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    // at the end; the single writer keeps every later write at the end.
    const HANDLE handle =
        CreateFileW(std::filesystem::path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
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
  WriteOutcome Write(FileHandle file, std::string_view bytes) override {
    const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1U << 30U));
    DWORD written = 0;
    if (!WriteFile(ToHandle(file), bytes.data(), request, &written, nullptr)) {
      return {written, FromLastError()};
    }
    return {written, IoError::kNone};
  }
  IoError SyncData(FileHandle file) override {
    return FlushFileBuffers(ToHandle(file)) ? IoError::kNone : IoError::kOther;
  }
  IoError SyncDirectory(const std::string& directory) override {
    const HANDLE handle = CreateFileW(std::filesystem::path(directory).c_str(), GENERIC_WRITE,
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

class PosixFileSystem final : public FileSystem {
 public:
  IoError EnsureDirectory(const std::string& directory) override {
    std::error_code code;
    const std::filesystem::path path(directory);
    if (std::filesystem::is_directory(path, code)) return IoError::kNone;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), code);
    if (::mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST) return FromErrno(errno);
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
  WriteOutcome Write(FileHandle file, std::string_view bytes) override {
    const auto count = ::write(ToDescriptor(file), bytes.data(), bytes.size());
    if (count < 0) return {0, FromErrno(errno)};
    return {static_cast<std::size_t>(count), IoError::kNone};
  }
  IoError SyncData(FileHandle file) override {
    // A failed data sync is never retried (ADR-0006 §4), not even after EINTR.
    return ::fdatasync(ToDescriptor(file)) == 0 ? IoError::kNone : IoError::kOther;
  }
  IoError SyncDirectory(const std::string& directory) override {
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return FromErrno(errno);
    const IoError result = ::fsync(descriptor) == 0 ? IoError::kNone : FromErrno(errno);
    ::close(descriptor);
    return result == IoError::kInterrupted ? IoError::kOther : result;
  }
  void Close(FileHandle file) noexcept override { ::close(ToDescriptor(file)); }
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

std::string JoinPath(std::string_view directory, std::string_view name) {
  return (std::filesystem::path(std::string(directory)) / std::string(name)).string();
}

}  // namespace sitometron::journal
