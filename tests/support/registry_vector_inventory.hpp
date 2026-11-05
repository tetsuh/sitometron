#ifndef SITOMETRON_TEST_REGISTRY_VECTOR_INVENTORY_HPP_
#define SITOMETRON_TEST_REGISTRY_VECTOR_INVENTORY_HPP_

#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>

namespace sitometron::test::registry_vectors {

enum class Group : std::uint8_t { kSchema, kLoader };

class Inventory final {
 public:
  Inventory(const std::string& filename, Group group)
      : group_(group == Group::kSchema ? "schema_vectors" : "loader_only_vectors") {
    std::ifstream input(filename, std::ios::binary);
    if (!input) throw std::runtime_error("registry vector file is missing");
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    if (input.bad() || bytes.find('\0') != std::string::npos || bytes.starts_with("\xef\xbb\xbf"))
      throw std::runtime_error("registry vector file contains unread/invalid bytes");
    document_ = nlohmann::json::parse(bytes.begin(), bytes.end());
    if (document_.at("format") != "application-registry-vectors.v1" ||
        document_.at("authority") != "ADR-0015" || !document_.at("schema_vectors").is_array() ||
        document_.at("schema_vectors").size() < 179 ||
        !document_.at("loader_only_vectors").is_array() ||
        document_.at("loader_only_vectors").size() < 69)
      throw std::runtime_error("registry vector inventory is incomplete (minimum 179/69)");
    std::set<std::string> all_ids;
    for (const auto* name : {"schema_vectors", "loader_only_vectors"}) {
      for (const auto& row : document_.at(name)) {
        const auto id = row.at("id").get<std::string>();
        if (id.empty() || !all_ids.insert(id).second)
          throw std::runtime_error("registry vector IDs must be globally unique");
        if (group_ == name) expected_.insert(id);
      }
    }
    if (expected_.empty()) throw std::runtime_error("registry vector owner group is empty");
  }

  const nlohmann::json& Rows() const { return document_.at(group_); }
  void Consume(const nlohmann::json& row) {
    const auto id = row.at("id").get<std::string>();
    if (!expected_.contains(id) || !executed_.insert(id).second)
      throw std::runtime_error("registry vector ID was consumed by the wrong owner or twice");
  }
  std::size_t Finish() const {
    if (executed_ != expected_) throw std::runtime_error("registry vectors were partly unread");
    return executed_.size();
  }

 private:
  std::string group_;
  nlohmann::json document_;
  std::set<std::string> expected_;
  std::set<std::string> executed_;
};

}  // namespace sitometron::test::registry_vectors

#endif  // SITOMETRON_TEST_REGISTRY_VECTOR_INVENTORY_HPP_
