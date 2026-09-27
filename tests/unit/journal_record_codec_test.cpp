#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "sitometron/core/job_ports.hpp"
#include "sitometron/journal/record_codec.hpp"

namespace sitometron::test {
namespace {
using Json = nlohmann::json;
using namespace sitometron::core;
using namespace sitometron::journal;
using namespace std::string_view_literals;

int Check(bool condition, const std::string& message) {
  if (condition) return 0;
  std::cerr << "journal_record_codec: " << message << '\n';
  return 1;
}

Uuid U(const Json& j) { return Uuid{j.get<std::string>()}; }
StableId S(const Json& j) { return StableId{j.get<std::string>()}; }
Digest D(const Json& j) { return Digest{j.get<std::string>()}; }

template <typename Enum, std::size_t N>
Enum Closed(const std::string& name, const std::array<std::pair<std::string_view, Enum>, N>& values,
            Enum fallback) {
  for (const auto& [text, value] : values) {
    if (text == name) return value;
  }
  return fallback;
}

EventType EventTypeOf(const std::string& name) {
  for (int i = 0; i < static_cast<int>(EventType::kInvalid); ++i) {
    if (ToString(static_cast<EventType>(i)) == name) return static_cast<EventType>(i);
  }
  return EventType::kInvalid;
}

EventPayload PayloadFrom(EventType type, const Json& p) {
  switch (type) {
    case EventType::kJobCreated:
      return JobCreatedPayload{U(p.at("session_id"))};
    case EventType::kResourcesCommitted: {
      const auto& r = p.at("resolved_allocation");
      return ResourcesCommittedPayload{
          S(p.at("allocation_id")), D(p.at("allocation_digest")), S(r.at("schema_id")),
          r.at("schema_version").get<std::uint32_t>(), r.at("payload_utf8").get<std::string>()};
    }
    case EventType::kWorkerLaunchIntent: {
      const auto& a = p.at("application");
      return WorkerLaunchIntentPayload{
          S(p.at("operation_id")),  S(a.at("application_id")), a.at("version").get<std::string>(),
          D(a.at("bundle_sha256")), S(p.at("allocation_id")),  D(p.at("allocation_digest")),
          U(p.at("worker_id"))};
    }
    case EventType::kWorkerLaunchObserved:
      return WorkerLaunchObservedPayload{S(p.at("operation_id")), p.at("outcome") == "started"};
    case EventType::kWorkerRunning:
      return WorkerRunningPayload{U(p.at("worker_id"))};
    case EventType::kCancelAccepted:
    case EventType::kTerminateAccepted:
      return PrincipalPayload{p.at("principal_subject").get<std::string>()};
    case EventType::kTimeoutExpired: {
      static constexpr std::array phases{
          std::pair{"preparation"sv, TimeoutPhase::kPreparation},
          std::pair{"execution"sv, TimeoutPhase::kExecution},
          std::pair{"cooperative_stop"sv, TimeoutPhase::kCooperativeStop},
          std::pair{"process_exit_confirmation"sv, TimeoutPhase::kProcessExitConfirmation}};
      return TimeoutExpiredPayload{
          Closed(p.at("phase").get<std::string>(), phases, TimeoutPhase::kInvalid),
          p.at("timer_generation").get<std::uint64_t>()};
    }
    case EventType::kWorkerCompleted:
    case EventType::kWorkerFailed:
      return WorkerEventPayload{U(p.at("worker_id")), p.at("event_sequence").get<std::uint64_t>()};
    case EventType::kProcessExitConfirmed: {
      static constexpr std::array modes{
          std::pair{"cooperative"sv, CompletionMode::kCooperative},
          std::pair{"forced"sv, CompletionMode::kForced},
          std::pair{"process_already_exited"sv, CompletionMode::kProcessAlreadyExited}};
      return ProcessExitConfirmedPayload{
          Closed(p.at("completion_mode").get<std::string>(), modes, CompletionMode::kInvalid),
          S(p.at("launch_operation_id"))};
    }
    case EventType::kSessionRetainRequested:
    case EventType::kSessionRetained:
      return SessionPayload{U(p.at("session_id"))};
    case EventType::kFinalizationCompleted:
    case EventType::kFinalizationFailed:
      return EmptyPayload{};
    case EventType::kTerminalOutcomeCommitted: {
      static constexpr std::array outcomes{std::pair{"succeeded"sv, TerminalOutcome::kSucceeded},
                                           std::pair{"failed"sv, TerminalOutcome::kFailed},
                                           std::pair{"cancelled"sv, TerminalOutcome::kCancelled},
                                           std::pair{"terminated"sv, TerminalOutcome::kTerminated},
                                           std::pair{"timed_out"sv, TerminalOutcome::kTimedOut}};
      return TerminalOutcomePayload{
          Closed(p.at("outcome").get<std::string>(), outcomes, TerminalOutcome::kInvalid)};
    }
    case EventType::kResourcesReleased:
      return ResourcesReleasedPayload{S(p.at("allocation_id")), D(p.at("allocation_digest"))};
    case EventType::kCleanupStatusRecorded:
      return CleanupStatusPayload{p.at("status") == "completed" ? CleanupStatus::kCompleted
                                                                : CleanupStatus::kIncomplete};
    case EventType::kLateWorkerEvent:
      return LateWorkerEventPayload{EventTypeOf(p.at("original_event_type").get<std::string>()),
                                    U(p.at("worker_id")),
                                    p.at("event_sequence").get<std::uint64_t>()};
    case EventType::kInvalid:
      break;
  }
  return EmptyPayload{};
}

LogicalJobEvent EventFrom(const Json& j) {
  const auto type = EventTypeOf(j.at("event_type").get<std::string>());
  return LogicalJobEvent{j.at("schema_version").get<std::uint32_t>(),
                         j.at("sequence").get<std::uint64_t>(),
                         type,
                         DiagnosticTimestamp{j.at("recorded_at").get<std::string>()},
                         U(j.at("job_id")),
                         PayloadFrom(type, j.at("payload"))};
}

LogicalJobEvent SampleEvent() {
  return LogicalJobEvent{1,
                         7,
                         EventType::kCancelAccepted,
                         DiagnosticTimestamp{"2026-09-27T01:02:03.456Z"},
                         Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
                         PrincipalPayload{"operator@example"}};
}

const std::string sample_record =
    R"({"schema_version":1,"sequence":7,"event_type":"cancel_accepted",)"
    R"("recorded_at":"2026-09-27T01:02:03.456Z","job_id":"01890f3e-7b00-7abc-8abc-0123456789ab",)"
    R"("payload":{"principal_subject":"operator@example"}})"
    "\n";

int RoundtripVectors(const Json& vectors) {
  int result = 0;
  std::set<std::string> seen;
  std::size_t count = 0;
  for (const auto& vector : vectors.at("case_vectors")) {
    const auto& fixture = vector.at("expected").at("journal_event");
    if (fixture.is_null()) continue;
    ++count;
    const auto id = vector.at("vector_id").get<std::string>();
    seen.insert(fixture.at("event_type").get<std::string>());
    const auto encoded = EncodeRecord(EventFrom(fixture));
    result |= Check(encoded.status == EncodeStatus::kEncoded, id + " encodes: " + encoded.detail);
    if (encoded.status != EncodeStatus::kEncoded) continue;
    result |= Check(!encoded.bytes.empty() && encoded.bytes.back() == '\n' &&
                        encoded.bytes.find('\n') == encoded.bytes.size() - 1,
                    id + " one line with one LF");
    result |= Check(Json::parse(encoded.bytes) == fixture,
                    id + " encoded object equals fixture: " + encoded.bytes);
    const auto decoded = DecodeRecord(encoded.bytes);
    result |= Check(decoded.status == DecodeStatus::kDecoded, id + " decodes: " + decoded.detail);
    if (decoded.status != DecodeStatus::kDecoded) continue;
    const auto again = EncodeRecord(decoded.event);
    result |= Check(again.status == EncodeStatus::kEncoded && again.bytes == encoded.bytes,
                    id + " decode then encode reproduces the bytes");
  }
  result |= Check(count > 0, "fixtures contain journal events");
  result |= Check(seen.size() == 19, "fixtures cover every closed event kind (saw " +
                                         std::to_string(seen.size()) + ")");
  return result;
}

int SizeBound() {
  int result = 0;
  auto event = SampleEvent();
  const auto base = EncodeRecord(event);
  result |= Check(base.status == EncodeStatus::kEncoded, "sample encodes: " + base.detail);
  result |= Check(base.bytes == sample_record, "sample is canonical: " + base.bytes);
  // Grow the fractional-seconds part so that the record lands exactly on the bound.
  const auto room = max_record_bytes - base.bytes.size();
  event.recorded_at.rfc3339 = "2026-09-27T01:02:03." + std::string(3 + room, '4') + "Z";
  const auto exact = EncodeRecord(event);
  result |= Check(exact.status == EncodeStatus::kEncoded && exact.bytes.size() == max_record_bytes,
                  "record exactly at the bound encodes");
  result |= Check(DecodeRecord(exact.bytes).status == DecodeStatus::kDecoded,
                  "record exactly at the bound decodes");
  event.recorded_at.rfc3339.insert(event.recorded_at.rfc3339.size() - 1, "4");
  const auto over = EncodeRecord(event);
  result |= Check(over.status == EncodeStatus::kOversize && over.bytes.empty(),
                  "record one byte over the bound is rejected without bytes");
  result |= Check(DecodeRecord(exact.bytes + "\n").status == DecodeStatus::kOversize,
                  "decoder rejects input over the bound before parsing");
  // The largest allowed allocation text still fits.
  LogicalJobEvent committed{
      1,
      1,
      EventType::kResourcesCommitted,
      DiagnosticTimestamp{"2026-09-27T01:02:03Z"},
      Uuid{"01890f3e-7b00-7abc-8abc-0123456789ab"},
      ResourcesCommittedPayload{StableId{"allocation-1"}, Digest{std::string(64, 'a')},
                                StableId{"schema.v1"}, 1, "[" + std::string(65534, '1') + "]"}};
  result |= Check(EncodeRecord(committed).status == EncodeStatus::kEncoded,
                  "65,536-byte allocation text encodes");
  std::get<ResourcesCommittedPayload>(committed.payload).payload_utf8.push_back('1');
  result |= Check(EncodeRecord(committed).status == EncodeStatus::kSchemaViolation,
                  "65,537-byte allocation text is a schema violation");
  return result;
}

int OversizeSchemaValidRejected() {
  int result = 0;
  auto event = SampleEvent();
  event.recorded_at.rfc3339 = "2026-09-27T01:02:03." + std::string(max_record_bytes, '9') + "Z";
  const auto encoded = EncodeRecord(event);
  result |= Check(encoded.status == EncodeStatus::kOversize, "schema-valid oversize is kOversize");
  result |= Check(encoded.bytes.empty(), "no bytes for an oversize record");
  return result;
}

int RejectsNoncanonical() {
  int result = 0;
  const auto ok = DecodeRecord(sample_record);
  result |= Check(ok.status == DecodeStatus::kDecoded, "canonical sample decodes: " + ok.detail);
  struct Case {
    std::string_view name;
    std::string record;
    DecodeStatus expected;
  };
  const std::string body = sample_record.substr(0, sample_record.size() - 1);
  struct Mutation {
    std::string_view from;
    std::string_view to;
  };
  auto replace = [&](Mutation m) {
    std::string copy = sample_record;
    const auto at = copy.find(m.from);
    if (at != std::string::npos) copy.replace(at, m.from.size(), m.to);
    return copy;
  };
  const std::array cases{
      Case{"missing LF", body, DecodeStatus::kMalformedJson},
      Case{"doubled LF", sample_record + "\n", DecodeStatus::kMalformedJson},
      Case{"CRLF", body + "\r\n", DecodeStatus::kNonCanonical},
      Case{"trailing space", body + " \n", DecodeStatus::kNonCanonical},
      Case{"leading space", " " + sample_record, DecodeStatus::kNonCanonical},
      Case{"not an object", "[]\n", DecodeStatus::kSchemaViolation},
      Case{"truncated", body.substr(0, body.size() - 3) + "\n", DecodeStatus::kMalformedJson},
      Case{"reordered members",
           replace({R"("schema_version":1,"sequence":7)", R"("sequence":7,"schema_version":1)"}),
           DecodeStatus::kNonCanonical},
      Case{"inner whitespace", replace({R"("sequence":7)", R"("sequence": 7)"}),
           DecodeStatus::kNonCanonical},
      Case{"extra member", replace({R"("sequence":7)", R"("sequence":7,"note":"x")"}),
           DecodeStatus::kSchemaViolation},
      Case{"duplicate member", replace({R"("sequence":7)", R"("sequence":7,"sequence":7)"}),
           DecodeStatus::kNonCanonical},
      Case{"escaped solidus", replace({"operator@example", R"(operator\/example)"}),
           DecodeStatus::kNonCanonical},
      Case{"needless unicode escape", replace({"operator@example", "operator\\u0040example"}),
           DecodeStatus::kNonCanonical},
      Case{"float sequence", replace({R"("sequence":7)", R"("sequence":7.0)"}),
           DecodeStatus::kSchemaViolation},
      Case{"negative sequence", replace({R"("sequence":7)", R"("sequence":-7)"}),
           DecodeStatus::kSchemaViolation},
      Case{"zero sequence", replace({R"("sequence":7)", R"("sequence":0)"}),
           DecodeStatus::kSchemaViolation},
      Case{"sequence beyond uint64",
           replace({R"("sequence":7)", R"("sequence":18446744073709551616)"}),
           DecodeStatus::kSchemaViolation},
      Case{"string sequence", replace({R"("sequence":7)", R"("sequence":"7")"}),
           DecodeStatus::kSchemaViolation},
      Case{"schema version 2", replace({R"("schema_version":1)", R"("schema_version":2)"}),
           DecodeStatus::kSchemaViolation},
      Case{"unknown event type", replace({"cancel_accepted", "cancel_requested"}),
           DecodeStatus::kSchemaViolation},
      Case{"payload for another event type", replace({"cancel_accepted", "worker_running"}),
           DecodeStatus::kSchemaViolation},
      Case{"missing payload member", replace({R"({"principal_subject":"operator@example"})", "{}"}),
           DecodeStatus::kSchemaViolation},
      Case{"empty principal", replace({"operator@example", ""}), DecodeStatus::kSchemaViolation},
      Case{
          "job id not v7",
          replace({"01890f3e-7b00-7abc-8abc-0123456789ab", "01890f3e-7b00-4abc-8abc-0123456789ab"}),
          DecodeStatus::kSchemaViolation},
      Case{"year zero", replace({"2026-09-27", "0000-09-27"}), DecodeStatus::kSchemaViolation},
      Case{"leap second", replace({"01:02:03.456Z", "23:59:60.456Z"}),
           DecodeStatus::kSchemaViolation},
      Case{"hour 24", replace({"01:02:03.456Z", "24:00:00.456Z"}), DecodeStatus::kSchemaViolation},
      Case{"offset hour 24", replace({"03.456Z", "03.456+24:00"}), DecodeStatus::kSchemaViolation},
      Case{"timestamp without zone", replace({"03.456Z", "03.456"}),
           DecodeStatus::kSchemaViolation},
      Case{"calendar-invalid date", replace({"2026-09-27", "2026-02-30"}),
           DecodeStatus::kSchemaViolation},
      Case{"BOM prefix", "\xEF\xBB\xBF" + sample_record, DecodeStatus::kNonCanonical},
      Case{"leading zero integer", replace({R"("sequence":7)", R"("sequence":07)"}),
           DecodeStatus::kMalformedJson},
      Case{"lone surrogate escape", replace({"operator@example", "operator\\ud800example"}),
           DecodeStatus::kMalformedJson},
      Case{"invalid UTF-8", replace({"operator@example", "op\xC3(rator"}),
           DecodeStatus::kMalformedJson},
  };
  for (const auto& c : cases) {
    const auto decoded = DecodeRecord(c.record);
    result |= Check(decoded.status == c.expected,
                    std::string(c.name) + ": expected status " +
                        std::to_string(static_cast<int>(c.expected)) + ", got " +
                        std::to_string(static_cast<int>(decoded.status)) + " " + decoded.detail);
  }
  // Timestamp spellings the schema accepts round-trip unchanged.
  for (const auto accepted : {"2026-09-27t01:02:03.456z"sv, "2024-02-29T23:59:59+09:00"sv,
                              "0001-01-01T00:00:00Z"sv, "9999-12-31T23:59:59.999999999-23:59"sv}) {
    auto event = SampleEvent();
    event.recorded_at.rfc3339 = std::string(accepted);
    const auto encoded = EncodeRecord(event);
    result |= Check(encoded.status == EncodeStatus::kEncoded &&
                        DecodeRecord(encoded.bytes).status == DecodeStatus::kDecoded,
                    "schema-valid timestamp round-trips: " + std::string(accepted));
  }
  for (const auto rejected : {"0000-01-01T00:00:00Z"sv, "2026-09-27T23:59:60Z"sv,
                              "2025-02-29T00:00:00Z"sv, "2026-09-27T01:02:03.Z"sv}) {
    auto event = SampleEvent();
    event.recorded_at.rfc3339 = std::string(rejected);
    result |= Check(EncodeRecord(event).status == EncodeStatus::kSchemaViolation,
                    "schema-invalid timestamp rejected before encoding: " + std::string(rejected));
  }
  // The sequence maximum is representable exactly.
  const auto max = DecodeRecord(replace({R"("sequence":7)", R"("sequence":18446744073709551615)"}));
  result |= Check(max.status == DecodeStatus::kDecoded && max.event.sequence == UINT64_MAX,
                  "uint64 maximum sequence decodes exactly");
  // Control characters are escaped with lowercase hex and round-trip.
  auto control = SampleEvent();
  control.payload = PrincipalPayload{std::string("a\x1f") + "b\"c\\d/e\xC3\xA9"};
  const auto encoded = EncodeRecord(control);
  result |= Check(
      encoded.status == EncodeStatus::kEncoded && encoded.bytes.find(R"("a\u001fb\"c\\d/e)"
                                                                     "\xC3\xA9"
                                                                     R"(")") != std::string::npos,
      "escapes are RFC 8259 minimum with lowercase hex: " + encoded.bytes);
  const auto back = DecodeRecord(encoded.bytes);
  result |= Check(back.status == DecodeStatus::kDecoded &&
                      std::get<PrincipalPayload>(back.event.payload).principal_subject ==
                          std::get<PrincipalPayload>(control.payload).principal_subject,
                  "escaped principal round-trips");
  // Encoder-side schema violations produce no bytes.
  auto bad = SampleEvent();
  bad.payload = WorkerRunningPayload{Uuid{"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"}};
  const auto mismatch = EncodeRecord(bad);
  result |= Check(mismatch.status == EncodeStatus::kSchemaViolation && mismatch.bytes.empty(),
                  "payload/event type mismatch is rejected before encoding");
  bad = SampleEvent();
  bad.payload = PrincipalPayload{"op\xC3(rator"};
  result |= Check(EncodeRecord(bad).status == EncodeStatus::kSchemaViolation,
                  "invalid UTF-8 is rejected before encoding");
  bad = SampleEvent();
  bad.event_type = EventType::kInvalid;
  result |= Check(EncodeRecord(bad).status == EncodeStatus::kSchemaViolation,
                  "invalid event type is rejected before encoding");
  return result;
}
}  // namespace
}  // namespace sitometron::test

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::cerr << "usage: journal_record_codec_test <job-reducer-vectors.json> <check>\n";
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "cannot open " << argv[1] << '\n';
    return 2;
  }
  const auto vectors = nlohmann::json::parse(input);
  const std::string_view check = argv[2];
  using namespace sitometron::test;
  if (check == "journal_record_roundtrip_vectors") return RoundtripVectors(vectors);
  if (check == "journal_record_size_bound") return SizeBound();
  if (check == "journal_record_oversize_schema_valid_rejected")
    return OversizeSchemaValidRejected();
  if (check == "journal_parser_rejects_noncanonical") return RejectsNoncanonical();
  std::cerr << "unknown check " << check << '\n';
  return 2;
} catch (const std::exception& error) {
  std::cerr << "journal_record_codec: unexpected exception: " << error.what() << '\n';
  return 1;
} catch (...) {
  std::cerr << "journal_record_codec: unexpected non-standard exception\n";
  return 1;
}
