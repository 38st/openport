#include "openport/trading/journal.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace openport::trading {
namespace {
using Json = nlohmann::json;
const std::string genesis(64, '0');
[[noreturn]] void corrupt(std::string message) { throw TradingError(Reason::JOURNAL_CORRUPT, std::move(message)); }
[[noreturn]] void io(std::string message) { throw TradingError(Reason::JOURNAL_IO, std::move(message)); }
std::string digest(const std::string& input) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_Digest(input.data(), input.size(), bytes, &size, EVP_sha256(), nullptr) != 1)
    io("SHA-256 failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(hex[bytes[i] >> 4]);
    result.push_back(hex[bytes[i] & 15]);
  }
  return result;
}
int open_locked(const std::string& path, bool create) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | (create ? O_CREAT | O_EXCL : 0), 0600);
  if (fd < 0) io("Cannot open journal: " + std::string(std::strerror(errno)));
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    io("Journal must be a regular file");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    io("Journal already has a writer");
  }
  return fd;
}
}  // namespace
JournalRecovery verify_journal(std::string_view jsonl, std::string_view expected_head) {
  JournalRecovery result;
  result.head = genesis;
  std::size_t start = 0;
  try {
    while (start < jsonl.size()) {
      const auto end = jsonl.find('\n', start);
      if (end == std::string_view::npos) { result.truncated_final_line = true; break; }
      const auto line = jsonl.substr(start, end - start);
      Json j = Json::parse(line);
      if (!j.is_object() || j.size() != 6 || !j.at("payload").is_object() ||
          !j.at("seq").is_number_unsigned() || !j.at("time").is_number_integer())
        corrupt("Invalid journal envelope");
      const auto hash = j.at("hash").get<std::string>();
      // Require our canonical serialization, rejecting duplicate keys and whitespace rewrites.
      if (j.dump() != line) corrupt("Noncanonical journal line");
      j.erase("hash");
      const auto seq = j.at("seq").get<std::uint64_t>();
      const auto time = j.at("time").get<Timestamp>();
      if (seq != result.records.size() + 1 || time < 0 ||
          (!result.records.empty() && time < result.records.back().time) ||
          j.at("prev_hash") != result.head || hash != digest(j.dump()))
        corrupt("Broken journal sequence, time or SHA-256 chain at record " + std::to_string(seq));
      result.records.push_back({seq, time, j.at("type").get<std::string>(),
          j.at("payload").dump(), result.head, hash});
      result.head = hash;
      start = end + 1;
    }
    if (!expected_head.empty() && result.head != expected_head) corrupt("Journal does not match trusted head");
  } catch (const Json::exception& e) { corrupt("Invalid journal JSON: " + std::string(e.what())); }
  return result;
}
FileJournal::FileJournal(int fd, std::uint64_t sequence, std::string head, Timestamp last_time)
    : fd_(fd), sequence_(sequence), head_(std::move(head)), last_time_(last_time) {}
FileJournal::~FileJournal() { if (fd_ >= 0) ::close(fd_); }
std::shared_ptr<FileJournal> FileJournal::create(const std::string& path) {
  return std::shared_ptr<FileJournal>(new FileJournal(open_locked(path, true), 0, genesis, 0));
}
JournalRecovery FileJournal::read(const std::string& path, std::string_view expected_head) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) io("Journal must be a readable regular file");
  std::ifstream file(path, std::ios::binary);
  if (!file) io("Cannot read journal");
  std::ostringstream contents;
  contents << file.rdbuf();
  if (file.bad()) io("Journal read failed");
  return verify_journal(contents.str(), expected_head);
}
std::shared_ptr<FileJournal> FileJournal::resume(const std::string& path) {
  const int fd = open_locked(path, false);
  try {
    const auto recovery = read(path);
    if (recovery.truncated_final_line) io("Torn journal suffix: export verified prefix before resuming");
    return std::shared_ptr<FileJournal>(new FileJournal(fd, recovery.records.size(), recovery.head, recovery.records.empty() ? 0 : recovery.records.back().time));
  } catch (...) { ::close(fd); throw; }
}
void FileJournal::append(Timestamp time, std::string_view type, std::string_view payload) {
  if (failed_) io("Journal is latched failed; recover before trading");
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) io("Journal sequence exhausted");
  try {
    Json data = Json::parse(payload);
    if (!data.is_object() || time < last_time_) io("Invalid journal payload/time");
    Json j{{"seq", sequence_ + 1}, {"time", time}, {"type", type}, {"payload", std::move(data)}, {"prev_hash", head_}};
    const std::string hash = digest(j.dump());
    j["hash"] = hash;
    const std::string line = j.dump() + '\n';
    std::size_t done = 0;
    while (done < line.size()) {
      const auto count = ::write(fd_, line.data() + done, line.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) io("Journal write failed: " + std::string(std::strerror(errno)));
      done += static_cast<std::size_t>(count);
    }
    if (::fsync(fd_) != 0) io("Journal fsync failed: " + std::string(std::strerror(errno)));
    ++sequence_;
    head_ = hash;
    last_time_ = time;
  } catch (const TradingError&) { failed_ = true; throw; }
    catch (const std::exception& e) { failed_ = true; io("Journal append failed: " + std::string(e.what())); }
}
}  // namespace openport::trading
