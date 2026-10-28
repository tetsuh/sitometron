#ifndef SITOMETRON_SITOMETROND_ENTRYPOINT_HPP_
#define SITOMETRON_SITOMETROND_ENTRYPOINT_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "sitometron/identity/identity_source.hpp"

namespace sitometron::daemon {

using IdentityFactory = std::unique_ptr<core::IdentitySourcePort> (*)(std::size_t capacity);

// Factory injection is a C++ composition seam, never a command-line registration or entropy option.
[[nodiscard]] int RunSitometrond(const std::vector<std::string>& args,
                                 IdentityFactory factory = identity::MakeProductionIdentitySource);

}  // namespace sitometron::daemon

#endif  // SITOMETRON_SITOMETROND_ENTRYPOINT_HPP_
