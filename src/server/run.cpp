#include "openport/server/run.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <openssl/evp.h>
#include <stdexcept>
#include <unistd.h>

#include "openport/providers/replay_batches.hpp"
#include "run_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
std::string hex_digest(const unsigned char* bytes, unsigned size) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned i = 0; i < size; ++i) {
    result.push_back(hex[bytes[i] >> 4]);
    result.push_back(hex[bytes[i] & 15]);
  }
  return result;
}
std::string hash_text(std::string_view text) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (EVP_Digest(text.data(), text.size(), bytes, &size, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  return hex_digest(bytes, size);
}
std::string hash_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Missing input: " + file.string());
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  std::array<char, 65536> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1)
      throw std::runtime_error("SHA-256 failed");
  }
  if (!input.eof()) throw std::runtime_error("Cannot read input: " + file.string());
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (EVP_DigestFinal_ex(context.get(), bytes, &size) != 1) throw std::runtime_error("SHA-256 failed");
  return hex_digest(bytes, size);
}
struct CalendarScope {
  std::vector<md::ScheduledDay> saved = md::scheduled_days();
  ~CalendarScope() { md::set_scheduled_days(std::move(saved)); }
};
struct TemporaryInput {
  std::filesystem::path directory;
  ~TemporaryInput() { std::error_code error; if (!directory.empty()) std::filesystem::remove_all(directory, error); }
  std::filesystem::path file() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-verify-XXXXXX").string();
    const auto* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("Cannot create verification directory");
    directory = created;
    return directory / "input.oprec";
  }
};
class ComparisonJournal final : public trading::Journal {
 public:
  explicit ComparisonJournal(const trading::JournalRecovery& expected) : expected_(expected) {}
  void append(md::Timestamp time, std::string_view type, std::string_view payload) override {
    json line{{"seq", sequence_ + 1}, {"time", time}, {"type", type}, {"payload", json::parse(payload)}, {"prev_hash", head_}};
    const auto hash = hash_text(line.dump());
    if (sequence_ >= expected_.records.size() || expected_.records[sequence_].hash != hash) {
      error = "First differing transaction " + std::to_string(sequence_ + 1) + " (" + std::string(type) + ")";
      throw std::runtime_error(error);
    }
    head_ = hash;
    ++sequence_;
  }
  std::uint64_t sequence() const override { return sequence_; }
  std::string head() const override { return head_; }
  std::string error;
 private:
  const trading::JournalRecovery& expected_;
  std::uint64_t sequence_ = 0;
  std::string head_ = std::string(64, '0');
};
}

