#include <iostream>

#include "sitometron/core/version.hpp"

int main() {
  std::cout << "sitometrond " << sitometron::Version() << '\n';
  return 0;
}
