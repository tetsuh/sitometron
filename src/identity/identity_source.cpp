#include "sitometron/identity/identity_source.hpp"

#include <memory>

namespace sitometron::identity {

// Intentional RED scaffold under Issue #115; generation is implemented only after recorded RED.
struct ProductionIdentitySource::Impl {};

ProductionIdentitySource::ProductionIdentitySource(std::unique_ptr<UuidV7Generator> v7,
                                                   std::unique_ptr<UuidV4Generator> v4,
                                                   std::size_t capacity) {
  (void)v7;
  (void)v4;
  (void)capacity;
}

ProductionIdentitySource::~ProductionIdentitySource() = default;

core::JobSessionIdentityResult ProductionIdentitySource::GenerateJobSessionIdentity() {
  return core::IdentitySourceExhausted{};
}

core::WorkerIdentityResult ProductionIdentitySource::GenerateWorkerIdentity() {
  return core::IdentitySourceExhausted{};
}

core::LaunchOperationIdentityResult ProductionIdentitySource::GenerateLaunchOperationIdentity() {
  return core::IdentitySourceExhausted{};
}

std::unique_ptr<core::IdentitySourcePort> MakeProductionIdentitySource(std::size_t capacity) {
  return std::make_unique<ProductionIdentitySource>(nullptr, nullptr, capacity);
}

}  // namespace sitometron::identity
