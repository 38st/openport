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
/// Resume requires a clean verified file. A torn suffix is never silently erased.
class FileJournal final : public Journal {
 public:
  static std::shared_ptr<FileJournal> create(const std::string& path);
  static std::shared_ptr<FileJournal> resume(const std::string& path);
  static JournalRecovery read(const std::string& path, std::string_view expected_head = {});
  ~FileJournal() override;
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