std::string recording_input(const std::filesystem::path& file) {
  const auto hash = hash_file(file);
  md::RecordingReader reader(file);
  return json{{"version", 1}, {"kind", "recording"}, {"file", std::filesystem::absolute(file).string()},
      {"name", file.filename().string()}, {"size", std::filesystem::file_size(file)}, {"sha256", hash},
      {"started", reader.header().started}}.dump();
}
std::string scenario_input(const providers::Scenario& scenario, md::Date date, std::uint64_t seed) {
  return json{{"version", 1}, {"kind", "scenario"}, {"id", scenario.id}, {"file", scenario.source_file.string()},
      {"builtin", scenario.builtin}, {"sha256", hash_text(scenario.source)}, {"generator", scenario.generator},
      {"seed", seed}, {"date", date}, {"started", providers::scenario_open(scenario, date)}}.dump();
}
RunVerification verify_run(const std::filesystem::path& journal) {
  RunVerification result;
  try {
    const auto expected = trading::FileJournal::read(journal.string());
    if (expected.truncated_final_line) throw std::runtime_error("Journal has a torn final line");
    std::vector<json> inputs;
    for (const auto& record : expected.records) {
      if (record.type != "run_input") continue;
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events"))
        if (event.at("type") == "run_input") inputs.push_back(event.at("payload"));
    }
    if (inputs.empty() || inputs.front().at("kind") != "start")
      throw std::runtime_error("Journal has no reproducible-run metadata (older journals still load as accounts)");
    const auto& start = inputs.front();
    const auto& input = start.at("input");
    const CalendarScope calendar;
    md::set_scheduled_days(start.value("calendar", std::vector<md::ScheduledDay>{}));
    if (input.at("version") != 1) throw std::runtime_error("Unsupported run driver version");
    TemporaryInput generated;
    std::filesystem::path file;
    if (input.at("kind") == "recording") {
      file = input.at("file").get<std::string>();
      if (hash_file(file) != input.at("sha256") || std::filesystem::file_size(file) != input.at("size"))
        throw std::runtime_error("Recording input changed: " + file.string());
    } else if (input.at("kind") == "scenario") {
      providers::Scenario scenario;
      if (input.at("builtin").get<bool>()) {
        bool found = false;
        for (const auto& candidate : providers::builtin_scenarios()) {
          if (candidate.id != input.at("id")) continue;
          scenario = candidate;
          found = true;
          break;
        }
        if (!found) throw std::runtime_error("Missing built-in scenario: " + input.at("id").get<std::string>());
      } else {
        file = input.at("file").get<std::string>();
        if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Missing scenario input: " + file.string());
        scenario = providers::read_scenario(file);
      }
      if (hash_text(scenario.source) != input.at("sha256"))
        throw std::runtime_error("Scenario input changed: " + scenario.id);
      if (scenario.generator != input.at("generator")) throw std::runtime_error("Scenario generator version changed");
      file = generated.file();
      providers::write_scenario_recording(file, scenario, input.at("date").get<md::Date>(), input.at("seed").get<std::uint64_t>());
    } else throw std::runtime_error("Unknown run input kind");
    const auto restored = trading::TradingSession::recover(expected);
    md::RecordingReader reader(file);
    const md::Subscription subscription{start.at("symbols").get<std::vector<std::string>>(), 0, 0};
    providers::ReplayBatches batches(reader, subscription);
    auto comparison = std::make_shared<ComparisonJournal>(expected);
    Desk::Options options;
    options.replay = true;
    options.run_input = input.dump();
    options.paper_sink = comparison;
    options.paper = json::parse(expected.records.front().payload).at("state").at("config").get<trading::SessionConfig>();
    options.analytics = start.at("analytics").get<analytics::AnalyticsOptions>();
    options.dividends = start.at("dividends").get<std::vector<trading::Dividend>>();
    Desk desk("replay (" + reader.header().provider + ")", reader.header().capabilities, subscription, options);
    desk.start_trading();
    for (std::size_t index = 1; index < inputs.size() && comparison->error.empty(); ++index) {
      const auto& operation = inputs[index];
      if (operation.at("kind") == "boundary") {
        auto batch = batches.next();
        if (!batch || batch->events.size() != operation.at("events") || batch->received != operation.at("driver_time"))
          throw std::runtime_error("Input boundary differs at operation " + std::to_string(index));
        desk.replay_batch(batch->events, batch->received, batch->time);
      } else if (operation.at("kind") == "command") {
        desk.command(operation.at("command").get<TradingCommand>(), [](TradingReply) {},
                     operation.at("time").get<md::Timestamp>(), operation.at("driver_time").get<md::Timestamp>());
      } else if (operation.at("kind") == "dividends") {
        desk.set_dividends(operation.at("dividends").get<std::vector<trading::Dividend>>());
      } else throw std::runtime_error("Unknown recorded run operation");
    }
    if (!comparison->error.empty()) throw std::runtime_error(comparison->error);
    if (comparison->sequence() != expected.records.size())
      throw std::runtime_error("First differing transaction " + std::to_string(comparison->sequence() + 1) + " (missing output)");
    const auto view = desk.trading_view();
    if (!view || view->snapshot->equity != restored.snapshot()->equity || comparison->head() != expected.head)
      throw std::runtime_error("Final equity or head hash differs");
    result.matched = true;
    result.transactions = comparison->sequence();
    result.head = comparison->head();
    result.equity = view->snapshot->equity;
    result.message = "Verified " + std::to_string(result.transactions) + " transactions; equity " +
        std::to_string(result.equity.micros()) + " micro-dollars; head " + result.head;
  } catch (const std::exception& error) { result.message = error.what(); }
  return result;
}
}  // namespace openport::server
