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

void ProcessRunner::HandoffCooperativeStop(core::ApplicationStopRequest&& /*request*/) noexcept {
  // cancel/terminate are out of scope for the skeleton (Issue #48).
}

void ProcessRunner::HandoffForcedStop(core::ApplicationStopRequest&& /*request*/) noexcept {}

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

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!spec.working_directory.empty())
    posix_spawn_file_actions_addchdir_np(&actions, spec.working_directory.c_str());

  SpawnResult result;
  const int status =
      posix_spawnp(&result.pid, spec.executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (status != 0) {
    result.pid = -1;
    result.error = std::strerror(status);
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
