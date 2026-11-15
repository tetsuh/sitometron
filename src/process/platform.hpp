#ifndef SITOMETRON_PROCESS_PLATFORM_HPP_
#define SITOMETRON_PROCESS_PLATFORM_HPP_
#include <memory>

#include "sitometron/process/runner.hpp"
namespace sitometron::process::internal {
struct NativeProcess;
// Only the slot owns/reaps this object. Service may signal it under its mailbox mutex until gone.
class Process final {
 public:
  Process() noexcept;
  ~Process();
  Process(Process&&) noexcept;
  Process& operator=(Process&&) noexcept;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  static Process Spawn(const LaunchSpecification&, const core::ApplicationLaunchRequest&);
  explicit operator bool() const noexcept;
  void Signal(bool forced) const noexcept;
  void Wait() const noexcept;
  void Reap() noexcept;

 private:
  std::unique_ptr<NativeProcess> state_;
};
}  // namespace sitometron::process::internal
#endif  // SITOMETRON_PROCESS_PLATFORM_HPP_
