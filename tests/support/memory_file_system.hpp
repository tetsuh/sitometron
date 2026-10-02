#ifndef SITOMETRON_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_HPP_
#define SITOMETRON_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "sitometron/journal/file_system.hpp"
#include "sitometron/journal/maintenance.hpp"

namespace sitometron::test {
using journal::FileHandle;
using journal::FileSystem;
using journal::IoError;
using journal::JoinPath;
using journal::MaintenanceFileSystem;
using journal::WriteOutcome;

// In-memory file system that records every operation and injects scripted faults. It also offers
// the offline maintenance operations; the daemon only ever sees it as a FileSystem.
class MemoryFileSystem final : public MaintenanceFileSystem {
 public:
  struct WriteFault {
    std::size_t accept = 0;  // bytes accepted before the error
    IoError error = IoError::kOther;
  };

  std::map<std::string, std::string> files;
  std::set<std::string> directories;
  std::set<std::string> durable_entries;  // files whose directory entry was synced
  std::vector<std::string> log;
  std::optional<WriteFault> next_write_fault;
  std::size_t interrupted_writes = 0;  // EINTR before the next real write
  std::size_t short_write_limit = 0;   // 0 = unlimited bytes per call
  bool fail_next_sync = false;
  bool fail_next_directory_sync = false;
  std::size_t directory_syncs_until_failure = 0;  // when nonzero, the Nth directory sync fails
  bool fail_next_create = false;
  bool throw_on_create = false;
  // Runs inside the throwing create, while the failing Commit() holds the journal lock.
  std::function<void()> on_throwing_create;
  bool throw_on_list = false;
  bool throw_on_read = false;
  bool fail_next_mkdir = false;
  bool fail_next_truncate = false;
  bool fail_next_rename = false;
  // The next call reports success but changes nothing, as a misbehaving file system might.
  bool ignore_next_truncate = false;
  bool fail_after_next_truncate = false;  // the next truncate cuts, then reports a failed sync
  std::optional<std::uint64_t> next_truncate_to;  // the next truncate cuts here and reports success
  bool ignore_next_rename = false;
  bool throw_on_rename = false;
  IoError create_error = IoError::kNoSpace;

