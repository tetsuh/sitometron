#include "sitometron/journal/record_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sitometron::journal {
namespace {
using Json = nlohmann::json;
using namespace std::string_view_literals;

constexpr std::size_t max_allocation_text_bytes = 65536;
constexpr std::size_t max_stable_id_chars = 128;
constexpr std::size_t max_version_chars = 128;
constexpr std::size_t max_principal_chars = 256;
constexpr std::size_t envelope_members = 6;

// --- closed enum spellings --------------------------------------------------------------------

constexpr std::array phase_names{
    std::pair{core::TimeoutPhase::kPreparation, "preparation"sv},
    std::pair{core::TimeoutPhase::kExecution, "execution"sv},
    std::pair{core::TimeoutPhase::kCooperativeStop, "cooperative_stop"sv},
    std::pair{core::TimeoutPhase::kProcessExitConfirmation, "process_exit_confirmation"sv}};
constexpr std::array completion_mode_names{
    std::pair{core::CompletionMode::kCooperative, "cooperative"sv},
    std::pair{core::CompletionMode::kForced, "forced"sv},
    std::pair{core::CompletionMode::kProcessAlreadyExited, "process_already_exited"sv}};
constexpr std::array outcome_names{std::pair{core::TerminalOutcome::kSucceeded, "succeeded"sv},
                                   std::pair{core::TerminalOutcome::kFailed, "failed"sv},
                                   std::pair{core::TerminalOutcome::kCancelled, "cancelled"sv},
                                   std::pair{core::TerminalOutcome::kTerminated, "terminated"sv},
                                   std::pair{core::TerminalOutcome::kTimedOut, "timed_out"sv}};
constexpr std::array cleanup_names{std::pair{core::CleanupStatus::kCompleted, "completed"sv},
                                   std::pair{core::CleanupStatus::kIncomplete, "incomplete"sv}};
constexpr std::array late_original_names{
    std::pair{core::EventType::kWorkerCompleted, "worker_completed"sv},
    std::pair{core::EventType::kWorkerFailed, "worker_failed"sv}};

template <typename Enum, std::size_t N>
std::optional<std::string_view> NameOf(Enum value,
                                       const std::array<std::pair<Enum, std::string_view>, N>& t) {
  for (const auto& [candidate, name] : t) {
    if (candidate == value) return name;
  }
  return std::nullopt;
}

template <typename Enum, std::size_t N>
std::optional<Enum> ValueOf(std::string_view name,
                            const std::array<std::pair<Enum, std::string_view>, N>& t) {
  for (const auto& [value, candidate] : t) {
    if (candidate == name) return value;
  }
  return std::nullopt;
}

// Name of a value already accepted by Violation(); an unlisted value yields an empty name, which
// cannot occur on the encode path because validation runs first.
template <typename Enum, std::size_t N>
std::string_view ValidatedName(Enum value,
                               const std::array<std::pair<Enum, std::string_view>, N>& t) {
  return NameOf(value, t).value_or(std::string_view{});
}

std::optional<core::EventType> EventTypeOf(std::string_view name) {
  for (int i = 0; i < static_cast<int>(core::EventType::kInvalid); ++i) {
    const auto type = static_cast<core::EventType>(i);
    if (core::ToString(type) == name) return type;
  }
  return std::nullopt;
}

// --- lexical validation ------------------------------------------------------------------------

bool IsLowerHex(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool IsAlnum(unsigned char c) {
  return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsUuid(std::string_view s, char version) {
  if (s.size() != 36) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (i == 14) {
      if (c != static_cast<unsigned char>(version)) return false;
    } else if (i == 19) {
      if (c != '8' && c != '9' && c != 'a' && c != 'b') return false;
    } else if (!IsLowerHex(c)) {
      return false;
    }
  }
  return true;
}

bool IsSha256(std::string_view s) {
  if (s.size() != 64) return false;
  for (const char c : s) {
    if (!IsLowerHex(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool IsStableId(std::string_view s) {
  if (s.empty() || s.size() > max_stable_id_chars) return false;
  if (!IsAlnum(static_cast<unsigned char>(s.front()))) return false;
  for (const char raw : s.substr(1)) {
    const auto c = static_cast<unsigned char>(raw);
    if (!IsAlnum(c) && c != '.' && c != '_' && c != ':' && c != '-') return false;
  }
  return true;
}

// Returns the number of code points, or nullopt when the text is not well-formed UTF-8.
std::optional<std::size_t> Utf8CodePoints(std::string_view s) {
  std::size_t count = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto lead = static_cast<unsigned char>(s[i]);
    std::size_t length = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xBF;
    if (lead <= 0x7F) {
      length = 1;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
    } else if (lead == 0xE0) {
      length = 3;
      second_min = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
      length = 3;
    } else if (lead == 0xED) {
      length = 3;
      second_max = 0x9F;
    } else if (lead == 0xF0) {
      length = 4;
      second_min = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      length = 4;
    } else if (lead == 0xF4) {
      length = 4;
      second_max = 0x8F;
    } else {
      return std::nullopt;
    }
    if (i + length > s.size()) return std::nullopt;
    for (std::size_t k = 1; k < length; ++k) {
      const auto c = static_cast<unsigned char>(s[i + k]);
      const unsigned char lo = k == 1 ? second_min : 0x80;
      const unsigned char hi = k == 1 ? second_max : 0xBF;
      if (c < lo || c > hi) return std::nullopt;
    }
    i += length;
    ++count;
  }
  return count;
}

bool BoundedText(std::string_view s, std::size_t max_chars) {
  const auto points = Utf8CodePoints(s);
  return points.has_value() && *points >= 1 && *points <= max_chars;
}

unsigned DigitValue(unsigned char c) { return static_cast<unsigned>(c - '0'); }

struct DigitRange {
  unsigned min;
  unsigned max;
};

bool TwoDigits(std::string_view s, std::size_t at, DigitRange range) {
  if (at + 2 > s.size()) return false;
  const auto a = static_cast<unsigned char>(s[at]);
  const auto b = static_cast<unsigned char>(s[at + 1]);
  if (!IsDigit(a) || !IsDigit(b)) return false;
  const unsigned value = DigitValue(a) * 10U + DigitValue(b);
  return value >= range.min && value <= range.max;
}

bool IsLeapYear(unsigned year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

unsigned DaysInMonth(unsigned month, bool leap) {
  constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leap ? 29 : days[month - 1];
}

// RFC 3339 date-time exactly as the repository's schema format checker accepts it
// (jsonschema `date-time` via rfc3339-validator): T and Z in either case, year 0001-9999,
// calendar-valid day, seconds 00-59 (no leap second), optional fraction, numeric offset.
bool IsRfc3339(std::string_view s) {
  if (s.size() < 20) return false;
  unsigned year = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (!IsDigit(c)) return false;
    year = year * 10U + DigitValue(c);
  }
  if (year == 0) return false;
  if (s[4] != '-' || !TwoDigits(s, 5, {1, 12}) || s[7] != '-' || !TwoDigits(s, 8, {1, 31}) ||
      (s[10] != 'T' && s[10] != 't') || !TwoDigits(s, 11, {0, 23}) || s[13] != ':' ||
      !TwoDigits(s, 14, {0, 59}) || s[16] != ':' || !TwoDigits(s, 17, {0, 59})) {
    return false;
  }
  const unsigned month = DigitValue(static_cast<unsigned char>(s[5])) * 10U +
                         DigitValue(static_cast<unsigned char>(s[6]));
  if (const unsigned day = DigitValue(static_cast<unsigned char>(s[8])) * 10U +
                           DigitValue(static_cast<unsigned char>(s[9]));
      day > DaysInMonth(month, IsLeapYear(year))) {
    return false;
  }
  std::size_t i = 19;
  if (i < s.size() && s[i] == '.') {
    ++i;
    const auto start = i;
    while (i < s.size() && IsDigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == start) return false;
  }
  if (i >= s.size()) return false;
  if (s[i] == 'Z' || s[i] == 'z') return i + 1 == s.size();
  if (s[i] != '+' && s[i] != '-') return false;
  return i + 6 == s.size() && TwoDigits(s, i + 1, {0, 23}) && s[i + 3] == ':' &&
         TwoDigits(s, i + 4, {0, 59});
}

// --- semantic validation (shared by encoder and decoder) ---------------------------------------

template <typename Payload>
const Payload* As(const core::EventPayload& payload) {
  return std::get_if<Payload>(&payload);
}

std::optional<std::string> PayloadViolation(const core::LogicalJobEvent& e) {
  using core::EventType;
  const auto& p = e.payload;
  switch (e.event_type) {
    case EventType::kJobCreated: {
      const auto* v = As<core::JobCreatedPayload>(p);
      if (v == nullptr) return "payload is not job_created";
      if (!IsUuid(v->session_id.value, '7')) return "session_id is not a UUIDv7";
      return std::nullopt;
    }
    case EventType::kResourcesCommitted: {
      const auto* v = As<core::ResourcesCommittedPayload>(p);
      if (v == nullptr) return "payload is not resources_committed";
      if (!IsStableId(v->allocation_id.value)) return "allocation_id is not a stable id";
      if (!IsSha256(v->allocation_digest.value)) return "allocation_digest is not sha256";
      if (!IsStableId(v->schema_id.value)) return "schema_id is not a stable id";
      if (v->schema_version < 1) return "resolved_allocation.schema_version below 1";
      if (v->payload_utf8.size() > max_allocation_text_bytes)
        return "payload_utf8 over 65536 bytes";
      if (!Utf8CodePoints(v->payload_utf8).has_value()) return "payload_utf8 is not UTF-8";
      return std::nullopt;
    }
    case EventType::kWorkerLaunchIntent: {
      const auto* v = As<core::WorkerLaunchIntentPayload>(p);
      if (v == nullptr) return "payload is not worker_launch_intent";
      if (!IsStableId(v->operation_id.value)) return "operation_id is not a stable id";
      if (!IsStableId(v->application_id.value)) return "application_id is not a stable id";
      if (!BoundedText(v->application_version, max_version_chars)) return "version out of bounds";
      if (!IsSha256(v->bundle_sha256.value)) return "bundle_sha256 is not sha256";
      if (!IsStableId(v->allocation_id.value)) return "allocation_id is not a stable id";
      if (!IsSha256(v->allocation_digest.value)) return "allocation_digest is not sha256";
      if (!IsUuid(v->worker_id.value, '4')) return "worker_id is not a UUIDv4";
      return std::nullopt;
    }
    case EventType::kWorkerLaunchObserved: {
      const auto* v = As<core::WorkerLaunchObservedPayload>(p);
      if (v == nullptr) return "payload is not worker_launch_observed";
      if (!IsStableId(v->operation_id.value)) return "operation_id is not a stable id";
      return std::nullopt;
    }
    case EventType::kWorkerRunning: {
      const auto* v = As<core::WorkerRunningPayload>(p);
      if (v == nullptr) return "payload is not worker_running";
      if (!IsUuid(v->worker_id.value, '4')) return "worker_id is not a UUIDv4";
      return std::nullopt;
    }
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted: {
      const auto* v = As<core::PrincipalPayload>(p);
      if (v == nullptr) return "payload is not a principal payload";
      if (!BoundedText(v->principal_subject, max_principal_chars)) {
        return "principal_subject out of bounds";
      }
      return std::nullopt;
    }
    case EventType::kTimeoutExpired: {
      const auto* v = As<core::TimeoutExpiredPayload>(p);
      if (v == nullptr) return "payload is not timeout_expired";
      if (!NameOf(v->phase, phase_names)) return "phase is not a closed value";
      if (v->timer_generation < 1) return "timer_generation below 1";
      return std::nullopt;
    }
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed: {
      const auto* v = As<core::WorkerEventPayload>(p);
      if (v == nullptr) return "payload is not a worker event payload";
      if (!IsUuid(v->worker_id.value, '4')) return "worker_id is not a UUIDv4";
      if (v->event_sequence < 1) return "event_sequence below 1";
      return std::nullopt;
    }
    case EventType::kProcessExitConfirmed: {
      const auto* v = As<core::ProcessExitConfirmedPayload>(p);
      if (v == nullptr) return "payload is not process_exit_confirmed";
      if (!NameOf(v->completion_mode, completion_mode_names)) {
        return "completion_mode is not a closed value";
      }
      if (!IsStableId(v->launch_operation_id.value)) {
        return "launch_operation_id is not a stable id";
      }
      return std::nullopt;
    }
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained: {
      const auto* v = As<core::SessionPayload>(p);
      if (v == nullptr) return "payload is not a session payload";
      if (!IsUuid(v->session_id.value, '7')) return "session_id is not a UUIDv7";
      return std::nullopt;
    }
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      if (As<core::EmptyPayload>(p) == nullptr) return "payload is not empty";
      return std::nullopt;
    case EventType::kTerminalOutcomeCommitted: {
      const auto* v = As<core::TerminalOutcomePayload>(p);
      if (v == nullptr) return "payload is not terminal_outcome_committed";
      if (!NameOf(v->outcome, outcome_names)) return "outcome is not a closed value";
      return std::nullopt;
    }
    case EventType::kResourcesReleased: {
      const auto* v = As<core::ResourcesReleasedPayload>(p);
      if (v == nullptr) return "payload is not resources_released";
      if (!IsStableId(v->allocation_id.value)) return "allocation_id is not a stable id";
      if (!IsSha256(v->allocation_digest.value)) return "allocation_digest is not sha256";
      return std::nullopt;
    }
    case EventType::kCleanupStatusRecorded: {
      const auto* v = As<core::CleanupStatusPayload>(p);
      if (v == nullptr) return "payload is not cleanup_status_recorded";
      if (!NameOf(v->status, cleanup_names)) return "status is not a closed value";
      return std::nullopt;
    }
    case EventType::kLateWorkerEvent: {
      const auto* v = As<core::LateWorkerEventPayload>(p);
      if (v == nullptr) return "payload is not late_worker_event";
      if (!NameOf(v->original_event_type, late_original_names)) {
        return "original_event_type is not a closed value";
      }
      if (!IsUuid(v->worker_id.value, '4')) return "worker_id is not a UUIDv4";
      if (v->event_sequence < 1) return "event_sequence below 1";
      return std::nullopt;
    }
    case EventType::kInvalid:
      break;
  }
  return "event_type is not a closed value";
}

std::optional<std::string> Violation(const core::LogicalJobEvent& e) {
  if (e.schema_version != 1) return "schema_version is not 1";
  if (e.sequence < 1) return "sequence below 1";
  if (!IsRfc3339(e.recorded_at.rfc3339)) return "recorded_at is not an RFC 3339 date-time";
  if (!IsUuid(e.job_id.value, '7')) return "job_id is not a UUIDv7";
  return PayloadViolation(e);
}

// --- canonical emitter -------------------------------------------------------------------------

class Emitter {
 public:
  Emitter& Str(std::string_view s) {
    out_.push_back('"');
    for (const char raw : s) {
      const auto c = static_cast<unsigned char>(raw);
      if (c == '"') {
        out_ += "\\\"";
      } else if (c == '\\') {
        out_ += "\\\\";
      } else if (c < 0x20) {
        constexpr std::string_view hex = "0123456789abcdef";
        out_ += "\\u00";
        out_.push_back(hex[static_cast<std::size_t>(c >> 4U)]);
        out_.push_back(hex[static_cast<std::size_t>(c & 0x0FU)]);
      } else {
        out_.push_back(raw);
      }
    }
    out_.push_back('"');
    return *this;
  }
  Emitter& Num(std::uint64_t v) {
    out_ += std::to_string(v);
    return *this;
  }
  Emitter& Key(std::string_view key) {
    if (first_.back() == 0) out_.push_back(',');
    first_.back() = 0;
    Str(key);
    out_.push_back(':');
    return *this;
  }
  void Begin() {
    out_.push_back('{');
    first_.push_back(1);
  }
  void End() {
    out_.push_back('}');
    first_.pop_back();
  }
  std::string Take() { return std::move(out_); }

 private:
  std::string out_;
  std::vector<char> first_{};
};

void EmitPayload(Emitter& w, const core::LogicalJobEvent& e) {
  using core::EventType;
  const auto& p = e.payload;
  w.Begin();
  switch (e.event_type) {
    case EventType::kJobCreated:
      w.Key("session_id").Str(As<core::JobCreatedPayload>(p)->session_id.value);
      break;
    case EventType::kResourcesCommitted: {
      const auto& v = *As<core::ResourcesCommittedPayload>(p);
      w.Key("allocation_id").Str(v.allocation_id.value);
      w.Key("allocation_digest").Str(v.allocation_digest.value);
      w.Key("resolved_allocation");
      w.Begin();
      w.Key("schema_id").Str(v.schema_id.value);
      w.Key("schema_version").Num(v.schema_version);
      w.Key("payload_utf8").Str(v.payload_utf8);
      w.End();
      break;
    }
    case EventType::kWorkerLaunchIntent: {
      const auto& v = *As<core::WorkerLaunchIntentPayload>(p);
      w.Key("operation_id").Str(v.operation_id.value);
      w.Key("application");
      w.Begin();
      w.Key("application_id").Str(v.application_id.value);
      w.Key("version").Str(v.application_version);
      w.Key("bundle_sha256").Str(v.bundle_sha256.value);
      w.End();
      w.Key("allocation_id").Str(v.allocation_id.value);
      w.Key("allocation_digest").Str(v.allocation_digest.value);
      w.Key("worker_id").Str(v.worker_id.value);
      break;
    }
    case EventType::kWorkerLaunchObserved: {
      const auto& v = *As<core::WorkerLaunchObservedPayload>(p);
      w.Key("operation_id").Str(v.operation_id.value);
      w.Key("outcome").Str(v.started ? "started"sv : "failed"sv);
      break;
    }
    case EventType::kWorkerRunning:
      w.Key("worker_id").Str(As<core::WorkerRunningPayload>(p)->worker_id.value);
      break;
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      w.Key("principal_subject").Str(As<core::PrincipalPayload>(p)->principal_subject);
      break;
    case EventType::kTimeoutExpired: {
      const auto& v = *As<core::TimeoutExpiredPayload>(p);
      w.Key("phase").Str(ValidatedName(v.phase, phase_names));
      w.Key("timer_generation").Num(v.timer_generation);
      break;
    }
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed: {
      const auto& v = *As<core::WorkerEventPayload>(p);
      w.Key("worker_id").Str(v.worker_id.value);
      w.Key("event_sequence").Num(v.event_sequence);
      break;
    }
    case EventType::kProcessExitConfirmed: {
      const auto& v = *As<core::ProcessExitConfirmedPayload>(p);
      w.Key("completion_mode").Str(ValidatedName(v.completion_mode, completion_mode_names));
      w.Key("launch_operation_id").Str(v.launch_operation_id.value);
      break;
    }
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      w.Key("session_id").Str(As<core::SessionPayload>(p)->session_id.value);
      break;
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      break;
    case EventType::kTerminalOutcomeCommitted:
      w.Key("outcome").Str(
          ValidatedName(As<core::TerminalOutcomePayload>(p)->outcome, outcome_names));
      break;
    case EventType::kResourcesReleased: {
      const auto& v = *As<core::ResourcesReleasedPayload>(p);
      w.Key("allocation_id").Str(v.allocation_id.value);
      w.Key("allocation_digest").Str(v.allocation_digest.value);
      break;
    }
    case EventType::kCleanupStatusRecorded:
      w.Key("status").Str(ValidatedName(As<core::CleanupStatusPayload>(p)->status, cleanup_names));
      break;
    case EventType::kLateWorkerEvent: {
      const auto& v = *As<core::LateWorkerEventPayload>(p);
      w.Key("original_event_type").Str(ValidatedName(v.original_event_type, late_original_names));
      w.Key("worker_id").Str(v.worker_id.value);
      w.Key("event_sequence").Num(v.event_sequence);
      break;
    }
    case EventType::kInvalid:
      break;
  }
  w.End();
}

// Emits the canonical bytes of an event that passed Violation().
std::string Emit(const core::LogicalJobEvent& e) {
  Emitter w;
  w.Begin();
  w.Key("schema_version").Num(e.schema_version);
  w.Key("sequence").Num(e.sequence);
  w.Key("event_type").Str(core::ToString(e.event_type));
  w.Key("recorded_at").Str(e.recorded_at.rfc3339);
  w.Key("job_id").Str(e.job_id.value);
  w.Key("payload");
  EmitPayload(w, e);
  w.End();
  auto bytes = w.Take();
  bytes.push_back('\n');
  return bytes;
}

// --- JSON to event -----------------------------------------------------------------------------

struct Reader {
  const Json& object;
  std::size_t expected_members;
  std::string error;

  bool Ok() const { return error.empty(); }
  bool Fail(std::string message) {
    if (error.empty()) error = std::move(message);
    return false;
  }
  const Json* Get(std::string_view key, Json::value_t type) {
    if (!Ok()) return nullptr;
    const auto it = object.find(key);
    if (it == object.end()) {
      Fail(std::string(key) + " is missing");
      return nullptr;
    }
    if (it->type() != type) {
      Fail(std::string(key) + " has the wrong type");
      return nullptr;
    }
    return &*it;
  }
  std::string Str(std::string_view key) {
    const auto* v = Get(key, Json::value_t::string);
    return v == nullptr ? std::string{} : v->get<std::string>();
  }
  std::uint64_t U64(std::string_view key) {
    const auto* v = Get(key, Json::value_t::number_unsigned);
    return v == nullptr ? 0 : v->get<std::uint64_t>();
  }
  std::uint32_t U32(std::string_view key) {
    const auto value = U64(key);
    if (Ok() && value > std::numeric_limits<std::uint32_t>::max()) {
      Fail(std::string(key) + " exceeds 32 bits");
      return 0;
    }
    return static_cast<std::uint32_t>(value);
  }
  bool CheckMemberCount() {
    if (Ok() && object.size() != expected_members) return Fail("unexpected member");
    return Ok();
  }
};

template <typename Enum, std::size_t N>
Enum ClosedValue(Reader& r, std::string_view key,
                 const std::array<std::pair<Enum, std::string_view>, N>& table, Enum invalid) {
  const auto name = r.Str(key);
  if (!r.Ok()) return invalid;
  const auto value = ValueOf(name, table);
  if (!value) {
    r.Fail(std::string(key) + " is not a closed value");
    return invalid;
  }
  return *value;
}

std::optional<core::EventPayload> PayloadFromJson(core::EventType type, const Json& payload,
                                                  std::string& error) {
  using core::EventType;
  if (!payload.is_object()) {
    error = "payload is not an object";
    return std::nullopt;
  }
  auto make = [&payload, &error](std::size_t members,
                                 auto&& build) -> std::optional<core::EventPayload> {
    Reader r{payload, members, {}};
    core::EventPayload result = build(r);
    if (!r.CheckMemberCount()) {
      error = r.error;
      return std::nullopt;
    }
    return result;
  };
  switch (type) {
    case EventType::kJobCreated:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::JobCreatedPayload{core::Uuid{r.Str("session_id")}};
      });
    case EventType::kResourcesCommitted:
      return make(3, [](Reader& r) -> core::EventPayload {
        core::ResourcesCommittedPayload v;
        v.allocation_id = core::StableId{r.Str("allocation_id")};
        v.allocation_digest = core::Digest{r.Str("allocation_digest")};
        if (const auto* nested = r.Get("resolved_allocation", Json::value_t::object);
            nested != nullptr) {
          Reader n{*nested, 3, {}};
          v.schema_id = core::StableId{n.Str("schema_id")};
          v.schema_version = n.U32("schema_version");
          v.payload_utf8 = n.Str("payload_utf8");
          if (!n.CheckMemberCount()) r.Fail("resolved_allocation." + n.error);
        }
        return v;
      });
    case EventType::kWorkerLaunchIntent:
      return make(5, [](Reader& r) -> core::EventPayload {
        core::WorkerLaunchIntentPayload v;
        v.operation_id = core::StableId{r.Str("operation_id")};
        if (const auto* nested = r.Get("application", Json::value_t::object); nested != nullptr) {
          Reader n{*nested, 3, {}};
          v.application_id = core::StableId{n.Str("application_id")};
          v.application_version = n.Str("version");
          v.bundle_sha256 = core::Digest{n.Str("bundle_sha256")};
          if (!n.CheckMemberCount()) r.Fail("application." + n.error);
        }
        v.allocation_id = core::StableId{r.Str("allocation_id")};
        v.allocation_digest = core::Digest{r.Str("allocation_digest")};
        v.worker_id = core::Uuid{r.Str("worker_id")};
        return v;
      });
    case EventType::kWorkerLaunchObserved:
      return make(2, [](Reader& r) -> core::EventPayload {
        core::WorkerLaunchObservedPayload v;
        v.operation_id = core::StableId{r.Str("operation_id")};
        const auto outcome = r.Str("outcome");
        if (r.Ok() && outcome != "started" && outcome != "failed") {
          r.Fail("outcome is not a closed value");
        }
        v.started = outcome == "started";
        return v;
      });
    case EventType::kWorkerRunning:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::WorkerRunningPayload{core::Uuid{r.Str("worker_id")}};
      });
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::PrincipalPayload{r.Str("principal_subject")};
      });
    case EventType::kTimeoutExpired:
      return make(2, [](Reader& r) -> core::EventPayload {
        core::TimeoutExpiredPayload v;
        v.phase = ClosedValue(r, "phase", phase_names, core::TimeoutPhase::kInvalid);
        v.timer_generation = r.U64("timer_generation");
        return v;
      });
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed:
      return make(2, [](Reader& r) -> core::EventPayload {
        core::WorkerEventPayload v;
        v.worker_id = core::Uuid{r.Str("worker_id")};
        v.event_sequence = r.U64("event_sequence");
        return v;
      });
    case EventType::kProcessExitConfirmed:
      return make(2, [](Reader& r) -> core::EventPayload {
        core::ProcessExitConfirmedPayload v;
        v.completion_mode = ClosedValue(r, "completion_mode", completion_mode_names,
                                        core::CompletionMode::kInvalid);
        v.launch_operation_id = core::StableId{r.Str("launch_operation_id")};
        return v;
      });
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::SessionPayload{core::Uuid{r.Str("session_id")}};
      });
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      return make(0, [](Reader&) -> core::EventPayload { return core::EmptyPayload{}; });
    case EventType::kTerminalOutcomeCommitted:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::TerminalOutcomePayload{
            ClosedValue(r, "outcome", outcome_names, core::TerminalOutcome::kInvalid)};
      });
    case EventType::kResourcesReleased:
      return make(2, [](Reader& r) -> core::EventPayload {
        core::ResourcesReleasedPayload v;
        v.allocation_id = core::StableId{r.Str("allocation_id")};
        v.allocation_digest = core::Digest{r.Str("allocation_digest")};
        return v;
      });
    case EventType::kCleanupStatusRecorded:
      return make(1, [](Reader& r) -> core::EventPayload {
        return core::CleanupStatusPayload{
            ClosedValue(r, "status", cleanup_names, core::CleanupStatus::kInvalid)};
      });
    case EventType::kLateWorkerEvent:
      return make(3, [](Reader& r) -> core::EventPayload {
        core::LateWorkerEventPayload v;
        v.original_event_type =
            ClosedValue(r, "original_event_type", late_original_names, core::EventType::kInvalid);
        v.worker_id = core::Uuid{r.Str("worker_id")};
        v.event_sequence = r.U64("event_sequence");
        return v;
      });
    case EventType::kInvalid:
      break;
  }
  error = "event_type is not a closed value";
  return std::nullopt;
}

