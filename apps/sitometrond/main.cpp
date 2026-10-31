#include <iostream>
#include <string>
#include <vector>

#include "entrypoint.hpp"

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    return sitometron::daemon::RunSitometrond(args);
  } catch (...) {
    std::cerr << "error: sitometrond terminated unexpectedly\n";
    return 1;
  }
}
