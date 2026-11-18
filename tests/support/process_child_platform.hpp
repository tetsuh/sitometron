#ifndef SITOMETRON_TEST_PROCESS_CHILD_PLATFORM_HPP_
#define SITOMETRON_TEST_PROCESS_CHILD_PLATFORM_HPP_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sitometron::test::process {
class ChildProcess {
 public:
  virtual ~ChildProcess() = default;
};
std::unique_ptr<ChildProcess> SpawnDescendant(const std::string& executable,
                                              const std::string& report,
                                              const std::string& control_port);
std::uint64_t ProcessId() noexcept;
std::uint64_t ParentProcessId();
#ifndef _WIN32
class SignalInheritance {
 public:
  virtual ~SignalInheritance() = default;
};
std::unique_ptr<SignalInheritance> BlockAndIgnoreTerm();
bool TermBlocked();
bool TermIgnored();
#endif
std::optional<std::string> EnvironmentValue(const char* name);
#ifdef _WIN32
std::string Utf8Argument(const wchar_t*);
#endif
class FileReadBlock {
 public:
  virtual ~FileReadBlock() = default;
};
std::unique_ptr<FileReadBlock> DenyExecutableRead(const std::string& path);
class VerifiedDescendant final {
 private:
  explicit VerifiedDescendant(std::uint64_t pid) : pid_(pid) {}
  std::uint64_t pid_;
  friend class Control;
  friend class DescendantOwnership;
};
// Test-only descendant ownership. On Linux adopt through a scoped subreaper;
// on Windows retain a waitable process handle before its parent can exit.
class DescendantOwnership final {
 public:
  DescendantOwnership();
  ~DescendantOwnership();
  DescendantOwnership(const DescendantOwnership&) = delete;
  DescendantOwnership& operator=(const DescendantOwnership&) = delete;
  void Adopt(VerifiedDescendant);
  bool Alive() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace sitometron::test::process
#endif
