#include "process_runner.hpp"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

extern char** environ;

namespace sitometron::spike {

void ProcessRunner::HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept {
  try {
    std::lock_guard lock(mutex_);
    launches_.insert_or_assign(request.job_id.value, std::move(request));
    ready_.notify_all();
  } catch (...) {
    // A lost handoff surfaces as the driver thread never seeing a launch; the spike reports it.
  }
}

void ProcessRunner::HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept {
  try {
    std::lock_guard lock(mutex_);
    stops_.insert(request.job_id.value);
    if (const auto child = children_.find(request.job_id.value); child != children_.end())
      Signal(child->second, SIGTERM);
  } catch (...) {
    // A lost stop leaves the Job stopping until its child exits by itself.
  }
}

// The skeleton has no timer adapter, so the cooperative-stop timeout that requests a forced stop
// never fires (Issue #84).
void ProcessRunner::HandoffForcedStop(core::ApplicationStopRequest&& /*request*/) noexcept {}

void ProcessRunner::Attach(const core::Uuid& job, pid_t pid) {
  std::lock_guard lock(mutex_);
  children_.insert_or_assign(job.value, pid);
  if (stops_.contains(job.value)) Signal(pid, SIGTERM);
}

ExitStatus ProcessRunner::WaitAttached(const core::Uuid& job, pid_t pid) {
  // Wait without reaping: the process identifier stays reserved while a stop may still signal it.
  siginfo_t info{};
  while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR) {
  }
  {
    std::lock_guard lock(mutex_);
    children_.erase(job.value);
  }
  return Wait(pid);
}

std::optional<core::ApplicationLaunchRequest> ProcessRunner::AwaitLaunch(const core::Uuid& job) {
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [&] { return abandoned_ || launches_.contains(job.value); });
  const auto found = launches_.find(job.value);
  if (found == launches_.end()) return std::nullopt;
  auto request = std::move(found->second);
  launches_.erase(found);
  return request;
}

void ProcessRunner::Abandon() {
  std::lock_guard lock(mutex_);
  abandoned_ = true;
  ready_.notify_all();
}

SpawnResult ProcessRunner::Spawn(const LaunchSpec& spec) {
  std::vector<std::string> owned;
  owned.reserve(spec.arguments.size() + 1);
  owned.push_back(spec.executable);
  owned.insert(owned.end(), spec.arguments.begin(), spec.arguments.end());
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (auto& item : owned) argv.push_back(item.data());
  argv.push_back(nullptr);

  SpawnResult result;
  posix_spawn_file_actions_t actions;
  if (const int status = posix_spawn_file_actions_init(&actions); status != 0) {
    result.error = std::string("posix_spawn_file_actions_init: ") + std::strerror(status);
    return result;
  }
  if (!spec.working_directory.empty()) {
    if (const int status =
            posix_spawn_file_actions_addchdir_np(&actions, spec.working_directory.c_str());
        status != 0) {
      posix_spawn_file_actions_destroy(&actions);
      result.error = std::string("posix_spawn_file_actions_addchdir_np: ") + std::strerror(status);
      return result;
    }
  }
  const int status =
      posix_spawnp(&result.pid, spec.executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (status != 0) {
    result.pid = -1;
    result.error = std::string("posix_spawnp: ") + std::strerror(status);
  }
  return result;
}

ExitStatus ProcessRunner::Wait(pid_t pid) {
  ExitStatus status;
  int raw = 0;
  for (;;) {
    if (::waitpid(pid, &raw, 0) == pid) break;
    if (errno == EINTR) continue;
    return status;
  }
  if (WIFEXITED(raw)) {
    status.exited_normally = true;
    status.exit_code = WEXITSTATUS(raw);
  } else if (WIFSIGNALED(raw)) {
    status.signal = WTERMSIG(raw);
  }
  return status;
}

void ProcessRunner::Signal(pid_t pid, int signal) {
  if (pid > 0) ::kill(pid, signal);
}

void NullSessionRetainer::HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept {
  try {
    std::lock_guard lock(mutex_);
    requests_.insert_or_assign(request.job_id.value, std::move(request));
    ready_.notify_all();
  } catch (...) {
  }
}

std::optional<core::SessionRetainRequest> NullSessionRetainer::AwaitRetain(const core::Uuid& job) {
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [&] { return abandoned_ || requests_.contains(job.value); });
  const auto found = requests_.find(job.value);
  if (found == requests_.end()) return std::nullopt;
  auto request = found->second;
  requests_.erase(found);
  return request;
}

void NullSessionRetainer::Abandon() {
  std::lock_guard lock(mutex_);
  abandoned_ = true;
  ready_.notify_all();
}

}  // namespace sitometron::spike