std::optional<core::LogicalJobEvent> EventFromJson(const Json& json, std::string& error) {
  if (!json.is_object()) {
    error = "record is not an object";
    return std::nullopt;
  }
  Reader r{json, envelope_members, {}};
  core::LogicalJobEvent e;
  e.schema_version = r.U32("schema_version");
  e.sequence = r.U64("sequence");
  const auto type_name = r.Str("event_type");
  e.recorded_at.rfc3339 = r.Str("recorded_at");
  e.job_id.value = r.Str("job_id");
  const auto* payload = r.Get("payload", Json::value_t::object);
  if (!r.CheckMemberCount()) {
    error = r.error;
    return std::nullopt;
  }
  const auto type = EventTypeOf(type_name);
  if (!type) {
    error = "event_type is not a closed value";
    return std::nullopt;
  }
  e.event_type = *type;
  auto decoded_payload = PayloadFromJson(*type, *payload, error);
  if (!decoded_payload) return std::nullopt;
  e.payload = std::move(*decoded_payload);
  return e;
}
}  // namespace

EncodedRecord EncodeRecord(const core::LogicalJobEvent& event) {
  if (auto violation = Violation(event)) {
    return EncodedRecord{EncodeStatus::kSchemaViolation, {}, std::move(*violation)};
  }
  auto bytes = Emit(event);
  if (bytes.size() > max_record_bytes) {
    return EncodedRecord{
        EncodeStatus::kOversize, {}, "record is " + std::to_string(bytes.size()) + " bytes"};
  }
  return EncodedRecord{EncodeStatus::kEncoded, std::move(bytes), {}};
}

