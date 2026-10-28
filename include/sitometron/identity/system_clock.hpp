#ifndef SITOMETRON_IDENTITY_SYSTEM_CLOCK_HPP_
#define SITOMETRON_IDENTITY_SYSTEM_CLOCK_HPP_

#include <cstdint>
#include <memory>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::identity {

// Tick periods are positive seconds per tick. The steady period is fixed for a sampler's lifetime.
struct TickPeriod {
  std::uint64_t numerator = 1;
  std::uint64_t denominator = 1'000'000'000;
  friend bool operator==(const TickPeriod&, const TickPeriod&) = default;
};

struct TickInstant {
  std::int64_t ticks = 0;
  TickPeriod period;
};

struct ClockSample {
  TickInstant wall;
  TickInstant steady;
};

class ClockSampler {
 public:
  virtual ~ClockSampler() = default;
  [[nodiscard]] virtual ClockSample Sample() = 0;
};

// Read is called only by the writer thread. The origin is captured at construction; no locking.
// Sampling/formatting/arithmetic failures throw before commit, not through another result
// catalogue.
class SystemClock final : public core::ClockPort {
 public:
  SystemClock();
  explicit SystemClock(std::unique_ptr<ClockSampler> sampler);
  ~SystemClock() override;
  SystemClock(const SystemClock&) = delete;
  SystemClock& operator=(const SystemClock&) = delete;

  [[nodiscard]] core::ClockReading Read() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<core::ClockPort> MakeProductionClock();

}  // namespace sitometron::identity

#endif  // SITOMETRON_IDENTITY_SYSTEM_CLOCK_HPP_
