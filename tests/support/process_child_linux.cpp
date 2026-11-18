#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

#include "process_child_platform.hpp"

extern char** environ;

namespace sitometron::test::process {
namespace {
void StopAndReap(pid_t pid) noexcept {
  if (pid <= 0) return;
  (void)::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}
class Spawned final : public ChildProcess {
 public:
  Spawned(const std::string& executable, const std::string& report, const std::string& port) {
    std::vector<std::string> arguments{executable, "--mode",         "wait", "--report",
                                       report,     "--control-port", port};
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    if (::posix_spawn(&pid_, executable.c_str(), nullptr, nullptr, pointers.data(), environ) != 0)
      throw std::runtime_error("test descendant creation failed");
  }
  ~Spawned() override { StopAndReap(pid_); }

 private:
  pid_t pid_ = -1;
};
}  // namespace
std::unique_ptr<ChildProcess> SpawnDescendant(const std::string& executable,
                                              const std::string& report, const std::string& port) {
  return std::make_unique<Spawned>(executable, report, port);
}
std::uint64_t ProcessId() noexcept { return static_cast<std::uint64_t>(::getpid()); }
namespace {
class Denied final : public FileReadBlock {
 public:
  explicit Denied(const std::string& path) : path_(path) {
    prior_ = std::filesystem::status(path_).permissions();
    std::filesystem::permissions(path_, std::filesystem::perms::none);
  }
  ~Denied() override {
    std::error_code ignored;
    std::filesystem::permissions(path_, prior_, ignored);
  }

 private:
  std::filesystem::path path_;
  std::filesystem::perms prior_{};
};
}  // namespace
std::unique_ptr<FileReadBlock> DenyExecutableRead(const std::string& path) {
  return std::make_unique<Denied>(path);
}
struct DescendantOwnership::Impl {
  int prior = 0;
  pid_t pid = -1;
};
DescendantOwnership::DescendantOwnership() : impl_(std::make_unique<Impl>()) {
  if (::prctl(PR_GET_CHILD_SUBREAPER, &impl_->prior) != 0 ||
      ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
    throw std::runtime_error("test descendant adoption unavailable");
}
DescendantOwnership::~DescendantOwnership() {
  StopAndReap(impl_->pid);
  (void)::prctl(PR_SET_CHILD_SUBREAPER, impl_->prior);
}
void DescendantOwnership::Adopt(std::uint64_t pid) {
  if (pid == 0 || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
    throw std::runtime_error("test descendant identifier invalid");
  impl_->pid = static_cast<pid_t>(pid);
}
bool DescendantOwnership::Alive() const {
  if (impl_->pid <= 0) return false;
  siginfo_t info{};
  const auto result =
      ::waitid(P_PID, static_cast<id_t>(impl_->pid), &info, WEXITED | WNOHANG | WNOWAIT);
  return result == 0 && info.si_pid == 0 && ::kill(impl_->pid, 0) == 0;
}
}  // namespace sitometron::test::process
