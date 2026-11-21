#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "process_child_platform.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
std::string Environment(const char* name) {
  return sitometron::test::process::EnvironmentValue(name).value_or(std::string{});
}
#ifndef _WIN32
volatile std::sig_atomic_t control_descriptor = -1;
volatile std::sig_atomic_t ignore_cooperative = 0;
volatile std::sig_atomic_t requested_status = 0;
extern "C" void Cooperative(int) {
  if (ignore_cooperative != 0) {
    const char observed = 'T';
    // Async-signal-safe acknowledgement, not a Worker protocol fact.
    if (control_descriptor >= 0) (void)::write(static_cast<int>(control_descriptor), &observed, 1);
  } else {
    ::_exit(static_cast<int>(requested_status));
  }
}
#endif
}  // namespace

namespace {
int RunChild(const std::vector<std::string>& arguments) {
  try {
    std::string report = Environment("ISSUE141_TEST_REPORT");
    std::string port = Environment("ISSUE141_TEST_CONTROL_PORT");
    std::string mode = Environment("ISSUE141_TEST_MODE");
    int status = 0;
    if (const auto value = Environment("ISSUE141_TEST_STATUS"); !value.empty())
      status = std::stoi(value);
    for (std::size_t index = 1; index < arguments.size(); ++index) {
      const auto& argument = arguments[index];
      if (argument == "--report" && index + 1 < arguments.size())
        report = arguments[++index];
      else if (argument == "--control-port" && index + 1 < arguments.size())
        port = arguments[++index];
      else if (argument == "--mode" && index + 1 < arguments.size())
        mode = arguments[++index];
      else if (argument == "--status" && index + 1 < arguments.size())
        status = std::stoi(arguments[++index]);
    }
    if (report.empty()) throw std::runtime_error("test report path missing");
    const auto cwd = std::filesystem::current_path().u8string();
    nlohmann::json observation{{"process_id", sitometron::test::process::ProcessId()},
                               {"parent_process_id", sitometron::test::process::ParentProcessId()},
                               {"fixture_nonce", Environment("ISSUE141_TEST_NONCE")},
#ifndef _WIN32
                               {"sigterm_blocked", sitometron::test::process::TermBlocked()},
                               {"sigterm_ignored", sitometron::test::process::TermIgnored()},
#endif
                               {"argv", arguments},
                               {"cwd", std::string(cwd.begin(), cwd.end())},
                               {"sentinel", Environment("ISSUE141_TEST_SENTINEL")},
                               {"job_id", Environment("SITOMETRON_JOB_ID")},
                               {"worker_id", Environment("SITOMETRON_WORKER_ID")},
                               {"operation_id", Environment("SITOMETRON_LAUNCH_OPERATION_ID")}};
#ifndef _WIN32
    if (mode == "descriptor-report")
      observation["open_descriptors"] = sitometron::test::process::OpenDescriptors();
#endif
    {
      const std::u8string native(report.begin(), report.end());
      std::ofstream output(std::filesystem::path(native), std::ios::binary);
      output << observation.dump() << '\n';
      output.close();
      if (!output) throw std::runtime_error("test report write failed");
    }
    if (mode == "exit" && port.empty()) return status;
    if (port.empty()) throw std::runtime_error("test control port missing");
    boost::asio::io_context context;
    boost::asio::ip::tcp::socket control(context);
    const auto parsed_port = std::stoul(port);
    if (parsed_port == 0 || parsed_port > 65535) throw std::runtime_error("invalid test port");
    control.connect(
        {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(parsed_port)});
#ifndef _WIN32
    control_descriptor = control.native_handle();
    ignore_cooperative = mode == "ignore-term" ? 1 : 0;
    requested_status = status;
    if (mode != "default-term" && std::signal(SIGTERM, Cooperative) == SIG_ERR)
      throw std::runtime_error("test signal registration failed");
#endif
    const char ready = 'R';
    boost::asio::write(control, boost::asio::buffer(&ready, 1));
    const auto process_id = sitometron::test::process::ProcessId();
    boost::asio::write(control, boost::asio::buffer(&process_id, sizeof(process_id)));
    if (mode == "exit") return status;
    std::unique_ptr<sitometron::test::process::ChildProcess> descendant;
    if (mode == "descendant-parent")
      descendant =
          sitometron::test::process::SpawnDescendant(arguments.at(0), report + ".desc", port);
    for (;;) {
      char command = 0;
      boost::system::error_code error;
      boost::asio::read(control, boost::asio::buffer(&command, 1), error);
      if (error || command == 'X') break;
      if (command == 'P') boost::asio::write(control, boost::asio::buffer(&command, 1));
    }
    return status;
  } catch (const std::exception& error) {
    std::cerr << "process test child: " << error.what() << '\n';
    return 125;
  }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
#ifdef _WIN32
      arguments.push_back(sitometron::test::process::Utf8Argument(argv[index]));
#else
      arguments.emplace_back(argv[index]);
#endif
    }
    return RunChild(arguments);
  } catch (const std::exception& error) {
    std::cerr << "process test child arguments: " << error.what() << '\n';
    return 125;
  }
}
