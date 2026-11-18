#include <windows.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform.hpp"
#include "windows_command_line.hpp"

namespace sitometron::process::internal {
namespace {
std::wstring Wide(std::string_view value) {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("process text exceeds Windows string limit");
  if (value.empty()) return {};
  const auto length = static_cast<int>(value.size());
  const int required =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
  if (required == 0) throw std::invalid_argument("process text is not UTF-8");
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(),
                            required) != required)
    throw std::invalid_argument("process text conversion failed");
  return result;
}
bool IdentityKey(std::wstring_view entry) {
  const auto end = entry.find(L'=');
  if (end == std::wstring_view::npos) return false;
  std::wstring key(entry.substr(0, end));
  for (auto& character : key)
    if (character >= L'a' && character <= L'z')
      character = static_cast<wchar_t>(character - L'a' + L'A');
  return key == L"SITOMETRON_JOB_ID" || key == L"SITOMETRON_WORKER_ID" ||
         key == L"SITOMETRON_LAUNCH_OPERATION_ID";
}
std::vector<wchar_t> Environment(const core::ApplicationLaunchRequest& request) {
  std::unique_ptr<wchar_t, decltype(&::FreeEnvironmentStringsW)> inherited(
      ::GetEnvironmentStringsW(), &::FreeEnvironmentStringsW);
  if (!inherited) throw std::runtime_error("cannot read process environment");
  std::vector<std::wstring> entries;
  for (const wchar_t* entry = inherited.get(); *entry != L'\0';) {
    const std::wstring_view value(entry);
    if (!IdentityKey(value)) entries.emplace_back(value);
    entry += value.size() + 1;
  }
  entries.push_back(L"SITOMETRON_JOB_ID=" + Wide(request.job_id.value));
  entries.push_back(L"SITOMETRON_WORKER_ID=" + Wide(request.intent.worker_id.value));
  entries.push_back(L"SITOMETRON_LAUNCH_OPERATION_ID=" + Wide(request.intent.operation_id.value));
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::vector<wchar_t> result;
  for (const auto& entry : entries) {
    result.insert(result.end(), entry.begin(), entry.end());
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  return result;
}
}  // namespace
struct NativeProcess {
  HANDLE handle = nullptr;
};
Process::Process() noexcept = default;
Process::~Process() {
  if (state_) {
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
  const auto executable = Wide(specification.executable);
  const auto command = Wide(WindowsCommandLine(specification.executable, specification.arguments));
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  auto environment = Environment(request);
  STARTUPINFOW startup{};
  startup.cb = static_cast<DWORD>(sizeof(startup));
  PROCESS_INFORMATION information{};
  Process result;
  // Inherit the daemon's current directory; never a shell or process-tree termination mechanism.
  if (::CreateProcessW(executable.c_str(), command_buffer.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup,
                       &information) != FALSE) {
    native->handle = information.hProcess;
    (void)::CloseHandle(information.hThread);
    result.state_ = std::move(native);
  } else {
    std::cerr << "process start failed: " << ::GetLastError() << '\n';
  }
  return result;
}
void Process::Signal(bool forced) const noexcept {
  (void)forced;  // Both request kinds use TerminateProcess in Phase 2; mode stays request-defined.
  if (state_ && ::TerminateProcess(state_->handle, 1) == FALSE)
    std::cerr << "process stop failed: " << ::GetLastError() << '\n';
}
void Process::Wait() const noexcept {
  if (state_ && ::WaitForSingleObject(state_->handle, INFINITE) != WAIT_OBJECT_0)
    std::cerr << "process exit observation failed: " << ::GetLastError() << '\n';
}
void Process::Reap() noexcept {
  if (!state_) return;
  Wait();
  if (DWORD status = 0; ::GetExitCodeProcess(state_->handle, &status) != FALSE)
    std::cerr << "process exit status: " << status << '\n';
  (void)::CloseHandle(state_->handle);
  state_.reset();
}
}  // namespace sitometron::process::internal
