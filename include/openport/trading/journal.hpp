#pragma once

#include <memory>
#include <string>
#include <vector>

#include "openport/trading/types.hpp"

namespace openport::trading {

struct JournalRecord {
  std::uint64_t seq = 0;
  Timestamp time = 0;
  std::string type;
  std::string payload;  ///< Canonical JSON object, sorted keys, compact UTF-8.
  std::string prev_hash;
  std::string hash;
};
struct JournalRecovery {
  std::vector<JournalRecord> records;
  bool truncated_final_line = false;
  std::string head;
};
/// What FileJournal::repair did.
struct JournalRepair {
  std::size_t bytes_cut = 0;  ///< the torn final line's length; 0 when the journal was whole
  std::string backup;         ///< where the original was copied, when a line was cut
};

/// Optional durable sink; all other trading components are filesystem-free.
/// One reducer transaction per newline contains all typed outcomes plus the
/// resulting state. append must persist the complete line or throw JOURNAL_IO.
/// A failure is indeterminate on disk: stop trading and recover before retrying.
class Journal {
 public:
  virtual ~Journal() = default;
  virtual void append(Timestamp time, std::string_view type, std::string_view payload) = 0;
  [[nodiscard]] virtual std::uint64_t sequence() const = 0;
  [[nodiscard]] virtual std::string head() const = 0;
};

/// Exclusive single-writer file, O_EXCL on creation, write + fsync before return.
/// Non-blocking flock excludes other opens (including in this process) until destruction.
/// Resume requires a clean verified file. A torn suffix is never silently erased.
class FileJournal final : public Journal {
 public:
  static std::shared_ptr<FileJournal> create(const std::string& path);
  static std::shared_ptr<FileJournal> resume(const std::string& path);
  static JournalRecovery read(const std::string& path, std::string_view expected_head = {});
  /// Cuts a torn final line, as a write the disk ran out for leaves, off a journal so
  /// that it resumes, after copying the original beside it as FILE.torn-YYYYMMDDTHHMMSSZ.
  /// It takes the writer's lock, so openportd must be stopped. A journal that verifies
  /// is left alone; damage before the last line throws JOURNAL_CORRUPT, changing nothing.
  static JournalRepair repair(const std::string& path);
  ~FileJournal() override;
  FileJournal(const FileJournal&) = delete;
  FileJournal& operator=(const FileJournal&) = delete;
  void append(Timestamp time, std::string_view type, std::string_view payload) override;
  [[nodiscard]] std::uint64_t sequence() const override { return sequence_; }
  [[nodiscard]] std::string head() const override { return head_; }
 private:
  FileJournal(int fd, std::uint64_t sequence, std::string head, Timestamp last_time);
  int fd_ = -1;
  std::uint64_t sequence_ = 0;
  std::string head_;
  bool failed_ = false;
  Timestamp last_time_ = 0;
};

/// Same verifier for in-memory captured JSONL; useful for imported audit files.
[[nodiscard]] JournalRecovery verify_journal(std::string_view jsonl,
                                             std::string_view expected_head = {});

}  // namespace openport::trading
