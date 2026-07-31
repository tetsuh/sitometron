#ifndef SITOMETRON_TESTS_FIXTURES_CORE_PUBLIC_API_FORBIDDEN_TYPE_HPP_
#define SITOMETRON_TESTS_FIXTURES_CORE_PUBLIC_API_FORBIDDEN_TYPE_HPP_

namespace sitometron::fixture {
struct Leaked {
  nlohmann::json value;
};
}  // namespace sitometron::fixture

#endif  // SITOMETRON_TESTS_FIXTURES_CORE_PUBLIC_API_FORBIDDEN_TYPE_HPP_
