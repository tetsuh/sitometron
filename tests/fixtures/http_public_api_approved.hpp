#ifndef SITOMETRON_TESTS_FIXTURES_HTTP_PUBLIC_API_APPROVED_HPP_
#define SITOMETRON_TESTS_FIXTURES_HTTP_PUBLIC_API_APPROVED_HPP_

#include <string_view>

namespace sitometron::http {
class PublicMarker {
 public:
  std::string_view name() const;
};
}  // namespace sitometron::http

#endif  // SITOMETRON_TESTS_FIXTURES_HTTP_PUBLIC_API_APPROVED_HPP_
