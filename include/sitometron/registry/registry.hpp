#ifndef SITOMETRON_REGISTRY_REGISTRY_HPP_
#define SITOMETRON_REGISTRY_REGISTRY_HPP_

#include <string>
#include <variant>
#include <vector>

namespace sitometron::registry {

struct RegisteredApplication {
  std::string application_id;
  std::string version;
  std::string executable;
  std::vector<std::string> arguments;
  std::string bundle_sha256;
};

struct RegistryRefusal {
  std::string reason;
};

using VerifiedApplications = std::vector<RegisteredApplication>;
using LoadResult = std::variant<VerifiedApplications, RegistryRefusal>;

// Reads once, validates every entry, and verifies every executable before publishing the set.
// An empty verified set is success. Refusals identify the operation/field and any faulty entry.
[[nodiscard]] LoadResult LoadRegistry(const std::string& path);

}  // namespace sitometron::registry

#endif  // SITOMETRON_REGISTRY_REGISTRY_HPP_
