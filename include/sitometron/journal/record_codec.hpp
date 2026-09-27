#ifndef SITOMETRON_JOURNAL_RECORD_CODEC_HPP_
#define SITOMETRON_JOURNAL_RECORD_CODEC_HPP_

#include <cstddef>
#include <string>
#include <string_view>

#include "sitometron/core/job_ports.hpp"

// Canonical physical JobJournal record codec (Accepted ADR-0006, Section 1).
//
// One physical record is one logical event encoded as one JSON object followed by exactly one LF.
// The encoder emits the ADR-0002 envelope members in order, payload members in schema order, no
// insignificant whitespace, and only the escapes RFC 8259 requires, with lowercase `\u` hex. The
// decoder accepts exactly the byte sequences the encoder can emit and rejects everything else.
namespace sitometron::journal {

// Upper bound of one record in bytes, including its terminating LF.
inline constexpr std::size_t kMaxRecordBytes = 1048576;

enum class EncodeStatus {
  kEncoded,
  // The event violates job-journal-event.schema.json (event type, payload shape, identifier
  // pattern, enum, size, or UTF-8); no bytes were produced.
  kSchemaViolation,
  // The event is schema-valid but its canonical encoding exceeds kMaxRecordBytes.
  kOversize,
};

struct EncodedRecord {
  EncodeStatus status = EncodeStatus::kSchemaViolation;
  std::string bytes;   // complete record including LF; empty unless status is kEncoded
  std::string detail;  // human-readable reason when status is not kEncoded
};

enum class DecodeStatus {
  kDecoded,
  // The input is longer than kMaxRecordBytes; it was not parsed.
  kOversize,
  // The input is not one JSON object followed by exactly one LF.
  kMalformedJson,
  // The JSON parses but violates the schema (missing member, wrong type, unknown event type,
  // integer not exactly representable as unsigned 64-bit, identifier pattern, enum, size).
  kSchemaViolation,
  // The JSON is schema-valid but is not the canonical encoding of its event (member order,
  // whitespace, escape spelling, duplicate or extra members).
  kNonCanonical,
};

struct DecodedRecord {
  DecodeStatus status = DecodeStatus::kMalformedJson;
  core::LogicalJobEvent event;  // valid only when status is kDecoded
  std::string detail;           // human-readable reason when status is not kDecoded
};

// Encodes one logical event. Never throws for invalid input; allocation failure propagates.
[[nodiscard]] EncodedRecord EncodeRecord(const core::LogicalJobEvent& event);

// Decodes one complete record (including its LF). Never throws for invalid input.
[[nodiscard]] DecodedRecord DecodeRecord(std::string_view record);

}  // namespace sitometron::journal

#endif  // SITOMETRON_JOURNAL_RECORD_CODEC_HPP_
