#include "process_child_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sitometron::test::process {
namespace {
std::wstring Native(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
}
std::wstring Quote(std::wstring_view value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
    result.push_back(character);
    slashes = 0;
  }
  result.append(2 * slashes, L'\\');
  result.push_back(L'"');
  return result;
}
void StopAndClose(HANDLE handle) noexcept {
  if (handle == nullptr) return;
  (void)::TerminateProcess(handle, 0);
  (void)::WaitForSingleObject(handle, INFINITE);
  (void)::CloseHandle(handle);
}
class Spawned final : public ChildProcess {
 public:
  Spawned(const std::string& executable, const std::string& report, const std::string& port) {
    const auto native_executable = Native(executable);
    const auto command = Quote(native_executable) + L" --mode wait --report " +
                         Quote(Native(report)) + L" --control-port " + Quote(Native(port));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(native_executable.c_str(), mutable_command.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, nullptr, &startup, &process))
      throw std::runtime_error("test descendant creation failed");
    handle_ = process.hProcess;
    (void)::CloseHandle(process.hThread);
  }
  ~Spawned() override { StopAndClose(handle_); }

 private:
  HANDLE handle_ = nullptr;
};
}  // namespace
std::unique_ptr<ChildProcess> SpawnDescendant(const std::string& executable,
                                              const std::string& report, const std::string& port) {
  return std::make_unique<Spawned>(executable, report, port);
}
std::uint64_t ProcessId() noexcept { return ::GetCurrentProcessId(); }
std::string Utf8Argument(const wchar_t* value) {
  const int required =
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) throw std::runtime_error("test child argument is not valid UTF-16");
  std::string result(static_cast<std::size_t>(required), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), required,
                            nullptr, nullptr) != required)
    throw std::runtime_error("test child argument UTF-8 conversion failed");
  result.pop_back();
  return result;
}
namespace {
class Denied final : public FileReadBlock {
 public:
  explicit Denied(const std::string& path) {
    handle_ = ::CreateFileW(Native(path).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error("unreadable executable control could not acquire sharing denial");
  }
  ~Denied() override { (void)::CloseHandle(handle_); }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};
}  // namespace
std::unique_ptr<FileReadBlock> DenyExecutableRead(const std::string& path) {
  return std::make_unique<Denied>(path);
}
struct DescendantOwnership::Impl {
  HANDLE handle = nullptr;
};
DescendantOwnership::DescendantOwnership() : impl_(std::make_unique<Impl>()) {}
DescendantOwnership::~DescendantOwnership() { StopAndClose(impl_->handle); }
void DescendantOwnership::Adopt(std::uint64_t pid) {
  if (pid == 0 || pid > std::numeric_limits<DWORD>::max())
    throw std::runtime_error("test descendant identifier invalid");
  impl_->handle = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (impl_->handle == nullptr) throw std::runtime_error("test descendant handle unavailable");
}
bool DescendantOwnership::Alive() const {
  return impl_->handle != nullptr && ::WaitForSingleObject(impl_->handle, 0) == WAIT_TIMEOUT;
}
}  // namespace sitometron::test::process
