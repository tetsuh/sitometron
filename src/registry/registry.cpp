#include "sitometron/registry/registry.hpp"

#include <algorithm>
#include <array>
#include <boost/hash2/sha2.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "registry_validation.hpp"

namespace sitometron::registry {
namespace {

using Json = nlohmann::json;
constexpr std::size_t k_registry_limit = 1'048'576;

std::string Entry(std::size_t index) {
  return "registry applications[" + std::to_string(index) + "]: ";
}

std::filesystem::path NativePath(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool HasOnly(const Json& object, std::initializer_list<std::string_view> names) {
  return std::ranges::all_of(object.items(), [&](const auto& member) {
    return std::ranges::find(names, member.key()) != names.end();
  });
}

bool Alphanumeric(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
}

bool Identifier(std::string_view text) {
  return !text.empty() && text.size() <= 128 && Alphanumeric(text.front()) &&
         std::ranges::all_of(text, [](unsigned char byte) {
           return Alphanumeric(byte) || byte == '.' || byte == '_' || byte == ':' || byte == '-';
         });
}

bool Digest(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

bool StringLength(const Json& value, std::size_t minimum, std::size_t maximum) {
  if (!value.is_string()) return false;
  const auto& text = value.get_ref<const std::string&>();
  if (text.find('\0') != std::string::npos) return false;
  // The parser already verified UTF-8. Count leading bytes, not bytes or UTF-16 code units.
  const auto count = static_cast<std::size_t>(
      std::ranges::count_if(text, [](unsigned char byte) { return (byte & 0xc0U) != 0x80U; }));
  return count >= minimum && count <= maximum;
}

std::optional<RegistryRefusal> ValidateEntry(const Json& entry, std::size_t index) {
  const auto prefix = Entry(index);
  if (!entry.is_object()) return RegistryRefusal{prefix + "entry must be an object"};
  if (!HasOnly(entry, {"application_id", "version", "executable", "arguments", "bundle_sha256"}))
    return RegistryRefusal{prefix + "unknown member"};
  for (const auto* field : {"application_id", "version", "executable", "bundle_sha256"}) {
    if (!entry.contains(field)) return RegistryRefusal{prefix + "missing " + field};
  }
  if (!StringLength(entry.at("application_id"), 1, 128) ||
      !Identifier(entry.at("application_id").get_ref<const std::string&>()))
    return RegistryRefusal{prefix + "invalid application_id"};
  if (!StringLength(entry.at("version"), 1, 128))
    return RegistryRefusal{prefix + "invalid version (1 to 128 code points, no U+0000)"};
  if (!StringLength(entry.at("executable"), 1, 4096))
    return RegistryRefusal{prefix + "invalid executable (1 to 4096 code points, no U+0000)"};
  if (!StringLength(entry.at("bundle_sha256"), 64, 64) ||
      !Digest(entry.at("bundle_sha256").get_ref<const std::string&>()))
    return RegistryRefusal{prefix + "invalid bundle_sha256"};
  if (entry.contains("arguments")) {
    const auto& arguments = entry.at("arguments");
    if (!arguments.is_array() || arguments.size() > 64)
      return RegistryRefusal{prefix + "arguments must be an array of at most 64 strings"};
    for (const auto& argument : arguments)
      if (!StringLength(argument, 0, 1024))
        return RegistryRefusal{prefix + "invalid argument (0 to 1024 code points, no U+0000)"};
  }
  return std::nullopt;
}

std::optional<RegistryRefusal> VerifyExecutable(const RegisteredApplication& application,
                                                std::size_t index) {
  const auto prefix = Entry(index);
  const auto path = NativePath(application.executable);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return RegistryRefusal{prefix + "executable is not a readable regular file"};
  std::ifstream input(path, std::ios::binary);
  if (!input) return RegistryRefusal{prefix + "cannot read executable"};
  boost::hash2::sha2_256 hash;
  std::array<char, 65536> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
  }
  if (input.bad() || !input.eof()) return RegistryRefusal{prefix + "cannot read executable bytes"};
  if (boost::hash2::to_string(hash.result()) != application.bundle_sha256)
    return RegistryRefusal{prefix + "executable digest mismatch"};
  return std::nullopt;
}

class DuplicateTracker final {
 public:
  bool operator()(int depth, Json::parse_event_t event, Json& value) {
    if (application_array_ && depth >= 2) context_ = application_index_;
    switch (event) {
      case Json::parse_event_t::object_start:
        keys_.emplace_back();
        break;
      case Json::parse_event_t::key:
        Key(depth, value);
        break;
      case Json::parse_event_t::array_start:
        StartArray(depth);
        break;
      case Json::parse_event_t::object_end:
        keys_.pop_back();
        break;
      default:
        break;
    }
    FinishValue(depth, event);
    return true;
  }

  [[nodiscard]] std::string Prefix() const { return context_ ? Entry(*context_) : "registry: "; }
  [[nodiscard]] std::optional<RegistryRefusal> Duplicate() const { return duplicate_; }

 private:
  void Key(int depth, const Json& value) {
    const auto& key = value.get_ref<const std::string&>();
    if (depth == 1) root_key_ = key;
    if (!keys_.back().insert(key).second && !duplicate_)
      duplicate_ = RegistryRefusal{Prefix() + "duplicate member name"};
  }

  void StartArray(int depth) {
    if (depth == 1 && root_key_ == "applications") {
      application_array_ = true;
      application_index_ = 0;
    }
  }

  void FinishValue(int depth, Json::parse_event_t event) {
    if (application_array_ && depth == 2 &&
        (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end ||
         event == Json::parse_event_t::value)) {
      ++application_index_;
      context_.reset();
    }
    if (event == Json::parse_event_t::array_end && depth == 1) {
      application_array_ = false;
      context_.reset();
    }
  }

  std::vector<std::set<std::string, std::less<>>> keys_;
  std::string root_key_;
  bool application_array_ = false;
  std::size_t application_index_ = 0;
  std::optional<std::size_t> context_;
  std::optional<RegistryRefusal> duplicate_;
};

std::variant<Json, RegistryRefusal> ParseStrictRegistry(std::string_view bytes) {
  if (bytes.find('\0') != std::string_view::npos) return RegistryRefusal{"registry: raw NUL byte"};
  if (bytes.starts_with("\xef\xbb\xbf"))
    return RegistryRefusal{"registry: leading byte-order mark"};
  DuplicateTracker tracker;
  const auto callback = [&tracker](int depth, Json::parse_event_t event, Json& value) {
    return tracker(depth, event, value);
  };
  Json document;
  try {
    document = Json::parse(bytes.begin(), bytes.end(), callback);
  } catch (const Json::exception&) {
    return RegistryRefusal{tracker.Prefix() + "invalid UTF-8 JSON"};
  }
  if (const auto duplicate = tracker.Duplicate()) return *duplicate;
  return document;
}

internal::ValidationResult ValidateRoot(const Json& document) {
  if (!document.is_object()) return RegistryRefusal{"registry: root must be an object"};
  if (!HasOnly(document, {"schema", "applications"}))
    return RegistryRefusal{"registry: unknown member"};
  if (!document.contains("schema") || !document.at("schema").is_string() ||
      document.at("schema") != "application-registry.v1")
    return RegistryRefusal{"registry: schema must be application-registry.v1"};
  if (!document.contains("applications") || !document.at("applications").is_array() ||
      document.at("applications").size() > 1024)
    return RegistryRefusal{"registry: applications must be an array of 0 to 1024 entries"};
  std::vector<RegisteredApplication> applications;
  applications.reserve(document.at("applications").size());
  std::size_t index = 0;
  for (const auto& entry : document.at("applications")) {
    if (const auto refusal = ValidateEntry(entry, index++)) return *refusal;
    applications.emplace_back(entry.at("application_id").get<std::string>(),
                              entry.at("version").get<std::string>(),
                              entry.at("executable").get<std::string>(),
                              entry.value("arguments", std::vector<std::string>{}),
                              entry.at("bundle_sha256").get<std::string>());
  }
  return applications;
}

}  // namespace

namespace internal {

ValidationResult ValidateRegistryDocument(std::string_view bytes) {
  auto parsed = ParseStrictRegistry(bytes);
  if (auto* refusal = std::get_if<RegistryRefusal>(&parsed)) return std::move(*refusal);
  return ValidateRoot(std::get<Json>(parsed));
}

std::optional<RegistryRefusal> ValidateRegistryLocations(
    const std::vector<RegisteredApplication>& applications) {
  std::set<std::string, std::less<>> identifiers;
  for (std::size_t index = 0; index < applications.size(); ++index) {
    const auto& application = applications[index];
    if (!identifiers.insert(application.application_id).second)
      return RegistryRefusal{Entry(index) + "duplicate application_id"};
    if (!NativePath(application.executable).is_absolute())
      return RegistryRefusal{Entry(index) + "executable must be absolute on this host"};
  }
  return std::nullopt;
}

}  // namespace internal

LoadResult LoadRegistry(const std::string& path) {
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input) return RegistryRefusal{"registry: cannot read registry file"};
  // Read at most cap+1 bytes even when file size changes during the read.
  std::string bytes(k_registry_limit + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  const auto count = static_cast<std::size_t>(input.gcount());
  if (count > k_registry_limit) return RegistryRefusal{"registry: file exceeds 1 MiB"};
  if (input.bad() || !input.eof()) return RegistryRefusal{"registry: cannot read registry bytes"};
  bytes.resize(count);
  auto result = internal::ValidateRegistryDocument(bytes);
  if (auto* refusal = std::get_if<RegistryRefusal>(&result)) return std::move(*refusal);
  auto applications = std::get<std::vector<RegisteredApplication>>(std::move(result));
  if (auto refusal = internal::ValidateRegistryLocations(applications)) return std::move(*refusal);
  for (std::size_t index = 0; index < applications.size(); ++index)
    if (auto refusal = VerifyExecutable(applications[index], index)) return std::move(*refusal);
  return applications;
}

}  // namespace sitometron::registry
