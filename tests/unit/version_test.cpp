#include "sitometron/core/version.hpp"

#include "test.hpp"

int main() {
  return sitometron::test::ExpectEqual(sitometron::Version(), "0.1.0-dev", "Version()");
}
