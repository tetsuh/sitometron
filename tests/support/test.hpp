#ifndef SITOMETRON_TESTS_SUPPORT_TEST_HPP_
#define SITOMETRON_TESTS_SUPPORT_TEST_HPP_

#include <iostream>
#include <string_view>

namespace sitometron::test {

inline int ExpectEqual(std::string_view actual, std::string_view expected,
                       std::string_view expression) {
  if (actual == expected) {
    return 0;
  }
  std::cerr << expression << ": expected '" << expected << "', got '" << actual << "'\n";
  return 1;
}

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_TEST_HPP_
