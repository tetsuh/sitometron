#ifndef SITOMETRON_SPIKE_PROCESS_RUNNER_HPP_
#define SITOMETRON_SPIKE_PROCESS_RUNNER_HPP_

#include <sys/types.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::spike {

struct LaunchSpec {
  // The registered Application this launch is for. Empty on the unversioned route, where the
  // driver derives an identifier from the executable.
  std::string application_id;
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
};

struct SpawnResult {
  pid_t pid = -1;
  std::string error;
};

struct ExitStatus {
  bool exited_normally = false;
  int exit_code = -1;
  int signal = 0;
};

// Mailbox-style runner. The single writer calls the Handoff* ports on its own thread and must
// never block there, so each request is parked per Job and picked up by that Job's driver thread.
class ProcessRunner final : public core::ApplicationRunnerPort {
 public:
  void HandoffLaunch(core::ApplicationLaunchRequest&& request) noexcept override;
  void HandoffCooperativeStop(core::ApplicationStopRequest&& request) noexcept override;
  void HandoffForcedStop(core::ApplicationStopRequest&& request) noexcept override;

  // Blocks the calling driver thread until the writer hands off the launch for this Job.
  [[nodiscard]] std::optional<core::ApplicationLaunchRequest> AwaitLaunch(const core::Uuid& job);

  [[nodiscard]] static SpawnResult Spawn(const LaunchSpec& spec);
  [[nodiscard]] static ExitStatus Wait(pid_t pid);
  // Registers the Job's child so that a cooperative stop can signal it. A stop that was handed
  // off before the child existed is delivered here.
  void Attach(const core::Uuid& job, pid_t pid);
  // Waits for the attached child to exit, forgets it, and reaps it. The child is forgotten before
  // it is reaped, so a stop never signals a process identifier that has been released.
  [[nodiscard]] ExitStatus WaitAttached(const core::Uuid& job, pid_t pid);
  static void Signal(pid_t pid, int signal);

  void Abandon();  // wake every waiter; used at shutdown

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::map<std::string, core::ApplicationLaunchRequest> launches_;
  std::map<std::string, pid_t> children_;     // attached and not yet reaped
  std::set<std::string, std::less<>> stops_;  // Jobs whose cooperative stop was handed off
  bool abandoned_ = false;
};

// Same mailbox pattern for the Session port. The spike has no Session store; it acknowledges
// retention immediately from the driver thread.
class NullSessionRetainer final : public core::SessionRetainerPort {
 public:
  void HandoffRetainSameIdentity(core::SessionRetainRequest&& request) noexcept override;
  [[nodiscard]] std::optional<core::SessionRetainRequest> AwaitRetain(const core::Uuid& job);
  void Abandon();

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::map<std::string, core::SessionRetainRequest> requests_;
  bool abandoned_ = false;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_PROCESS_RUNNER_HPP_
