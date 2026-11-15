#ifndef SITOMETRON_TEST_PROCESS_CHILD_PLATFORM_HPP_
#define SITOMETRON_TEST_PROCESS_CHILD_PLATFORM_HPP_

#include <cstdint>
#include <memory>
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
#ifdef _WIN32
std::string Utf8Argument(const wchar_t*);
#endif
class FileReadBlock {
 public:
  virtual ~FileReadBlock() = default;
};
std::unique_ptr<FileReadBlock> DenyExecutableRead(const std::string& path);
// Test-only descendant ownership. On Linux adopt through a scoped subreaper;
// on Windows retain a waitable process handle before its parent can exit.
class DescendantOwnership final {
 public:
  DescendantOwnership();
  ~DescendantOwnership();
  DescendantOwnership(const DescendantOwnership&) = delete;
  DescendantOwnership& operator=(const DescendantOwnership&) = delete;
  void Adopt(std::uint64_t);
  bool Alive() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace sitometron::test::process
#endif
