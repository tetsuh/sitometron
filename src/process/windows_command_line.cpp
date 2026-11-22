#include "windows_command_line.hpp"

#include <cstddef>

namespace sitometron::process::internal {
std::string QuoteWindowsArgument(std::string_view value) {
  if (!value.empty() && value.find_first_of(" \t\"") == std::string_view::npos)
    return std::string(value);
  std::string result = "\"";
  std::size_t slashes = 0;
  for (const char character : value) {
    if (character == '\\') {
      ++slashes;
      continue;
    }
    result.append(character == '"' ? 2 * slashes + 1 : slashes, '\\');
    result.push_back(character);
    slashes = 0;
  }
  result.append(2 * slashes, '\\');
  result.push_back('"');
  return result;
}
std::string WindowsCommandLine(std::string_view executable,
                               const std::vector<std::string>& arguments) {
  auto result = QuoteWindowsArgument(executable);
  for (const auto& argument : arguments) {
    result.push_back(' ');
    result += QuoteWindowsArgument(argument);
  }
  return result;
}
}  // namespace sitometron::process::internal
