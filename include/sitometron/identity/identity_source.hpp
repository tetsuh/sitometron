#ifndef SITOMETRON_IDENTITY_IDENTITY_SOURCE_HPP_
#define SITOMETRON_IDENTITY_IDENTITY_SOURCE_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::identity {

struct UuidBytes {
  std::array<std::uint8_t, 16> bytes{};
  friend bool operator==(const UuidBytes&, const UuidBytes&) = default;
};

class UuidV7Generator {
 public:
  virtual ~UuidV7Generator() = default;
  [[nodiscard]] virtual UuidBytes Generate() = 0;
};

class UuidV4Generator {
 public:
  virtual ~UuidV4Generator() = default;
  [[nodiscard]] virtual UuidBytes Generate() = 0;
};

inline constexpr std::size_t kDefaultIdentityCapacity = 1'048'576;

// Calls are serialized by the creating thread, outside the ingress mutex. No internal locking.
// Generator objects yield valid UUIDs of their named version and are owned for this instance's
// life.
class ProductionIdentitySource final : public core::IdentitySourcePort {
 public:
  ProductionIdentitySource(std::unique_ptr<UuidV7Generator> v7, std::unique_ptr<UuidV4Generator> v4,
                           std::size_t capacity = kDefaultIdentityCapacity);
  ~ProductionIdentitySource() override;
  ProductionIdentitySource(const ProductionIdentitySource&) = delete;
  ProductionIdentitySource& operator=(const ProductionIdentitySource&) = delete;

  [[nodiscard]] core::JobSessionIdentityResult GenerateJobSessionIdentity() override;
  [[nodiscard]] core::WorkerIdentityResult GenerateWorkerIdentity() override;
  [[nodiscard]] core::LaunchOperationIdentityResult GenerateLaunchOperationIdentity() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<core::IdentitySourcePort> MakeProductionIdentitySource(
    std::size_t capacity = kDefaultIdentityCapacity);

}  // namespace sitometron::identity

#endif  // SITOMETRON_IDENTITY_IDENTITY_SOURCE_HPP_
