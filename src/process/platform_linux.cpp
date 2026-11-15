#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform.hpp"

extern char** environ;

namespace sitometron::process::internal {
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
  // No shell, path search, working-directory action, or digest verification.
  const int error = ::posix_spawn(&native->pid, specification.executable.c_str(), nullptr, nullptr,
                                  argv.data(), envp.data());
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
