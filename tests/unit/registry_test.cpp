#include <exception>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "registry_validation.hpp"
#include "registry_vector_inventory.hpp"

int main(int argc, char** argv) {
  constexpr auto k_name = "registry_schema_vectors";
  try {
    if (argc > 2) return 2;
    const auto filename =
        argc == 2 ? std::string(argv[1])
                  : std::string(SITOMETRON_TEST_SOURCE_DIR) +
                        "/schemas/application-registry/v1/application-registry-vectors.json";
    sitometron::test::registry_vectors::Inventory vectors(
        filename, sitometron::test::registry_vectors::Group::kSchema);
    std::size_t failures = 0;
    for (const auto& vector : vectors.Rows()) {
      vectors.Consume(vector);
      const auto& instance = vector.at("instance");
      const auto result = sitometron::registry::internal::ValidateRegistryDocument(instance.dump());
      const bool accepted =
          std::holds_alternative<std::vector<sitometron::registry::RegisteredApplication>>(result);
      const bool expected = vector.at("valid").get<bool>();
      if (accepted != expected) {
        std::cerr << k_name << " [" << vector.at("id").get<std::string>()
                  << "]: expected stage-A acceptance=" << expected << ", got " << accepted << '\n';
        ++failures;
        continue;
      }
      if (!accepted) {
        const auto& reason = std::get<sitometron::registry::RegistryRefusal>(result).reason;
        if (reason.empty()) ++failures;
        const auto id = vector.at("id").get<std::string>();
        const bool root_fault = id.starts_with("root-") || id.starts_with("schema-") ||
                                id.starts_with("applications-") ||
                                id.starts_with("unknown-root-") || id == "missing-schema" ||
                                id == "missing-applications";
        if (!root_fault && reason.find("applications[0]") == std::string::npos) {
          std::cerr << k_name << " [" << id << "]: field refusal omitted entry index\n";
          ++failures;
        }
        continue;
      }
      const auto& applications =
          std::get<std::vector<sitometron::registry::RegisteredApplication>>(result);
      if (applications.size() != instance.at("applications").size()) {
        ++failures;
        continue;
      }
      for (std::size_t index = 0; index < applications.size(); ++index) {
        const auto& actual = applications[index];
        const auto& entry = instance.at("applications").at(index);
        if (actual.application_id != entry.at("application_id").get<std::string>() ||
            actual.version != entry.at("version").get<std::string>() ||
            actual.executable != entry.at("executable").get<std::string>() ||
            actual.bundle_sha256 != entry.at("bundle_sha256").get<std::string>() ||
            nlohmann::json(actual.arguments) != entry.value("arguments", nlohmann::json::array())) {
          std::cerr << k_name << " [" << vector.at("id").get<std::string>()
                    << "]: validated fields/default arguments changed\n";
          ++failures;
        }
      }
    }
    std::cout << k_name << ": stage-A vectors executed=" << vectors.Finish()
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << k_name << ": fixture/check failed: " << error.what() << '\n';
    return 1;
  }
}
