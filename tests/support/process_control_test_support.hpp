#ifndef SITOMETRON_TEST_PROCESS_CONTROL_TEST_SUPPORT_HPP_
#define SITOMETRON_TEST_PROCESS_CONTROL_TEST_SUPPORT_HPP_

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
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
  Environment(std::string name, std::string value) : name_(std::move(name)) {
    if (const char* inherited = std::getenv(name_.c_str())) previous_ = inherited;
    Set(value.c_str());
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
  Control() : acceptor_(context_, {boost::asio::ip::make_address("127.0.0.1"), 0}) {
    peers_.reserve(8);
    process_ids_.reserve(8);
  }
  ~Control() {
    for (auto& peer : peers_) {
      boost::system::error_code ignored;
      peer->close(ignored);
    }
  }
  Control(const Control&) = delete;
  Control& operator=(const Control&) = delete;
  std::string port() const { return std::to_string(acceptor_.local_endpoint().port()); }
  std::size_t Ready() {
    auto peer = std::make_unique<boost::asio::ip::tcp::socket>(context_);
    acceptor_.accept(*peer);
    char ready = 0;
    boost::asio::read(*peer, boost::asio::buffer(&ready, 1));
    std::uint64_t process_id = 0;
    boost::asio::read(*peer, boost::asio::buffer(&process_id, sizeof(process_id)));
    Require(ready == 'R' && process_id != 0,
            "child did not acknowledge initialized report/stop handler and process identity");
    process_ids_.push_back(process_id);
    peers_.push_back(std::move(peer));
    return peers_.size() - 1;
  }
  std::uint64_t ProcessId(std::size_t index) const { return process_ids_.at(index); }
  void Exit(std::size_t index = 0) {
    const char command = 'X';
    boost::asio::write(*peers_.at(index), boost::asio::buffer(&command, 1));
  }
  void TermObserved(std::size_t index = 0) {
    char observed = 0;
    boost::asio::read(*peers_.at(index), boost::asio::buffer(&observed, 1));
    Require(observed == 'T', "Linux cooperative stop did not reach initialized child");
  }
  void ExitAll() noexcept {
    for (auto& peer : peers_) {
      const char command = 'X';
      boost::system::error_code ignored;
      boost::asio::write(*peer, boost::asio::buffer(&command, 1), ignored);
    }
  }

 private:
  boost::asio::io_context context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::vector<std::unique_ptr<boost::asio::ip::tcp::socket>> peers_;
  std::vector<std::uint64_t> process_ids_;
};
inline nlohmann::json ReadReport(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "child report file missing after its ready acknowledgement");
  return nlohmann::json::parse(input);
}
}  // namespace sitometron::test::process
#endif
