#ifndef SITOMETRON_CORE_VERSION_HPP_
#define SITOMETRON_CORE_VERSION_HPP_

#include <string_view>

namespace sitometron {

[[nodiscard]] std::string_view Version() noexcept;

}  // namespace sitometron

#endif  // SITOMETRON_CORE_VERSION_HPP_
