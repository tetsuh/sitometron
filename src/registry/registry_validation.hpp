#ifndef SITOMETRON_REGISTRY_REGISTRY_VALIDATION_HPP_
#define SITOMETRON_REGISTRY_REGISTRY_VALIDATION_HPP_

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "sitometron/registry/registry.hpp"

namespace sitometron::registry::internal {

// Private stage A: parse bytes and apply schema rules, not uniqueness, native paths, or files.
// These entries are unverified. Only LoadRegistry publishes a verified set after stages B/C.
using ValidationResult = std::variant<std::vector<RegisteredApplication>, RegistryRefusal>;
[[nodiscard]] ValidationResult ValidateRegistryDocument(std::string_view bytes);

// Private stage B: the two rules assigned to owned code rather than the JSON Schema.
[[nodiscard]] std::optional<RegistryRefusal> ValidateRegistryLocations(
    const std::vector<RegisteredApplication>& applications);

}  // namespace sitometron::registry::internal

#endif  // SITOMETRON_REGISTRY_REGISTRY_VALIDATION_HPP_