  IoError EnsureDirectory(const std::string& directory) override {
    log.push_back("mkdir " + directory);
    if (fail_next_mkdir) {
      fail_next_mkdir = false;
      return IoError::kOther;
    }
    directories.insert(directory);
    return IoError::kNone;
  }
  std::optional<FileHandle> Lock(const std::string& path, IoError& error) override {
    log.push_back("lock " + path);
    if (locked_.count(path) != 0) {
      error = IoError::kLocked;
      return std::nullopt;
    }
    locked_.insert(path);
    return Register(path);
  }
  std::optional<std::vector<std::string>> List(const std::string& directory,
                                               IoError& error) override {
    log.push_back("list " + directory);
    if (throw_on_list) throw std::runtime_error("injected list exception");
    if (directories.count(directory) == 0) {
      error = IoError::kOther;
      return std::nullopt;
    }
    std::vector<std::string> names;
    const auto prefix = JoinPath(directory, "");
    for (const auto& [path, content] : files) {
      // Only files directly inside the directory, like the system implementation.
      if (path.rfind(prefix, 0) == 0 &&
          path.find_first_of("/\\", prefix.size()) == std::string::npos) {
        names.push_back(path.substr(prefix.size()));
      }
    }
    return names;
  }
  std::optional<std::string> ReadAll(const std::string& path, IoError& error) override {
    log.push_back("read " + path);
    if (throw_on_read) throw std::runtime_error("injected read exception");
    const auto it = files.find(path);
    if (it == files.end()) {
      error = IoError::kOther;
      return std::nullopt;
    }
    return it->second;
  }
  std::optional<FileHandle> OpenAppend(const std::string& path, bool create_new,
                                       IoError& error) override {
    log.push_back(std::string(create_new ? "create " : "open ") + path);
    if (create_new && throw_on_create) {
      throw_on_create = false;
      if (on_throwing_create) on_throwing_create();
      throw std::runtime_error("injected create exception");
    }
    if (create_new) {
      if (fail_next_create) {
        fail_next_create = false;
        error = create_error;
        return std::nullopt;
      }
      if (files.count(path) != 0) {
        error = IoError::kExists;
        return std::nullopt;
      }
      files[path];
    } else if (files.count(path) == 0) {
      error = IoError::kOther;
      return std::nullopt;
    }
    return Register(path);
  }
  WriteOutcome Write(FileHandle file, std::string_view bytes) noexcept override {
    log.push_back("write " + std::to_string(bytes.size()));
    if (interrupted_writes > 0) {
      --interrupted_writes;
      return {0, IoError::kInterrupted};
    }
    auto& content = files[handles_.at(file.value)];
    if (next_write_fault) {
      const auto fault = *next_write_fault;
      next_write_fault.reset();
      const auto accepted = std::min(fault.accept, bytes.size());
      content.append(bytes.substr(0, accepted));
      return {accepted, fault.error};
    }
    auto accepted = bytes.size();
    if (short_write_limit != 0) accepted = std::min(accepted, short_write_limit);
    content.append(bytes.substr(0, accepted));
    return {accepted, IoError::kNone};
  }
  IoError SyncData(FileHandle file) noexcept override {
    log.push_back("sync " + handles_.at(file.value));
    if (fail_next_sync) {
      fail_next_sync = false;
      return IoError::kOther;
    }
    return IoError::kNone;
  }
  IoError SyncDirectory(const std::string& directory) noexcept override {
    log.push_back("syncdir " + directory);
    if (directory_syncs_until_failure != 0 && --directory_syncs_until_failure == 0) {
      return IoError::kUnsupported;
    }
    if (fail_next_directory_sync) {
      fail_next_directory_sync = false;
      return IoError::kUnsupported;
    }
    const auto prefix = JoinPath(directory, "");
    for (const auto& [path, content] : files) {
      if (path.rfind(prefix, 0) == 0 &&
          path.find_first_of("/\\", prefix.size()) == std::string::npos) {
        durable_entries.insert(path);
      }
    }
    return IoError::kNone;
  }
  IoError Truncate(const std::string& path, std::uint64_t size) override {
    log.push_back("truncate " + path + " " + std::to_string(size));
    if (fail_next_truncate) {
      fail_next_truncate = false;
      return IoError::kOther;
    }
    if (ignore_next_truncate) {
      ignore_next_truncate = false;
      return IoError::kNone;
    }
    const auto it = files.find(path);
    if (it == files.end() || size > it->second.size()) return IoError::kOther;
    if (next_truncate_to) {
      it->second.resize(static_cast<std::size_t>(*next_truncate_to));
      next_truncate_to.reset();
      return IoError::kNone;
    }
    it->second.resize(static_cast<std::size_t>(size));
    if (fail_after_next_truncate) {
      fail_after_next_truncate = false;
      return IoError::kOther;
    }
    return IoError::kNone;
  }
  IoError RenameNoReplace(const std::string& from, const std::string& to) override {
    log.push_back("rename " + from + " " + to);
    if (throw_on_rename) throw std::runtime_error("injected rename exception");
    if (fail_next_rename) {
      fail_next_rename = false;
      return IoError::kOther;
    }
    if (ignore_next_rename) {
      ignore_next_rename = false;
      return IoError::kNone;
    }
    const auto it = files.find(from);
    if (it == files.end()) return IoError::kOther;
    if (files.count(to) != 0) return IoError::kExists;
    files[to] = std::move(it->second);
    files.erase(from);
    durable_entries.erase(from);
    return IoError::kNone;
  }
  void Close(FileHandle file) noexcept override {
    const auto it = handles_.find(file.value);
    if (it == handles_.end()) return;
    log.push_back("close " + it->second);
    locked_.erase(it->second);
    handles_.erase(it);
  }
  [[nodiscard]] std::size_t OpenHandles() const { return handles_.size(); }
  [[nodiscard]] std::size_t CountOps(std::string_view prefix) const {
    return static_cast<std::size_t>(
        std::count_if(log.begin(), log.end(),
                      [&](const std::string& entry) { return entry.rfind(prefix, 0) == 0; }));
  }

 private:
  FileHandle Register(const std::string& path) {
    handles_[next_handle_] = path;
    return FileHandle{next_handle_++};
  }
  std::map<std::intptr_t, std::string> handles_;
  std::set<std::string> locked_;
  std::intptr_t next_handle_ = 3;
};

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_HPP_
