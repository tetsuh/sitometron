#include "sitometron/identity/identity_source.hpp"

#include <algorithm>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/time_generator_v7.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace sitometron::identity {
namespace {

UuidBytes OwnedBytes(const boost::uuids::uuid& value) {
  UuidBytes result;
  std::copy(value.begin(), value.end(), result.bytes.begin());
  return result;
}

class BoostV7Generator final : public UuidV7Generator {
 public:
  UuidBytes Generate() override { return OwnedBytes(generator_()); }

 private:
  boost::uuids::time_generator_v7 generator_;
};

class BoostV4Generator final : public UuidV4Generator {
 public:
  UuidBytes Generate() override { return OwnedBytes(generator_()); }

 private:
  boost::uuids::random_generator generator_;
};

struct UuidBytesHash {
  std::size_t operator()(const UuidBytes& value) const noexcept {
    auto hash = static_cast<std::size_t>(14695981039346656037ULL);
    for (const auto byte : value.bytes) {
      hash ^= byte;
      hash *= static_cast<std::size_t>(1099511628211ULL);
    }
    return hash;
  }
};

}  // namespace

struct ProductionIdentitySource::Impl {
  Impl(std::unique_ptr<UuidV7Generator> first, std::unique_ptr<UuidV4Generator> second,
       std::size_t limit)
      : v7(std::move(first)), v4(std::move(second)), capacity(limit) {
    if (!v7 || !v4) throw std::invalid_argument("identity generators must be present");
    if (capacity != 0) issued.reserve(capacity);
  }

  template <class Generator>
  std::variant<std::string, core::IdentitySourceExhausted> Next(Generator& generator, int version) {
    if (issued.size() >= capacity) return core::IdentitySourceExhausted{};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      const auto candidate = generator.Generate();
      boost::uuids::uuid value;
      std::copy(candidate.bytes.begin(), candidate.bytes.end(), value.begin());
      if (static_cast<int>(value.version()) != version ||
          value.variant() != boost::uuids::uuid::variant_rfc_4122)
        throw std::invalid_argument("generator violated its UUID version/variant precondition");
      if (issued.contains(candidate)) continue;
      // All fallible text materialization precedes remembering/publication. The result then moves.
      auto text = boost::uuids::to_string(value);
      issued.insert(candidate);
      return std::variant<std::string, core::IdentitySourceExhausted>{
          std::in_place_type<std::string>, std::move(text)};
    }
    return core::IdentitySourceExhausted{};
  }

  std::unique_ptr<UuidV7Generator> v7;
  std::unique_ptr<UuidV4Generator> v4;
  const std::size_t capacity;
  std::unordered_set<UuidBytes, UuidBytesHash> issued;
};

ProductionIdentitySource::ProductionIdentitySource(std::unique_ptr<UuidV7Generator> v7,
                                                   std::unique_ptr<UuidV4Generator> v4,
                                                   std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(v7), std::move(v4), capacity)) {}

ProductionIdentitySource::~ProductionIdentitySource() = default;

core::JobSessionIdentityResult ProductionIdentitySource::GenerateJobSessionIdentity() {
  auto result = impl_->Next(*impl_->v7, 7);
  if (auto* text = std::get_if<std::string>(&result))
    return core::GeneratedJobSessionIdentity{core::Uuid{std::move(*text)}};
  return core::IdentitySourceExhausted{};
}

core::WorkerIdentityResult ProductionIdentitySource::GenerateWorkerIdentity() {
  auto result = impl_->Next(*impl_->v4, 4);
  if (auto* text = std::get_if<std::string>(&result))
    return core::GeneratedWorkerIdentity{core::Uuid{std::move(*text)}};
  return core::IdentitySourceExhausted{};
}

core::LaunchOperationIdentityResult ProductionIdentitySource::GenerateLaunchOperationIdentity() {
  auto result = impl_->Next(*impl_->v7, 7);
  if (auto* text = std::get_if<std::string>(&result))
    return core::GeneratedLaunchOperationIdentity{core::StableId{std::move(*text)}};
  return core::IdentitySourceExhausted{};
}

std::unique_ptr<core::IdentitySourcePort> MakeProductionIdentitySource(std::size_t capacity) {
  auto v7 = std::make_unique<BoostV7Generator>();
  auto v4 = std::make_unique<BoostV4Generator>();
  return std::make_unique<ProductionIdentitySource>(std::move(v7), std::move(v4), capacity);
}

}  // namespace sitometron::identity
