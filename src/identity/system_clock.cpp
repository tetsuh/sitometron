#include "sitometron/identity/system_clock.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace sitometron::identity {
namespace {

constexpr auto k_max = std::numeric_limits<std::uint64_t>::max();

std::uint64_t CheckedAdd(std::uint64_t left, std::uint64_t right) {
  if (right > k_max - left) throw std::overflow_error("elapsed time addition overflow");
  return left + right;
}

std::uint64_t CheckedMultiply(std::uint64_t left, std::uint64_t right) {
  if (right != 0 && left > k_max / right)
    throw std::overflow_error("elapsed time multiplication overflow");
  return left * right;
}

struct WideProduct {
  std::uint64_t high;
  std::uint64_t low;
};

// Two standard unsigned limbs, not a compiler-specific integer extension.
WideProduct MultiplyWide(std::uint64_t left, std::uint64_t right) {
  constexpr std::uint64_t k_mask = 0xFFFF'FFFF;
  const auto lower = (left & k_mask) * (right & k_mask);
  const auto middle = (left >> 32) * (right & k_mask) + (lower >> 32);
  const auto upper_middle = (middle & k_mask) + (left & k_mask) * (right >> 32);
  return {(left >> 32) * (right >> 32) + (middle >> 32) + (upper_middle >> 32),
          (upper_middle << 32) | (lower & k_mask)};
}

struct Division {
  std::uint64_t quotient;
  std::uint64_t remainder;
};

Division MultiplyDivide(std::uint64_t left, std::uint64_t right, std::uint64_t divisor) {
  if (right == 0 || left == 0) return {0, 0};
  if (left <= k_max / right) return {left * right / divisor, left * right % divisor};
  const auto product = MultiplyWide(left, right);
  if (product.high >= divisor) throw std::overflow_error("elapsed time quotient overflow");
  std::uint64_t quotient = 0;
  auto remainder = product.high;
  for (int bit = 63; bit >= 0; --bit) {
    const bool carry = (remainder >> 63) != 0;
    remainder = (remainder << 1) | ((product.low >> bit) & 1U);
    if (carry || remainder >= divisor) {
      // Unsigned subtraction includes the carried high bit through modulo arithmetic.
      remainder -= divisor;
      quotient |= std::uint64_t{1} << bit;
    }
  }
  return {quotient, remainder};
}

void ValidatePeriod(TickPeriod period) {
  if (period.numerator == 0 || period.denominator == 0)
    throw std::invalid_argument("sample tick period must be positive");
}

struct Scaled {
  std::uint64_t whole;
  bool fractional;
};

Scaled Scale(std::uint64_t magnitude, TickPeriod period, std::uint64_t units_per_second) {
  ValidatePeriod(period);
  const auto unit_gcd = std::gcd(units_per_second, period.denominator);
  const auto factor = units_per_second / unit_gcd;
  auto divisor = period.denominator / unit_gcd;
  const auto period_gcd = std::gcd(period.numerator, divisor);
  const auto numerator = period.numerator / period_gcd;
  divisor /= period_gcd;
  const auto integral = MultiplyDivide(magnitude, numerator, divisor);
  const auto fractional = MultiplyDivide(integral.remainder, factor, divisor);
  return {CheckedAdd(CheckedMultiply(integral.quotient, factor), fractional.quotient),
          fractional.remainder != 0};
}

std::uint64_t Magnitude(std::int64_t value) {
  return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                   : static_cast<std::uint64_t>(value);
}

struct TimestampField {
  std::size_t offset;
  std::size_t width;
};

void PutDigits(std::string& text, TimestampField field, unsigned value) {
  std::array<char, 4> buffer{};
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc{}) throw std::overflow_error("timestamp digit conversion failed");
  const auto length = static_cast<std::size_t>(converted.ptr - buffer.data());
  if (length > field.width || field.offset > text.size() ||
      field.width > text.size() - field.offset)
    throw std::out_of_range("timestamp field exceeds its fixed width");
  text.replace(field.offset + field.width - length, length, buffer.data(), length);
}

