#ifndef SITOMETRON_TESTS_SUPPORT_REDUCER_SNAPSHOT_JSON_HPP_
#define SITOMETRON_TESTS_SUPPORT_REDUCER_SNAPSHOT_JSON_HPP_

#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "sitometron/core/job_reducer.hpp"

// Conversions between the reducer vector JSON fixtures and core snapshots, shared by the reducer
// vector test and the Journal replay test.
namespace sitometron::test {

core::Uuid U(const nlohmann::json& j);
std::optional<core::StableId> Stable(const nlohmann::json& j);
std::optional<core::Digest> DigestValue(const nlohmann::json& j);
std::optional<core::Uuid> UuidValue(const nlohmann::json& j);

template <typename Enum, std::size_t Size>
Enum ClosedEnum(std::string_view value,
                const std::array<std::pair<std::string_view, Enum>, Size>& values,
                std::string_view field) {
  for (const auto& [name, enumeration] : values)
    if (value == name) return enumeration;
  throw std::invalid_argument("unknown " + std::string(field) + ": " + std::string(value));
}

core::JobState State(std::string_view value);
std::optional<core::TerminalOutcome> Outcome(const nlohmann::json& j);
// A null fixture is the absent position: the initial snapshot of the input's Job.
core::Snapshot SnapshotFrom(const nlohmann::json& j, const nlohmann::json* absent_input = nullptr);
nlohmann::json SnapshotJson(const core::Snapshot& s);

}  // namespace sitometron::test

#endif  // SITOMETRON_TESTS_SUPPORT_REDUCER_SNAPSHOT_JSON_HPP_