DecodedRecord DecodeRecord(std::string_view record) {
  if (record.size() > max_record_bytes) {
    return DecodedRecord{
        DecodeStatus::kOversize, {}, "record is " + std::to_string(record.size()) + " bytes"};
  }
  if (record.empty() || record.back() != '\n') {
    return DecodedRecord{DecodeStatus::kMalformedJson, {}, "record does not end with LF"};
  }
  const auto body = record.substr(0, record.size() - 1);
  if (body.find('\n') != std::string_view::npos) {
    return DecodedRecord{DecodeStatus::kMalformedJson, {}, "record spans more than one line"};
  }
  const Json json = Json::parse(body, nullptr, false);
  if (json.is_discarded()) {
    return DecodedRecord{DecodeStatus::kMalformedJson, {}, "record is not valid JSON"};
  }
  std::string error;
  auto event = EventFromJson(json, error);
  if (!event) return DecodedRecord{DecodeStatus::kSchemaViolation, {}, std::move(error)};
  if (auto violation = Violation(*event)) {
    return DecodedRecord{DecodeStatus::kSchemaViolation, {}, std::move(*violation)};
  }
  if (Emit(*event) != record) {
    return DecodedRecord{
        DecodeStatus::kNonCanonical, {}, "record is not the canonical encoding of its event"};
  }
  return DecodedRecord{DecodeStatus::kDecoded, std::move(*event), {}};
}

}  // namespace sitometron::journal
