#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
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
std::uint64_t ParentProcessId() { return static_cast<std::uint64_t>(::getppid()); }
namespace {
class InheritedChild final : public SignalInheritance {
 public:
  explicit InheritedChild(ChildDisposition disposition) {
    if (::sigaction(SIGCHLD, nullptr, &prior_) != 0)
      throw std::runtime_error("cannot save SIGCHLD disposition");
    struct sigaction action {};
    action.sa_handler = disposition == ChildDisposition::kIgnored ? SIG_IGN : SIG_DFL;
    if (disposition == ChildDisposition::kNoChildWait) action.sa_flags = SA_NOCLDWAIT;
    (void)::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGCHLD, &action, nullptr) != 0)
      throw std::runtime_error("cannot set test SIGCHLD disposition");
  }
  ~InheritedChild() override { (void)::sigaction(SIGCHLD, &prior_, nullptr); }

 private:
  struct sigaction prior_ {};
};
class InheritedTerm final : public SignalInheritance {
 public:
  InheritedTerm() {
    if (::sigaction(SIGTERM, nullptr, &prior_action_) != 0 ||
        ::pthread_sigmask(SIG_SETMASK, nullptr, &prior_mask_) != 0)
      throw std::runtime_error("cannot save inherited signal state");
    sigset_t term;
    (void)::sigemptyset(&term);
    (void)::sigaddset(&term, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &term, nullptr) != 0)
      throw std::runtime_error("cannot block inherited SIGTERM");
    struct sigaction ignored {};
    ignored.sa_handler = SIG_IGN;
    (void)::sigemptyset(&ignored.sa_mask);
    if (::sigaction(SIGTERM, &ignored, nullptr) != 0) {
      (void)::pthread_sigmask(SIG_SETMASK, &prior_mask_, nullptr);
      throw std::runtime_error("cannot ignore inherited SIGTERM");
    }
  }
  ~InheritedTerm() override {
    (void)::sigaction(SIGTERM, &prior_action_, nullptr);
    (void)::pthread_sigmask(SIG_SETMASK, &prior_mask_, nullptr);
  }

 private:
  struct sigaction prior_action_ {};
  sigset_t prior_mask_{};
};
}  // namespace
std::unique_ptr<SignalInheritance> BlockAndIgnoreTerm() {
  return std::make_unique<InheritedTerm>();
}
std::unique_ptr<SignalInheritance> SetChildDisposition(ChildDisposition disposition) {
  return std::make_unique<InheritedChild>(disposition);
}
bool TermBlocked() {
  sigset_t mask;
  if (::pthread_sigmask(SIG_SETMASK, nullptr, &mask) != 0)
    throw std::runtime_error("cannot read test signal mask");
  return ::sigismember(&mask, SIGTERM) == 1;
}
bool TermIgnored() {
  struct sigaction action {};
  if (::sigaction(SIGTERM, nullptr, &action) != 0)
    throw std::runtime_error("cannot read test signal disposition");
  return action.sa_handler == SIG_IGN;
}
namespace {
int InheritableDescriptor(int descriptor) {
  if (descriptor < 0) throw std::runtime_error("cannot open descriptor inheritance fixture");
  const int duplicated = ::fcntl(descriptor, F_DUPFD, 3);
  (void)::close(descriptor);
  if (duplicated < 3) throw std::runtime_error("cannot duplicate inheritable test descriptor");
  return duplicated;
}
class InheritedDescriptors final : public DescriptorInheritance {
 public:
  InheritedDescriptors() {
    try {
      file_ = InheritableDescriptor(::open("/dev/null", O_RDONLY));
      listener_ = InheritableDescriptor(::socket(AF_INET, SOCK_STREAM, 0));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                 static_cast<socklen_t>(sizeof(address))) != 0 ||
          ::listen(listener_, 1) != 0)
        throw std::runtime_error("cannot bind ephemeral descriptor-inheritance listener");
      for (const int descriptor : {file_, listener_}) {
        const int flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) != 0)
          throw std::runtime_error("descriptor inheritance fixture is not inheritable");
      }
    } catch (...) {
      Close();
      throw;
    }
  }
  ~InheritedDescriptors() override { Close(); }

 private:
  void Close() noexcept {
    if (file_ >= 0) (void)::close(file_);
    if (listener_ >= 0) (void)::close(listener_);
  }
  int file_ = -1;
  int listener_ = -1;
};
}  // namespace
std::unique_ptr<DescriptorInheritance> OpenInheritableDescriptors() {
  return std::make_unique<InheritedDescriptors>();
}
std::vector<int> OpenDescriptors() {
  std::vector<int> descriptors;
  const auto enumeration = "/proc/" + std::to_string(ProcessId()) + "/fd";
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    // The iterator opened this directory itself; do not mistake it for an inherited FD.
    if (std::filesystem::read_symlink(entry.path()) == enumeration) continue;
    descriptors.push_back(std::stoi(entry.path().filename().string()));
  }
  return descriptors;
}
void ReapExitedChild(std::uint64_t pid) {
  if (pid == 0 || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
    throw std::runtime_error("invalid owned child identifier for external test reap");
  const auto native = static_cast<pid_t>(pid);
  int status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(native, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result != native) throw std::runtime_error("cannot externally reap owned exited child");
}
std::optional<std::string> EnvironmentValue(const char* name) {
  if (const char* value = std::getenv(name)) return std::string(value);
  return std::nullopt;
}
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
void DescendantOwnership::Adopt(VerifiedDescendant verified) {
  const auto pid = verified.pid_;
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
