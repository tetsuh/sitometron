#ifndef SITOMETRON_SPIKE_UUID_HPP_
#define SITOMETRON_SPIKE_UUID_HPP_

#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>

namespace sitometron::spike {

// Hand-rolled RFC 9562 UUIDs. Boost.UUID stays private to sitometron_core; the spike does not
// need to widen that boundary for two generators.
class UuidGenerator {
 public:
  UuidGenerator() : engine_(std::random_device{}()) {}

  std::string V7() {
    std::lock_guard lock(mutex_);
    const auto millis =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    std::uint64_t high = (millis << 16) | (0x7000U | (engine_() & 0x0FFFU));
    std::uint64_t low = (engine_() & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    return Format(high, low);
  }

  std::string V4() {
    std::lock_guard lock(mutex_);
    std::uint64_t high = (engine_() & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    std::uint64_t low = (engine_() & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    return Format(high, low);
  }

 private:
  static std::string Format(std::uint64_t high, std::uint64_t low) {
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    auto emit = [&](std::uint64_t value, int bytes) {
      for (int index = bytes - 1; index >= 0; --index) {
        const auto byte = static_cast<unsigned>((value >> (index * 8)) & 0xFFU);
        out.push_back(k_hex[byte >> 4]);
        out.push_back(k_hex[byte & 0xFU]);
      }
    };
    emit(high >> 32, 4);
    out.push_back('-');
    emit((high >> 16) & 0xFFFFU, 2);
    out.push_back('-');
    emit(high & 0xFFFFU, 2);
    out.push_back('-');
    emit(low >> 48, 2);
    out.push_back('-');
    emit(low & 0xFFFFFFFFFFFFULL, 6);
    return out;
  }

  std::mutex mutex_;
  std::mt19937_64 engine_;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_UUID_HPP_
