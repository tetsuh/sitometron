#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "identity_test_support.hpp"

namespace sitometron::test::identity_checks {
namespace {

constexpr std::array k_kinds{Kind::kJob, Kind::kWorker, Kind::kLaunch};

bool StableGrammar(std::string_view value) {
  const auto alnum = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 128 && alnum(value.front()) &&
         std::all_of(value.begin(), value.end(), [&](unsigned char c) {
           return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
         });
}

bool CoreAcceptsStable(std::string_view value) {
  const core::Uuid job{std::string(kJob)};
  const auto payload = nlohmann::json{{"operation_id", value}, {"outcome", "started"}}.dump();
  const auto normalized = core::NormalizeCandidate(core::InitialSnapshot(job, job),
                                                   {1, job, "worker_launch_observed", payload});
  return std::holds_alternative<core::InternalEvent>(normalized.value);
}

void GrammarOracles() {
  for (const auto& value :
       {std::string("A"), std::string("0"), std::string("a.Z_:-9"), std::string(128, 'A')})
    Require(StableGrammar(value) && CoreAcceptsStable(value), "identity_stable_id_grammar",
            "valid edge StableId was rejected");
  for (const auto& value :
       {std::string(), std::string(129, 'a'), std::string(".a"), std::string("_a"),
        std::string(":a"), std::string("-a"), std::string("a b"), std::string("a/b"),
        std::string("a\n"), std::string("\xc3\xa9"), std::string("a\0b", 3)})
    Require(!StableGrammar(value) && !CoreAcceptsStable(value), "identity_stable_id_grammar",
            "invalid near-miss StableId was accepted");
  for (const char version : {'4', '7'}) {
    std::string valid(version == '4' ? kWorker : kJob);
    for (const char variant : {'8', '9', 'a', 'b'}) {
      valid[19] = variant;
      Require(Canonical(valid, version), "canonical oracle", "valid variant was rejected");
    }
    auto upper = valid;
    upper[0] = 'A';
    auto wrong_version = valid;
    wrong_version[14] = version == '4' ? '7' : '4';
    auto wrong_variant = valid;
    wrong_variant[19] = 'c';
    auto misplaced = valid;
    std::swap(misplaced[7], misplaced[8]);
    auto compact = valid;
    compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
    for (const auto& invalid : {"{" + valid + "}", upper, wrong_version, wrong_variant, misplaced,
                                compact, valid + "x", valid + std::string(1, '\0')})
      Require(!Canonical(invalid, version), "canonical oracle", "UUID near-miss was accepted");
  }
}

void IdentityUuidV7Canonical() {
  constexpr std::string_view name = "identity_uuidv7_canonical";
  auto source = adapter::MakeProductionIdentitySource();
  std::set<std::string> seen;
  for (std::size_t i = 0; i < 10'000; ++i) {
    for (const auto kind : {Kind::kJob, Kind::kLaunch}) {
      const auto value = Generate(*source, kind);
      if (!value) Fail(name, "production v7 generation returned IdentitySourceExhausted");
      Require(Canonical(value.value(), '7'), name, "noncanonical UUIDv7 version/variant/text");
      Require(seen.insert(value.value()).second, name,
              "Job/launch UUIDv7 repeated within the instance");
    }
  }
  Require(seen.size() == 20'000, name, "combined Job/launch count mismatch");
}

void IdentityUuidV4Canonical() {
  constexpr std::string_view name = "identity_uuidv4_canonical";
  auto source = adapter::MakeProductionIdentitySource();
  std::set<std::string> seen;
  for (std::size_t i = 0; i < 10'000; ++i) {
    const auto value = Generate(*source, Kind::kWorker);
    if (!value) Fail(name, "production v4 generation returned IdentitySourceExhausted");
    Require(Canonical(value.value(), '4'), name, "noncanonical UUIDv4 version/variant/text");
    Require(seen.insert(value.value()).second, name, "Worker UUIDv4 repeated within the instance");
  }
}

void IdentityStableIdGrammar() {
  constexpr std::string_view name = "identity_stable_id_grammar";
  GrammarOracles();
  auto source = adapter::MakeProductionIdentitySource();
  for (std::size_t i = 0; i < 10'000; ++i) {
    const auto value = Generate(*source, Kind::kLaunch);
    if (!value) Fail(name, "launch generation returned IdentitySourceExhausted");
    Require(value.value().size() == 36 && Canonical(value.value(), '7') &&
                StableGrammar(value.value()) && CoreAcceptsStable(value.value()),
            name,
            "launch identity is not canonical v7 text satisfying the existing StableId grammar");
  }
}

bool StrictBurstEligible(std::uint64_t count, std::uint64_t elapsed_ns, std::size_t max_per_us,
                         bool wall_forward) {
  return wall_forward && elapsed_ns != 0 && max_per_us <= 64 && count * 1'000 <= elapsed_ns * 64;
}

void IdentityUuidV7MonotonicBurst() {
  constexpr std::string_view name = "identity_uuidv7_monotonic_burst";
  Require(StrictBurstEligible(64, 1'000, 64, true), name, "64/us boundary was excluded");
  Require(!StrictBurstEligible(65, 1'000, 65, true) && !StrictBurstEligible(64, 1'000, 65, true) &&
              !StrictBurstEligible(64, 1'000, 64, false),
          name, "over-rate or rollback burst incorrectly requires monotonicity");
  auto source = adapter::MakeProductionIdentitySource();
  std::vector<std::array<std::uint8_t, 16>> values;
  std::set<std::string> seen;
  std::vector<std::pair<std::int64_t, int>> occupancy;
  constexpr std::size_t k_count = 10'000;
  values.reserve(k_count);
  occupancy.reserve(k_count * 2);
  bool wall_forward = true;
  std::int64_t previous_wall = std::numeric_limits<std::int64_t>::min();
  const auto wall_us = [] {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  const auto started = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < k_count; ++i) {
    const auto before = wall_us();
    const auto value = Generate(*source, Kind::kJob);
    const auto after = wall_us();
    if (!value) Fail(name, "burst generation returned IdentitySourceExhausted");
    Require(Canonical(value.value(), '7'), name, "burst value is not canonical UUIDv7");
    Require(seen.insert(value.value()).second, name, "burst repeated an identity");
    values.push_back(Bytes(value.value()).bytes);
    wall_forward = wall_forward && before >= previous_wall && after >= before;
    previous_wall = after;
    if (after >= before) {
      Require(after < std::numeric_limits<std::int64_t>::max(), name, "wall bracket overflow");
      occupancy.emplace_back(before, 1);
      occupancy.emplace_back(after + 1, -1);
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  Require(elapsed > 0, name, "burst duration is not positive");
  std::sort(occupancy.begin(), occupancy.end());
  int active = 0;
  std::size_t maximum = 0;
  for (std::size_t i = 0; i < occupancy.size();) {
    const auto time = occupancy[i].first;
    do {
      active += occupancy[i++].second;
    } while (i < occupancy.size() && occupancy[i].first == time);
    Require(active >= 0, name, "invalid burst occupancy accounting");
    maximum = std::max(maximum, static_cast<std::size_t>(active));
  }
  const bool eligible =
      StrictBurstEligible(k_count, static_cast<std::uint64_t>(elapsed), maximum, wall_forward);
  std::cout << name << ": count=" << k_count << " elapsed_ns=" << elapsed << " values_per_us="
            << static_cast<double>(k_count) * 1'000.0 / static_cast<double>(elapsed)
            << " conservative_max_per_us=" << maximum << " wall_forward=" << wall_forward
            << " strict_increase_eligible=" << eligible << '\n';
  if (eligible)
    for (std::size_t i = 1; i < values.size(); ++i)
      Require(values[i - 1] < values[i], name,
              "UUIDs did not strictly increase in an eligible burst");
}

void CheckRetry(Kind first, Kind second, std::size_t repeats, bool success) {
  constexpr std::string_view name = "identity_never_repeats_within_instance";
  ScriptedSource fixture(2);
  const auto seed = Bytes(first == Kind::kWorker ? kWorker : kJob);
  auto fresh = seed;
  ++fresh.bytes.back();
  auto& script = fixture.For(first);
  script.values.push_back(seed);
  script.values.insert(script.values.end(), repeats, seed);
  script.values.push_back(fresh);
  Require(Generate(fixture.source, first).has_value(), name,
          "fresh scripted value was exhausted before collision setup");
  const auto result = Generate(fixture.source, second);
  Require(result.has_value() == success, name, "retry boundary returned the wrong result");
  const auto attempts = success ? repeats + 1 : 8;
  Require(script.calls == 1 + attempts, name, "retry count is not exactly bounded at eight");
  if (success) {
    Require(result && Bytes(result.value()) == fresh, name,
            "retry returned the repeated value instead of fresh bytes");
  } else {
    const auto next = Generate(fixture.source, second);
    Require(next && Bytes(*next) == fresh && script.calls == 10, name,
            "ninth queued value was consumed early or the per-operation budget stayed latched");
  }
  const auto before = script.calls;
  Require(!Generate(fixture.source, first), name, "full set did not refuse another generation");
  Require(script.calls == before, name, "full set called a generator");
}

void CheckCoreExhaustion(ScriptedSource& source) {
  constexpr std::string_view name = "identity_never_repeats_within_instance";
  FakeClock fake{{{"2026-10-28T00:00:00.000Z"}, {0}}};
  CountingClock clock(fake);
  CoreFixture fixture(source.source, clock);
  const auto result = fixture.writer->CreateJob();
  Require(result.ingress.code == internal::IngressCode::kServiceFailed && !result.job_id &&
              fixture.writer->failed() && !fixture.writer->ready(),
          name, "identity exhaustion did not latch the existing core failure state");
  Require(fixture.journal.count == 0 && clock.calls == 0, name,
          "exhausted creation read the clock or appended a Journal event");
  const auto calls = source.v7->calls + source.v4->calls;
  Require(fixture.writer->CreateJob().ingress.code == internal::IngressCode::kServiceFailed &&
              source.v7->calls + source.v4->calls == calls,
          name, "core failure latch allowed another generation");
}

void IdentityNeverRepeatsWithinInstance() {
  constexpr std::string_view name = "identity_never_repeats_within_instance";
  static_assert(sizeof(adapter::UuidBytes) == 16);
  for (const auto kind : k_kinds) {
    ScriptedSource fresh;
    fresh.For(kind).values = {Bytes(kind == Kind::kWorker ? kWorker : kJob)};
    Require(Generate(fresh.source, kind).has_value() && fresh.For(kind).calls == 1, name,
            "fresh first candidate must succeed with exactly one generator call");
    for (const std::size_t repeats : {1U, 6U, 7U}) CheckRetry(kind, kind, repeats, true);
    CheckRetry(kind, kind, 8, false);
    ScriptedSource zero(0);
    Require(!Generate(zero.source, kind) && zero.v7->calls == 0 && zero.v4->calls == 0, name,
            "zero capacity called a generator or issued a value");
    ScriptedSource one(1);
    one.For(kind).values = {Bytes(kind == Kind::kWorker ? kWorker : kJob)};
    Require(Generate(one.source, kind).has_value(), name, "capacity-one first value was refused");
    for (const auto next : k_kinds)
      Require(!Generate(one.source, next) && one.v7->calls + one.v4->calls == 1, name,
              "capacity is not shared across all three kinds");
  }
  for (const auto first : {Kind::kJob, Kind::kLaunch}) {
    const auto second = first == Kind::kJob ? Kind::kLaunch : Kind::kJob;
    CheckRetry(first, second, 1, true);
    CheckRetry(first, second, 7, true);
    CheckRetry(first, second, 8, false);
  }
  ScriptedSource three(3);
  three.v7->values = {Bytes(kJob), Bytes(kFresh)};
  three.v4->values = {Bytes(kWorker)};
  for (const auto kind : k_kinds)
    Require(Generate(three.source, kind).has_value(), name,
            "capacity-three mixed generation failed");
  for (const auto kind : k_kinds)
    Require(!Generate(three.source, kind) && three.v7->calls + three.v4->calls == 3, name,
            "mixed full set consumed another generator value");
  for (const auto kind : k_kinds) {
    ScriptedSource instance;
    instance.For(kind).values = {Bytes(kind == Kind::kWorker ? kWorker : kJob)};
    Require(Generate(instance.source, kind).has_value(), name,
            "a new instance incorrectly consulted earlier instances");
  }
  for (const std::size_t capacity : {0U, 1U, 2U}) {
    ScriptedSource source(capacity);
    source.v7->values = {Bytes(kJob), Bytes(kFresh)};
    source.v4->values = {Bytes(kWorker)};
    CheckCoreExhaustion(source);
    Require(source.v7->calls + source.v4->calls == capacity, name,
            "partial create did not consume exactly the available capacity");
  }
  for (const auto exhausted_kind : k_kinds) {
    ScriptedSource source;
    auto& repeated = source.For(exhausted_kind);
    const auto seed = Bytes(exhausted_kind == Kind::kWorker ? kWorker : kJob);
    repeated.values.push_back(seed);
    Require(Generate(source.source, exhausted_kind).has_value(), name,
            "core collision seed failed");
    if (exhausted_kind == Kind::kLaunch) repeated.values.push_back(Bytes(kFresh));
    repeated.values.insert(repeated.values.end(), 8, seed);
    if (exhausted_kind != Kind::kWorker) source.v4->values = {Bytes(kWorker)};
    if (exhausted_kind == Kind::kWorker) source.v7->values = {Bytes(kJob), Bytes(kFresh)};
    if (exhausted_kind == Kind::kJob) repeated.values.push_back(Bytes(kFresh));
    CheckCoreExhaustion(source);
    Require(repeated.calls == (exhausted_kind == Kind::kWorker ? 9U : 10U), name,
            "core collision stage did not stop after exactly eight repeat attempts");
  }
}

void IdentitySourceNeverExhausted() {
  constexpr std::string_view name = "identity_source_never_exhausted";
  auto source = adapter::MakeProductionIdentitySource();
  std::set<std::string> seen;
  for (std::size_t i = 0; i < 10'000; ++i) {
    const auto value = Generate(*source, k_kinds[i % k_kinds.size()]);
    if (!value) Fail(name, "production source exhausted within ten thousand mixed calls");
    Require(seen.insert(value.value()).second, name, "mixed production calls repeated a value");
  }
}

bool UtcMillisecondForm(std::string_view text) {
  if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[19] != '.' || text[23] != 'Z')
    return false;
  const auto number = [&](std::size_t start, std::size_t length) {
    int result = -1;
    const auto parsed = std::from_chars(text.data() + start, text.data() + start + length, result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + start + length ? result : -1;
  };
  const auto year = number(0, 4);
  const auto month = number(5, 2);
  const auto day = number(8, 2);
  const auto date = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                    std::chrono::day{static_cast<unsigned>(day)};
  return year >= 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && date.ok() &&
         number(11, 2) >= 0 && number(11, 2) <= 23 && number(14, 2) >= 0 && number(14, 2) <= 59 &&
         number(17, 2) >= 0 && number(17, 2) <= 59 && number(20, 3) >= 0 && number(20, 3) <= 999;
}

template <class Clock, class Duration>
adapter::TickInstant Ticks(std::chrono::time_point<Clock, Duration> point) {
  return {point.time_since_epoch().count(), {Duration::period::num, Duration::period::den}};
}

void ClockRecordedAtRfc3339() {
  constexpr std::string_view name = "clock_recorded_at_rfc3339";
  using namespace std::chrono;
  const auto epoch = system_clock::time_point{};
  const auto leap = sys_days{year{2024} / February / 29} + hours{12} + minutes{34} + seconds{56} +
                    milliseconds{789};
  const auto last = sys_days{year{2023} / December / 31} + hours{23} + minutes{59} + seconds{59} +
                    milliseconds{999};
  const std::vector<std::pair<adapter::TickInstant, std::string_view>> cases{
      {Ticks(epoch), "1970-01-01T00:00:00.000Z"},
      {Ticks(epoch + milliseconds{1}), "1970-01-01T00:00:00.001Z"},
      {Ticks(epoch - milliseconds{1}), "1969-12-31T23:59:59.999Z"},
      {Ticks(sys_time<nanoseconds>{nanoseconds{-1}}), "1969-12-31T23:59:59.999Z"},
      {Ticks(leap), "2024-02-29T12:34:56.789Z"},
      {Ticks(last), "2023-12-31T23:59:59.999Z"},
      {Ticks(last + milliseconds{1}), "2024-01-01T00:00:00.000Z"},
      {Ticks(last + nanoseconds{999'999}), "2023-12-31T23:59:59.999Z"}};
  auto state = std::make_shared<SampleState>();
  adapter::SystemClock clock(std::make_unique<ScriptedSampler>(state));
  for (const auto& [instant, expected] : cases) {
    state->value.wall = instant;
    ++state->value.steady.ticks;
    const auto reading = ReadClock(clock, name);
    Require(
        reading.recorded_at.rfc3339 == expected && UtcMillisecondForm(reading.recorded_at.rfc3339),
        name, "UTC RFC 3339 millisecond formatting differs from the injected time point");
  }
  for (const auto invalid :
       {"1970-01-01T00:00:00Z", "1970-01-01T00:00:00.00Z", "1970-01-01T00:00:00.0000Z",
        "1970-01-01t00:00:00.000Z", "1970-01-01T00:00:00.000z", "1970-01-01T00:00:00.000+00:00",
        "1970-01-01T00:00:00.000", "1970-01-01T00:00:00.000Zx", "2023-02-29T00:00:00.000Z",
        "2024-02-30T00:00:00.000Z"})
    Require(!UtcMillisecondForm(invalid), name, "output-format near-miss passed the oracle");
  state->value.wall = Ticks(sys_days{year{10000} / January / 1});
  Require(ReadThrows(clock), name, "extended year was emitted instead of refusing formatting");
}

void CheckMonotonic(std::int64_t origin, adapter::TickPeriod period,
                    const std::vector<std::pair<std::int64_t, std::uint64_t>>& cases) {
  constexpr std::string_view name = "clock_monotonic_instant";
  auto state = std::make_shared<SampleState>();
  state->value.steady = {origin, period};
  adapter::SystemClock clock(std::make_unique<ScriptedSampler>(state));
  Require(state->calls == 1, name, "steady origin was not captured exactly once at construction");
  for (const auto& [ticks, expected] : cases) {
    state->value.steady.ticks = ticks;
    const auto calls = state->calls;
    const auto reading = ReadClock(clock, name);
    Require(reading.monotonic_time.nanoseconds_since_origin == expected, name,
            "checked elapsed nanoseconds differ from the injected steady time point");
    Require(state->calls == calls + 1 && state->value.steady.ticks == ticks, name,
            "Read sampled more than once or advanced injected time");
  }
}

void ClockMonotonicInstant() {
  constexpr std::string_view name = "clock_monotonic_instant";
  using Limit = std::numeric_limits<std::int64_t>;
  constexpr auto k_max = std::numeric_limits<std::uint64_t>::max();
  const auto origin = Ticks(std::chrono::steady_clock::time_point{});
  CheckMonotonic(
      origin.ticks, origin.period,
      {{0, 0}, {0, 0}, {1, 1'000'000'000U * origin.period.numerator / origin.period.denominator}});
  CheckMonotonic(-5, {1, 1'000'000'000}, {{-5, 0}, {-5, 0}, {-4, 1}, {0, 5}, {7, 12}});
  CheckMonotonic(0, {1, 10'000'000}, {{0, 0}, {1, 100}, {2, 200}});
  CheckMonotonic(0, {1, 2'000'000'000}, {{0, 0}, {1, 0}, {2, 1}, {3, 1}});
  CheckMonotonic(Limit::min(), {1, 1'000'000'000}, {{Limit::min(), 0}, {Limit::max(), k_max}});
  CheckMonotonic(0, {1, 3'000'000'000},
                 {{Limit::max(), static_cast<std::uint64_t>(Limit::max()) / 3}});
  // Exercise both wide-product divisions even when intermediate products exceed 64 bits.
  CheckMonotonic(0, {k_max, k_max - 2}, {{0, 0}, {1, 1'000'000'000}, {10, 10'000'000'000}});
  CheckMonotonic(0, {(k_max - 2) / 2, k_max - 2},
                 {{0, 0}, {1, 499'999'999}, {2, 999'999'999}, {3, 1'499'999'999}});
  CheckMonotonic(0, {1, 1'000'000'000}, {{0, 0}, {1, 1}});  // Independent new origin.
  for (const bool overflow : {false, true}) {
    auto state = std::make_shared<SampleState>();
    state->value.steady = {overflow ? Limit::min() : 0,
                           {1, overflow ? 500'000'000U : 1'000'000'000U}};
    adapter::SystemClock clock(std::make_unique<ScriptedSampler>(state));
    state->value.steady.ticks = overflow ? 0 : -1;
    Require(ReadThrows(clock), name,
            "one-past-maximum/before-origin reading did not throw precommit");
    if (overflow) {
      state->value.steady.ticks = Limit::max();
      Require(ReadThrows(clock), name, "large elapsed conversion overflow did not throw precommit");
    }
  }
  auto state = std::make_shared<SampleState>();
  adapter::SystemClock clock(std::make_unique<ScriptedSampler>(state));
  state->value.steady.ticks = 2;
  (void)ReadClock(clock, name);
  state->value.steady.ticks = 1;
  Require(ReadThrows(clock), name, "a decrease after a successful Read was not refused");
  state->value.steady = {3, {1, 0}};
  Require(ReadThrows(clock), name, "zero period denominator was not refused");
  state->value.steady = {3, {0, 1}};
  Require(ReadThrows(clock), name, "zero period numerator was not refused");
  state->value.steady = {3, {1, 10'000'000}};
  Require(ReadThrows(clock), name, "changed steady period was not refused");
}

}  // namespace
}  // namespace sitometron::test::identity_checks

int main(int argc, char** argv) {
  using namespace sitometron::test::identity_checks;
  if (argc != 2) {
    std::cerr << "usage: sitometron_identity_tests <check>\n";
    return 2;
  }
  const std::string_view name(argv[1]);
  try {
    if (name == "identity_uuidv7_canonical")
      IdentityUuidV7Canonical();
    else if (name == "identity_uuidv7_monotonic_burst")
      IdentityUuidV7MonotonicBurst();
    else if (name == "identity_uuidv4_canonical")
      IdentityUuidV4Canonical();
    else if (name == "identity_stable_id_grammar")
      IdentityStableIdGrammar();
    else if (name == "identity_never_repeats_within_instance")
      IdentityNeverRepeatsWithinInstance();
    else if (name == "identity_source_never_exhausted")
      IdentitySourceNeverExhausted();
    else if (name == "clock_recorded_at_rfc3339")
      ClockRecordedAtRfc3339();
    else if (name == "clock_monotonic_instant")
      ClockMonotonicInstant();
    else if (name == "identity_seed_failure_refuses_startup")
      IdentitySeedFailureRefusesStartup();
    else if (name == "sitometrond_composes_production_identity")
      SitometrondComposesProductionIdentity();
    else {
      std::cerr << "unknown identity check: " << name << '\n';
      return 2;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << name << ": " << error.what() << '\n';
    return 1;
  }
}
