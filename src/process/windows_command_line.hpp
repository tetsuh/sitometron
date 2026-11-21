#ifndef SITOMETRON_PROCESS_WINDOWS_COMMAND_LINE_HPP_
#define SITOMETRON_PROCESS_WINDOWS_COMMAND_LINE_HPP_
#include <string>
#include <string_view>
#include <vector>
namespace sitometron::process::internal {
std::string QuoteWindowsArgument(std::string_view value);
std::string WindowsCommandLine(std::string_view executable,
                               const std::vector<std::string>& arguments);
}  // namespace sitometron::process::internal
#endif  // SITOMETRON_PROCESS_WINDOWS_COMMAND_LINE_HPP_
