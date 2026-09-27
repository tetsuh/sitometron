#include "sitometron/journal/file_system.hpp"

#include <stdexcept>

namespace sitometron::journal {

FileSystem& SystemFileSystem() { throw std::logic_error("not implemented"); }
std::string JoinPath(std::string_view directory, std::string_view name) {
  return std::string(directory) + "/" + std::string(name);
}

}  // namespace sitometron::journal
