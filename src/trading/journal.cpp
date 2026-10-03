#include "openport/trading/journal.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
/// A record must survive power loss, such as a laptop's battery running out. On macOS
/// fsync leaves data in the drive's cache; F_FULLFSYNC asks the drive to write it.
bool full_sync(int fd) {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}
/// A write the disk runs out for would tear the journal, so appends stop short of it.
constexpr unsigned long long kMinimumFreeBytes = 64ull * 1024 * 1024;
std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) io("Cannot read journal");
  std::ostringstream contents;
  contents << file.rdbuf();
  if (file.bad()) io("Journal read failed");
  return contents.str();
}
int open_locked(const std::string& path, bool create) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0), 0600);
  if (fd < 0 && create && errno == EEXIST) {
    // Preserve create's no-overwrite contract, but report contention consistently,
    // including a concurrent creator winning the Engine's existence-check race.
    const int existing = open_locked(path, false);
    ::close(existing);
    io("Cannot create journal '" + path + "': file already exists");
  }
  if (fd < 0) io("Cannot open journal: " + std::string(std::strerror(errno)));
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    io("Journal must be a regular file");
  }
  // flock belongs to this open file description, unlike process-owned fcntl locks:
  // another open in this process conflicts, and closing recovery reads cannot unlock it.
  int result;
  do { result = ::flock(fd, LOCK_EX | LOCK_NB); } while (result != 0 && errno == EINTR);
  if (result != 0) {
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK || error == EAGAIN)
      throw TradingError(Reason::JOURNAL_LOCKED, "paper journal '" + path +
          "' is in use by another openportd; use --paper-journal to choose another file or --no-paper");
    io("Cannot lock paper journal '" + path + "': " + std::strerror(error));
  }
  // The lock's holder may have removed an empty journal between this open and the
  // lock: writing to the file the path no longer names would lose every record.
  struct stat named {};
  if (::stat(path.c_str(), &named) != 0 || named.st_dev != info.st_dev || named.st_ino != info.st_ino) {
    ::close(fd);
    io("Journal '" + path + "' was removed or replaced while opening it");
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
      if (end == std::string_view::npos) { result.truncated_final_line = true; result.bytes_cut = jsonl.size() - start; break; }
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
          j.at("payload").dump(), result.head, hash, j.at("payload").value("actor", std::string("unknown"))});
      result.head = hash;
      start = end + 1;
    }
    if (!expected_head.empty() && result.head != expected_head) corrupt("Journal does not match trusted head");
  } catch (const Json::exception& e) { corrupt("Invalid journal JSON: " + std::string(e.what())); }
  return result;
}
FileJournal::FileJournal(int fd, std::uint64_t sequence, std::string head, Timestamp last_time, Options options)
    : fd_(fd), sequence_(sequence), head_(std::move(head)), last_time_(last_time), options_(std::move(options)) {}
FileJournal::~FileJournal() {
  try { flush(); } catch (...) {}  // Explicit boundaries report failures; destruction cannot throw.
  if (fd_ >= 0) ::close(fd_);
}
std::shared_ptr<FileJournal> FileJournal::create(const std::string& path) {
  return create(path, Options{});
}
std::shared_ptr<FileJournal> FileJournal::create(const std::string& path, Options options) {
  return std::shared_ptr<FileJournal>(new FileJournal(open_locked(path, true), 0, genesis, 0, std::move(options)));
}
JournalRecovery FileJournal::read(const std::string& path, std::string_view expected_head) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) io("Journal must be a readable regular file");
  return verify_journal(read_file(path), expected_head);
}
std::shared_ptr<FileJournal> FileJournal::resume(const std::string& path) {
  return resume(path, Options{});
}
std::shared_ptr<FileJournal> FileJournal::resume(const std::string& path, Options options) {
  const int fd = open_locked(path, false);
  try {
    const auto recovery = read(path);
    if (recovery.truncated_final_line)
      io("Torn journal suffix, as a full disk leaves: with openportd stopped, "
         "openportd --repair-journals cuts it off and keeps the original");
    return std::shared_ptr<FileJournal>(new FileJournal(fd, recovery.records.size(), recovery.head,
        recovery.records.empty() ? 0 : recovery.records.back().time, std::move(options)));
  } catch (...) { ::close(fd); throw; }
}
JournalRepair FileJournal::repair(const std::string& path) {
  const int fd = open_locked(path, false);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  const auto contents = read_file(path);
  if (!verify_journal(contents).truncated_final_line) return {0, {}, contents.empty()};
  // A torn first record, the journal's only content, leaves an empty journal: it
  // held no transaction, so its account starts afresh.
  const auto end = contents.rfind('\n');
  const std::size_t keep = end == std::string::npos ? 0 : end + 1;
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  ::gmtime_r(&now, &utc);
  std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc);
  JournalRepair result{contents.size() - keep, path + ".torn-" + stamp, keep == 0};
  const int copy = ::open(result.backup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (copy < 0) io("Cannot create " + result.backup + ": " + std::strerror(errno));
  std::size_t done = 0;
  while (done < contents.size()) {
    const auto count = ::write(copy, contents.data() + done, contents.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ::close(copy); io("Cannot write " + result.backup + ": " + std::strerror(errno)); }
    done += static_cast<std::size_t>(count);
  }
  const bool copied = full_sync(copy);
  ::close(copy);
  if (!copied) io("Cannot sync " + result.backup);
  if (::ftruncate(fd, static_cast<off_t>(keep)) != 0 || !full_sync(fd))
    io("Cannot cut the torn line off " + path + ": " + std::strerror(errno));
  return result;
}
void FileJournal::remove(const std::string& path) {
  const int fd = open_locked(path, false);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  // Nothing is read: removing a damaged journal needs no verified chain.
  if (::unlink(path.c_str()) != 0) io("Cannot remove journal: " + std::string(std::strerror(errno)));
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
    struct statvfs space {};
    if (::fstatvfs(fd_, &space) == 0 &&
        static_cast<unsigned long long>(space.f_bavail) * space.f_frsize < kMinimumFreeBytes + line.size())
      io("Disk nearly full: the journal stops before a write could tear it; free space and restart");
    std::size_t done = 0;
    while (done < line.size()) {
      const auto count = ::write(fd_, line.data() + done, line.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) io("Journal write failed: " + std::string(std::strerror(errno)));
      done += static_cast<std::size_t>(count);
    }
    pending_ = true;
    if (options_.sync_policy == SyncPolicy::PerRecord || !synced_ ||
        options_.hooks.clock() - last_sync_ >= options_.sync_interval) flush();
    ++sequence_;
    head_ = hash;
    last_time_ = time;
  } catch (const TradingError&) { failed_ = true; throw; }
    catch (const std::exception& e) { failed_ = true; io("Journal append failed: " + std::string(e.what())); }
}
void FileJournal::flush() {
  if (failed_) io("Journal is latched failed; recover before trading");
  if (!pending_) return;
  try {
    if (!(options_.hooks.sync ? options_.hooks.sync(fd_) : full_sync(fd_)))
      io("Journal sync failed: " + std::string(std::strerror(errno)));
    last_sync_ = options_.hooks.clock();
    synced_ = true;
    pending_ = false;
  } catch (const TradingError&) { failed_ = true; throw; }
    catch (const std::exception& e) { failed_ = true; io("Journal sync failed: " + std::string(e.what())); }
    catch (...) { failed_ = true; io("Journal sync failed"); }
}
}  // namespace openport::trading
