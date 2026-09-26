#ifndef SITOMETRON_SPIKE_FILE_JOURNAL_HPP_
#define SITOMETRON_SPIKE_FILE_JOURNAL_HPP_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "sitometron/core/job_ports.hpp"

namespace sitometron::spike {

// Append-only JSON Lines Journal. One line per logical event, fsync before the commit result is
// returned, so the writer's commit-before-apply ordering holds against the file.
//
// NOT a contract: Phase 0B owns the real record layout, replay, recovery, and pruning.
class FileJournal final : public core::JobJournalPort {
 public:
  explicit FileJournal(std::string path);
  ~FileJournal() override;
  FileJournal(const FileJournal&) = delete;
  FileJournal& operator=(const FileJournal&) = delete;

  [[nodiscard]] bool Open(std::string& error);
  [[nodiscard]] core::LogicalCommitResult Commit(
      const core::LogicalJobEvent& event) noexcept override;
  [[nodiscard]] std::size_t committed_count() const noexcept;
  [[nodiscard]] std::size_t lines_on_open() const noexcept { return lines_on_open_; }
  // Highest logical sequence found in the existing file (0 when empty). The writer continues
  // from the next value so a restart never reuses a sequence number.
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }

  static nlohmann::json ToJson(const core::LogicalJobEvent& event);

 private:
  std::string path_;
  int fd_ = -1;
  std::size_t lines_on_open_ = 0;
  std::uint64_t last_sequence_ = 0;
  std::size_t committed_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_FILE_JOURNAL_HPP_
