#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform.hpp"

extern char** environ;

namespace sitometron::process::internal {
namespace {
class SpawnAttributes final {
 public:
  SpawnAttributes() {
    if (::posix_spawnattr_init(&value_) != 0)
      throw std::runtime_error("cannot initialize process spawn attributes");
  }
  ~SpawnAttributes() { (void)::posix_spawnattr_destroy(&value_); }
  SpawnAttributes(const SpawnAttributes&) = delete;
  SpawnAttributes& operator=(const SpawnAttributes&) = delete;
  void ConfigureSignals() {
    sigset_t mask;
    sigset_t defaults;
    if (::sigemptyset(&mask) != 0 || ::sigemptyset(&defaults) != 0)
      throw std::runtime_error("cannot initialize child signal sets");
    // A handler-less executable must not inherit daemon-specific ignore policy for
    // termination, pipeline, resource-limit or job-control signals. The producer
    // validates the daemon's SIGCHLD policy without altering that disposition.
    constexpr std::array signals{SIGTERM, SIGINT,  SIGHUP,  SIGQUIT, SIGPIPE, SIGALRM, SIGUSR1,
                                 SIGUSR2, SIGXCPU, SIGXFSZ, SIGTSTP, SIGTTIN, SIGTTOU};
    for (const int signal : signals)
      if (::sigaddset(&defaults, signal) != 0)
        throw std::runtime_error("cannot configure child default signals");
    if (::posix_spawnattr_setsigmask(&value_, &mask) != 0 ||
        ::posix_spawnattr_setsigdefault(&value_, &defaults) != 0 ||
        ::posix_spawnattr_setflags(
            &value_, static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF)) != 0)
      throw std::runtime_error("cannot configure child signal attributes");
  }
  const posix_spawnattr_t* Get() const noexcept { return &value_; }

 private:
  posix_spawnattr_t value_{};
};
}  // namespace
void CheckChildWaitPolicy() {
  struct sigaction action {};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0)
    throw std::runtime_error("cannot query SIGCHLD disposition");
  if (action.sa_handler == SIG_IGN || (action.sa_flags & SA_NOCLDWAIT) != 0)
    throw std::runtime_error(
        "runner requires waitable children: SIGCHLD must not be ignored or use SA_NOCLDWAIT");
}
struct NativeProcess {
  pid_t pid = -1;
};
Process::Process() noexcept = default;
Process::~Process() {
  if (state_) {
    // Startup/unwind guard; normal supervision explicitly marks gone before reaping.
    Signal(true);
    Reap();
  }
}
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    if (state_) {
      Signal(true);
      Reap();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}
Process::operator bool() const noexcept { return state_ != nullptr; }
Process Process::Spawn(const LaunchSpecification& specification,
                       const core::ApplicationLaunchRequest& request) {
  auto native = std::make_unique<NativeProcess>();
  std::vector<std::string> arguments{specification.executable};
  arguments.insert(arguments.end(), specification.arguments.begin(), specification.arguments.end());
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment;
  for (auto** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view value(*entry);
    if (!value.starts_with("SITOMETRON_JOB_ID=") && !value.starts_with("SITOMETRON_WORKER_ID=") &&
        !value.starts_with("SITOMETRON_LAUNCH_OPERATION_ID="))
      environment.emplace_back(value);
  }
  environment.push_back("SITOMETRON_JOB_ID=" + request.job_id.value);
  environment.push_back("SITOMETRON_WORKER_ID=" + request.intent.worker_id.value);
  environment.push_back("SITOMETRON_LAUNCH_OPERATION_ID=" + request.intent.operation_id.value);
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto& entry : environment) envp.push_back(entry.data());
  envp.push_back(nullptr);
  SpawnAttributes attributes;
  attributes.ConfigureSignals();
  // No shell, path search, working-directory action, or digest verification.
  const int error = ::posix_spawn(&native->pid, specification.executable.c_str(), nullptr,
                                  attributes.Get(), argv.data(), envp.data());
  Process result;
  if (error == 0)
    result.state_ = std::move(native);
  else
    std::cerr << "process start failed: " << error << '\n';
  return result;
}
void Process::Signal(bool forced) const noexcept {
  if (state_ && ::kill(state_->pid, forced ? SIGKILL : SIGTERM) != 0 && errno != ESRCH)
    std::cerr << "process stop failed: " << errno << '\n';
}
void Process::Wait() const noexcept {
  if (!state_) return;
  // Construction refuses SIG_IGN/SA_NOCLDWAIT, preserving the WNOWAIT ownership
  // premise unless another component changes SIGCHLD policy after startup. Such
  // a change can let the kernel auto-reap a child and reuse its PID before gone,
  // making a later stop unsafe. ECHILD still logs and returns here; it is not
  // proof of an unreaped exit. Reap also logs ECHILD and releases state below.
  siginfo_t information{};
  int result = 0;
  do {
    result = ::waitid(P_PID, static_cast<id_t>(state_->pid), &information, WEXITED | WNOWAIT);
  } while (result != 0 && errno == EINTR);
  if (result != 0) std::cerr << "process exit observation failed: " << errno << '\n';
}
void Process::Reap() noexcept {
  if (!state_) return;
  int status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(state_->pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result == -1)
    std::cerr << "process reap failed: " << errno << '\n';
  else
    std::cerr << "process exit status: " << status << '\n';
  state_.reset();
}
}  // namespace sitometron::process::internal
