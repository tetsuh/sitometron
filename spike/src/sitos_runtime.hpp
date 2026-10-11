#ifndef SITOMETRON_SPIKE_SITOS_RUNTIME_HPP_
#define SITOMETRON_SPIKE_SITOS_RUNTIME_HPP_

#include <memory>
#include <string>

namespace sitometron::spike {

// Optional Issue #98 host for one in-process Sitos StorageNode and one Session per Job. The
// disabled build is deliberately a no-op so the existing walking skeleton remains unchanged.
class SitosRuntime final {
 public:
  explicit SitosRuntime(std::string prefix);
  ~SitosRuntime();
  SitosRuntime(const SitosRuntime&) = delete;
  SitosRuntime& operator=(const SitosRuntime&) = delete;

  [[nodiscard]] bool Enabled() const noexcept { return enabled_; }
  [[nodiscard]] bool Ready() const noexcept { return ready_; }
  [[nodiscard]] const std::string& Prefix() const noexcept { return prefix_; }
  [[nodiscard]] const std::string& Config() const noexcept { return config_; }
  [[nodiscard]] const std::string& Error() const noexcept { return error_; }

  [[nodiscard]] bool CreateSession(const std::string& session_id, std::string& error);
  [[nodiscard]] bool CloseSession(const std::string& session_id, std::string& error);
  void CloseAll();

 private:
  std::string prefix_;
  std::string config_;
  bool enabled_ = false;
  bool ready_ = true;
  std::string error_;

#ifdef SITOMETRON_SPIKE_SITOS
  // Declaration order is intentional: the client store is destroyed before the node, then the
  // engine and transport it depends on.
  struct Impl;
  std::unique_ptr<Impl> impl_;
#endif
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_SITOS_RUNTIME_HPP_
