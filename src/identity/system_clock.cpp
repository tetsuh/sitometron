#include "sitometron/identity/system_clock.hpp"

#include <memory>
#include <stdexcept>

namespace sitometron::identity {

// Intentional RED scaffold under Issue #115; no host clock is read here yet.
struct SystemClock::Impl {};

SystemClock::SystemClock() = default;

SystemClock::SystemClock(std::unique_ptr<ClockSampler> sampler) { (void)sampler; }

SystemClock::~SystemClock() = default;

core::ClockReading SystemClock::Read() { throw std::runtime_error("clock RED scaffold"); }

std::unique_ptr<core::ClockPort> MakeProductionClock() { return std::make_unique<SystemClock>(); }

}  // namespace sitometron::identity
