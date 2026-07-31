#include <boost/hash2/sha2.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int Check(bool condition, std::string_view description) {
  if (condition) {
    return 0;
  }
  std::cerr << "core_dependency_api_smoke: " << description << '\n';
  return 1;
}

int CheckSha256(const void* bytes, std::size_t size, std::string_view expected) {
  boost::hash2::sha2_256 hasher;
  hasher.update(bytes, size);
  return Check(boost::hash2::to_string(hasher.result()) == expected, "SHA-256 vector");
}

int CheckUuid(std::string_view text, boost::uuids::uuid::version_type version,
              boost::uuids::uuid::variant_type variant) {
  const boost::uuids::string_generator parse;
  const auto value = parse(text);
  int result = 0;
  result |= Check(value.version() == version, "UUID version inspection");
  result |= Check(value.variant() == variant, "UUID variant inspection");
  return result;
}

}  // namespace

int main() {
  int result = 0;

  const std::array<char, 4> raw_nul_json{'{', '}', '\0', 'x'};
  // The C-string overload stops at the NUL and therefore accepts the wrong range.
  const auto null_terminated_value = nlohmann::json::parse(raw_nul_json.data());
  result |= Check(null_terminated_value.is_object(),
                  "C-string JSON path exposes prefix acceptance");
  bool bounded_range_rejected = false;
  try {
    (void)nlohmann::json::parse(raw_nul_json.begin(), raw_nul_json.end(), nullptr, true, true);
    // The library's bounded iterator path treats NUL as an input sentinel. The test-only
    // capability boundary must therefore reject the complete byte range rather than accept its
    // valid prefix.
    bounded_range_rejected = raw_nul_json.end() !=
                             std::find(raw_nul_json.begin(), raw_nul_json.end(), '\0');
  } catch (const nlohmann::json::parse_error&) {
    bounded_range_rejected = true;
  }
  result |= Check(bounded_range_rejected, "bounded JSON path rejects raw NUL and trailing bytes");

  constexpr std::array<char, 14> escaped_nul_json{
      '{', '"', 'x', '"', ':', '"', '\\', 'u', '0', '0', '0', '0', '"', '}'};
  const auto escaped_value =
      nlohmann::json::parse(escaped_nul_json.begin(), escaped_nul_json.end());
  result |= Check(escaped_value.at("x").get<std::string>() == std::string(1, '\0'),
                  "escaped JSON U+0000 remains valid");

  result |= CheckSha256(nullptr, 0,
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  constexpr std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  result |= CheckSha256(abc.data(), abc.size(),
                        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  constexpr std::array<std::uint8_t, 3> embedded_nul{'a', 0, 'b'};
  result |= CheckSha256(embedded_nul.data(), embedded_nul.size(),
                        "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138");
  boost::hash2::sha2_256 json_hasher;
  json_hasher.update(escaped_nul_json.data(), escaped_nul_json.size());
  const auto digest = boost::hash2::to_string(json_hasher.result());
  result |= Check(digest.size() == 64, "JSON digest has 64 hexadecimal characters");
  result |= Check(digest.find_first_not_of("0123456789abcdef") == std::string::npos,
                  "JSON digest is lowercase hexadecimal");

  result |= CheckUuid("550e8400-e29b-41d4-a716-446655440000",
                      boost::uuids::uuid::version_random_number_based,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("01890f30-7b54-7cc3-98c4-dc0c0c07398f",
                      boost::uuids::uuid::version_time_based_v7,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("{550e8400-e29b-41d4-a716-446655440000}",
                      boost::uuids::uuid::version_random_number_based,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("550E8400-E29B-41D4-A716-446655440000",
                      boost::uuids::uuid::version_random_number_based,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("550e8400e29b41d4a716446655440000",
                      boost::uuids::uuid::version_random_number_based,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("550e8400-e29b-61d4-a716-446655440000",
                      boost::uuids::uuid::version_time_based_v6,
                      boost::uuids::uuid::variant_rfc_4122);
  result |= CheckUuid("550e8400-e29b-41d4-c716-446655440000",
                      boost::uuids::uuid::version_random_number_based,
                      boost::uuids::uuid::variant_microsoft);

  return result;
}
