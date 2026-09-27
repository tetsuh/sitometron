#include "sitometron/journal/segment_journal.hpp"

namespace sitometron::journal {

SegmentJournal::SegmentJournal(FileSystem& file_system, SegmentJournalOptions options)
    : file_system_(file_system), options_(options) {}
SegmentJournal::~SegmentJournal() = default;
OpenResult SegmentJournal::Open(const std::string& /*directory*/) {
  return OpenResult{false, "not implemented", 0, {}};
}
core::LogicalCommitResult SegmentJournal::Commit(const core::LogicalJobEvent& /*event*/) noexcept {
  return core::LogicalCommitResult::kDefiniteFailure;
}
bool SegmentJournal::poisoned() const { return false; }
std::uint64_t SegmentJournal::next_sequence() const { return 0; }
std::string SegmentJournal::SegmentName(std::uint64_t /*first_sequence*/) { return {}; }
core::LogicalCommitResult SegmentJournal::CommitLocked(const core::LogicalJobEvent& /*event*/) {
  return core::LogicalCommitResult::kDefiniteFailure;
}
core::LogicalCommitResult SegmentJournal::StartSegment(std::uint64_t /*first_sequence*/) {
  return core::LogicalCommitResult::kDefiniteFailure;
}
void SegmentJournal::CloseActive() noexcept {}

}  // namespace sitometron::journal
