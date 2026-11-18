#ifndef SITOMETRON_TEST_PROCESS_CONTROL_TEST_SUPPORT_HPP_
#define SITOMETRON_TEST_PROCESS_CONTROL_TEST_SUPPORT_HPP_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "process_child_platform.hpp"

namespace sitometron::test::process {
inline void Require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
inline std::string PathText(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
class Directory final {
 public:
  Directory() {
    static std::atomic_uint64_t serial{0};
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("issue141-process-" + std::to_string(serial.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) return;
    }
    throw std::runtime_error("owned test directory unavailable");
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
class Environment final {
 public:
  Environment(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
    previous_ = EnvironmentValue(name_.c_str());
    Set(value ? value->c_str() : nullptr);
  }
  ~Environment() { Set(previous_ ? previous_->c_str() : nullptr); }
  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;

 private:
  void Set(const char* value) noexcept {
#ifdef _WIN32
    (void)::_putenv_s(name_.c_str(), value == nullptr ? "" : value);
#else
    if (value == nullptr)
      (void)::unsetenv(name_.c_str());
    else
      (void)::setenv(name_.c_str(), value, 1);
#endif
  }
  std::string name_;
  std::optional<std::string> previous_;
};
class Control final {
 public:
  Control();
  ~Control();
  Control(const Control&) = delete;
  Control& operator=(const Control&) = delete;
  std::string port() const;
  std::string nonce() const;
  VerifiedDescendant VerifyDescendant(std::size_t index, std::size_t parent,
                                      const std::filesystem::path& report) const;
  std::size_t Ready();
  std::uint64_t ProcessId(std::size_t index) const;
  void Ping(std::size_t index = 0);
  void Exit(std::size_t index = 0);
  void TermObserved(std::size_t index = 0);
  void ExitAll() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
inline nlohmann::json ReadReport(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "child report file missing after its ready acknowledgement");
  return nlohmann::json::parse(input);
}
}  // namespace sitometron::test::process
#endif