std::string FormatRecordedAt(TickInstant instant) {
  using namespace std::chrono;
  const auto scaled = Scale(Magnitude(instant.ticks), instant.period, 1'000);
  const bool negative = instant.ticks < 0;
  const auto magnitude = CheckedAdd(scaled.whole, negative && scaled.fractional ? 1 : 0);
  constexpr auto k_first =
      duration_cast<milliseconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
  constexpr auto k_after_last =
      duration_cast<milliseconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
  if ((negative && magnitude > Magnitude(k_first)) ||
      (!negative && magnitude >= static_cast<std::uint64_t>(k_after_last)))
    throw std::out_of_range("diagnostic timestamp is outside the four-digit year domain");
  const auto signed_ms =
      negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
  const sys_time<milliseconds> point{milliseconds{signed_ms}};
  const auto day = floor<days>(point);
  const year_month_day date{day};
  const hh_mm_ss parts{point - day};
  std::string text = "0000-00-00T00:00:00.000Z";
  PutDigits(text, {0, 4}, static_cast<unsigned>(static_cast<int>(date.year())));
  PutDigits(text, {5, 2}, static_cast<unsigned>(date.month()));
  PutDigits(text, {8, 2}, static_cast<unsigned>(date.day()));
  PutDigits(text, {11, 2}, static_cast<unsigned>(parts.hours().count()));
  PutDigits(text, {14, 2}, static_cast<unsigned>(parts.minutes().count()));
  PutDigits(text, {17, 2}, static_cast<unsigned>(parts.seconds().count()));
  PutDigits(text, {20, 3}, static_cast<unsigned>(parts.subseconds().count()));
  return text;
}

template <class Clock, class Duration>
TickInstant NativeTicks(std::chrono::time_point<Clock, Duration> point) {
  static_assert(sizeof(typename Duration::rep) <= sizeof(std::int64_t));
  static_assert(Duration::min().count() < 0);
  return {static_cast<std::int64_t>(point.time_since_epoch().count()),
          {static_cast<std::uint64_t>(Duration::period::num),
           static_cast<std::uint64_t>(Duration::period::den)}};
}

class StandardSampler final : public ClockSampler {
 public:
  ClockSample Sample() override {
    const auto wall = std::chrono::system_clock::now();
    const auto steady = std::chrono::steady_clock::now();
    return {NativeTicks(wall), NativeTicks(steady)};
  }
};

}  // namespace

struct SystemClock::Impl {
  explicit Impl(std::unique_ptr<ClockSampler> value) : sampler(std::move(value)) {
    if (!sampler) throw std::invalid_argument("clock sampler must be present");
    const auto sample = sampler->Sample();
    ValidatePeriod(sample.wall.period);
    ValidatePeriod(sample.steady.period);
    origin = sample.steady;
    last_ticks = origin.ticks;
  }

  std::unique_ptr<ClockSampler> sampler;
  TickInstant origin;
  std::int64_t last_ticks = 0;
  std::uint64_t last_ns = 0;
};

SystemClock::SystemClock() : SystemClock(std::make_unique<StandardSampler>()) {}

SystemClock::SystemClock(std::unique_ptr<ClockSampler> sampler)
    : impl_(std::make_unique<Impl>(std::move(sampler))) {}

SystemClock::~SystemClock() = default;

core::ClockReading SystemClock::Read() {
  const auto sample = impl_->sampler->Sample();
  if (sample.steady.period != impl_->origin.period)
    throw std::invalid_argument("steady sample period changed within the instance");
  if (sample.steady.ticks < impl_->origin.ticks || sample.steady.ticks < impl_->last_ticks)
    throw std::out_of_range("steady sample moved backwards");
  const auto delta = static_cast<std::uint64_t>(sample.steady.ticks) -
                     static_cast<std::uint64_t>(impl_->origin.ticks);
  const auto elapsed = Scale(delta, sample.steady.period, 1'000'000'000).whole;
  if (elapsed < impl_->last_ns) throw std::out_of_range("monotonic coordinate decreased");
  core::ClockReading reading{{FormatRecordedAt(sample.wall)}, {elapsed}};
  impl_->last_ticks = sample.steady.ticks;
  impl_->last_ns = elapsed;
  return reading;
}

std::unique_ptr<core::ClockPort> MakeProductionClock() { return std::make_unique<SystemClock>(); }

}  // namespace sitometron::identity
