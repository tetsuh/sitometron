#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#if __has_include("registry_validation.hpp")
#include "registry_validation.hpp"
#define SITOMETRON_REGISTRY_STAGE_A_AVAILABLE 1
#endif

int main() {
  constexpr auto k_name = "registry_schema_vectors";
  try {
    std::ifstream input(std::string(SITOMETRON_TEST_SOURCE_DIR) +
                        "/schemas/application-registry/v1/application-registry-vectors.json");
    if (!input) {
      std::cerr << k_name << ": shared vector file is missing\n";
      return 1;
    }
    const auto vectors = nlohmann::json::parse(input);
    if (vectors.at("format") != "application-registry-vectors.v1" ||
        vectors.at("schema_vectors").empty()) {
      std::cerr << k_name << ": shared vector format or coverage is missing\n";
      return 1;
    }
#ifndef SITOMETRON_REGISTRY_STAGE_A_AVAILABLE
    // Intentional RED: a missing implementation is an assertion, not a compile error or skip.
    std::cerr << k_name << ": registry stage A is not implemented\n";
    return 1;
#else
    std::size_t failures = 0;
    for (const auto& vector : vectors.at("schema_vectors")) {
      const auto& instance = vector.at("instance");
      const auto result = sitometron::registry::internal::ValidateRegistryDocument(instance.dump());
      const bool accepted =
          std::holds_alternative<std::vector<sitometron::registry::RegisteredApplication> >(result);
      const bool expected = vector.at("valid").get<bool>();
      if (accepted != expected) {
        std::cerr << k_name << " [" << vector.at("id").get<std::string>()
                  << "]: expected stage-A acceptance=" << expected << ", got " << accepted << '\n';
        ++failures;
        continue;
      }
      if (!accepted) {
        if (std::get<sitometron::registry::RegistryRefusal>(result).reason.empty()) ++failures;
        continue;
      }
      const auto& applications =
          std::get<std::vector<sitometron::registry::RegisteredApplication> >(result);
      if (applications.size() != instance.at("applications").size()) {
        ++failures;
        continue;
      }
      for (std::size_t index = 0; index < applications.size(); ++index) {
        const auto& actual = applications[index];
        const auto& entry = instance.at("applications").at(index);
        if (actual.application_id != entry.at("application_id") ||
            actual.version != entry.at("version") || actual.executable != entry.at("executable") ||
            actual.bundle_sha256 != entry.at("bundle_sha256") ||
            nlohmann::json(actual.arguments) != entry.value("arguments", nlohmann::json::array())) {
          std::cerr << k_name << " [" << vector.at("id").get<std::string>()
                    << "]: validated fields/default arguments changed\n";
          ++failures;
        }
      }
    }
    std::cout << k_name << ": stage-A vectors=" << vectors.at("schema_vectors").size()
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
#endif
  } catch (const std::exception& error) {
    std::cerr << k_name << ": fixture/check failed: " << error.what() << '\n';
    return 1;
  }
}
