#include "sitometron/journal/replay.hpp"

namespace sitometron::journal {

std::optional<core::Snapshot> ReplayRecord(const std::optional<core::Snapshot>& /*before*/,
                                           const core::LogicalJobEvent& /*record*/,
                                           std::string& error) {
  error = "not implemented";
  return std::nullopt;
}

bool IsUnresolved(const core::Snapshot& /*snapshot*/) noexcept { return true; }

ReplayResult ReplayJournal(FileSystem& /*file_system*/, const std::string& /*directory*/,
                           const ReplayOptions& /*options*/) {
  return ReplayResult{ReplayStatus::kCorrupt, "not implemented", 0, 0, {}, {}};
}

}  // namespace sitometron::journal
