#include "sitometron/journal/record_codec.hpp"

namespace sitometron::journal {

EncodedRecord EncodeRecord(const core::LogicalJobEvent& /*event*/) {
  return EncodedRecord{EncodeStatus::kSchemaViolation, {}, "not implemented"};
}

DecodedRecord DecodeRecord(std::string_view /*record*/) {
  return DecodedRecord{DecodeStatus::kMalformedJson, {}, "not implemented"};
}

}  // namespace sitometron::journal
