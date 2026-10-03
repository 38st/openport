#include "openport/server/replay_host.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <fstream>
#include <charconv>
#include <cstdio>
#include <random>
#include <map>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "openport/providers/demo.hpp"
#include "openport/providers/replay.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/run.hpp"
#include "openport/server/playbooks.hpp"
#include "run_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;

/// A plain file name in the recordings directory: no directories, no hidden files.
bool plain_name(std::string_view name) {
  return !name.empty() && name.size() <= 255 && name.front() != '.' &&
         name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

json header_json(const md::RecordingHeader& header) {
  return {{"provider", header.provider}, {"imported", header.imported}, {"simulated", providers::simulated_provider(header.provider)}, {"symbols", header.subscription.underlyings},
          {"started", header.started > 0 ? json(md::format_timestamp(header.started)) : json(nullptr)},
          {"delay_seconds", header.capabilities.delay.count()}};
}

json recordings_json(const std::filesystem::path& directory) {
  json list = json::array();
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec)) return list;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    if (entry.is_regular_file(ec) && plain_name(entry.path().filename().string())) files.push_back(entry.path());
  // Recordings are named by when they started: newest first.
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });
  for (const auto& file : files) {
    json item{{"file", file.filename().string()}, {"bytes", std::filesystem::file_size(file, ec)}};
    try {
      const md::RecordingReader reader(file);
      item.update(header_json(reader.header()));
    } catch (const std::exception& error) {
      item["error"] = error.what();
    }
    list.push_back(std::move(item));
  }
  return list;
}

ApiResponse ok(const json& body, int status = 200) { return {status, body.dump()}; }

json parse_body(const ApiRequest& request, std::initializer_list<std::string_view> allowed) {
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = request.body.empty() ? json::object() : json::parse(request.body);
  if (!body.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto it = body.begin(); it != body.end(); ++it)
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
      throw std::invalid_argument("Unknown field: " + it.key());
  return body;
}

/// Each session a scenario run plays: its kind, trading date, first and last snapshot.
json sessions_json(const std::vector<providers::ScenarioWindow>& windows) {
  json out = json::array();
  for (const auto& w : windows)
    out.push_back({{"session", w.session}, {"date", md::format_date(w.date)},
                   {"open", md::format_timestamp(w.first)}, {"end", md::format_timestamp(w.last)}});
  return out;
}
json demo_json(const providers::Scenario& d) {
  const auto windows = providers::scenario_windows(d, d.date);
  return {{"id", d.id}, {"title", d.title}, {"description", d.description}, {"goal", d.goal},
          {"session", d.overnight ? "overnight" : "regular"}, {"date", md::format_date(d.date)},
          {"seed", std::to_string(d.seed)}, {"generator", d.generator}, {"provider", providers::kDemoProvider},
          {"symbols", d.symbols}, {"started", md::format_timestamp(providers::scenario_open(d, d.date))},
          {"sessions", sessions_json(windows)}, {"end", md::format_timestamp(windows.back().last)}};
}
const providers::Scenario& default_scenario(const std::vector<providers::Scenario>& scenarios) {
  const auto found = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "reversal"; });
  return found == scenarios.end() ? scenarios.front() : *found;
}
json demos_json(const std::vector<providers::Scenario>& scenarios) {
  json list = json::array();
  for (const auto& d : scenarios) list.push_back(demo_json(d));
  return list;
}
std::uint64_t seed_field(const json& value, const providers::Scenario& scenario) {
  if (value == "scenario") return scenario.seed;
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    std::uint64_t seed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (error == std::errc{} && end == text.data() + text.size()) return seed;
  }
  if (value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0))
    return value.get<std::uint64_t>();
  throw std::invalid_argument("seed must be a uint64, its decimal string, or scenario");
}
std::uint64_t fresh_seed() {
  std::random_device random;
  return (static_cast<std::uint64_t>(random()) << 32) ^ random();
}
std::string slug(std::string_view source) {
  std::string out;
  for (const char c : source) {
    if (out.size() == 40) break;
    out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
  }
  return out.empty() ? "recording" : out;
}

/// A date and time, "YYYY-MM-DDTHH:MM[:SS]" (or with a space) in New York unless it
/// carries Z or a UTC offset; empty for a bare "HH:MM[:SS]".
std::optional<md::Timestamp> dated_time(std::string value, const char* field) {
  if (value.size() <= 8) return std::nullopt;
  // Seconds are optional: 2026-09-17T10:30 is 10:30:00.
  if (value.size() >= 16 && value[13] == ':' && (value.size() == 16 || value[16] != ':')) value.insert(16, ":00");
  const auto parsed = md::parse_datetime(value, md::Zone::NewYork);
  if (!parsed) throw std::invalid_argument(std::string(field) + " must be New York HH:MM, or a date and time such as 2026-09-17T10:30");
  return parsed;
}
/// A bare New York "HH:MM[:SS]" in a multi-session run: its first occurrence after
/// `after` inside one of the run's sessions.
md::Timestamp session_time(const std::string& value, const std::vector<providers::ScenarioWindow>& windows,
                           md::Timestamp after, const char* field) {
  const auto parsed = md::parse_datetime("2000-01-03T" + value + (value.size() == 5 ? ":00" : ""), md::Zone::Utc);
  if ((value.size() != 5 && value.size() != 8) || !parsed)
    throw std::invalid_argument(std::string(field) + " must be New York HH:MM, or a date and time such as 2026-09-17T10:30");
  const auto seconds = static_cast<int>(*parsed % md::kNanosPerDay / md::kNanosPerSecond);
  for (const auto& w : windows) {
    // A session spans at most two New York dates.
    for (const auto day : {md::new_york_time(w.first).date, md::new_york_time(w.last).date}) {
      const auto candidate = md::new_york_to_utc(day, seconds / 3600, seconds / 60 % 60, seconds % 60);
      if (candidate != md::kInvalidTimestamp && candidate > after && candidate >= w.first && candidate <= w.last) return candidate;
    }
  }
  throw std::invalid_argument(std::string(field) + " " + value + " falls in none of this run's sessions after " +
                              md::format_timestamp(after) + "; give a date and time");
}

void replay_gate(json& message, const providers::ReplayProvider& provider) {
  const bool preparing = provider.fast_forwarding(), stepping = provider.stepping(), finished = provider.finished();
  if (!preparing && !stepping && !finished) return;
  const auto* reason = preparing ? "REPLAY_FAST_FORWARD" : stepping ? "REPLAY_STEPPING" : "REPLAY_READ_ONLY";
  const auto gate = [&](json& trading) {
    if (trading.is_object() && trading.value("enabled", false)) {
      trading["write"] = "disabled";
      trading["reason"] = reason;
    }
  };
  if (message.contains("trading")) gate(message["trading"]);
  if (message.contains("accounts")) for (auto& account : message["accounts"]) gate(account["trading"]);
  if (message.contains("underlyings")) for (auto& underlying : message["underlyings"]) {
    if (!underlying.contains("paper") || !underlying["paper"].is_object()) continue;
    underlying["paper"]["accepting"] = false;
    underlying["paper"]["reason"] = reason;
    underlying["paper"]["message"] = preparing ? "Preparing replay start state" :
        stepping ? "Advancing to the until time" : "Finished replay is read-only";
  }
}

/// A history id that names no saved run: answered 404, never with a file path.
struct UnknownRun : std::runtime_error {
  explicit UnknownRun(const std::string& id) : std::runtime_error("No saved replay run " + id) {}
};

/// A plan's id from its display name, for runs whose metadata sidecar is missing.
std::string plan_id(const std::string& name) {
  for (const auto& plan : plan_presets()) if (plan.name == name) return plan.id;
  return name;
}

/// A saved run plays no further, so an order it left working or armed can never fill or
/// cancel: its archive shows it cancelled RUN_ENDED. The journal keeps the order as it was.
std::shared_ptr<const trading::TradingSnapshot> end_open_orders(std::shared_ptr<const trading::TradingSnapshot> snapshot) {
  if (snapshot->open_orders.empty()) return snapshot;
  auto ended = std::make_shared<trading::TradingSnapshot>(*snapshot);
  const trading::Decision reason{trading::Reason::RUN_ENDED, "The replay run ended with this order still open", {}, {}, {}};
  for (std::size_t i = 0; i < ended->recent_orders.size(); ++i) {
    if (!ended->recent_orders[i].open()) continue;
    auto& order = ended->recent_orders.mut(i);
    order.status = trading::OrderStatus::Cancelled;
    order.reason = reason;
  }
  ended->open_orders.clear();
  return ended;
}

}  // namespace

/// A small cache of generated (scenario, date, seed) recordings.
class ReplayHost::DemoRecordings {
 public:
  DemoRecordings() = default;
  DemoRecordings(const DemoRecordings&) = delete;
  DemoRecordings& operator=(const DemoRecordings&) = delete;
  ~DemoRecordings() {
    std::map<std::string, Attempt> days;
    {
      const std::lock_guard lock(mutex_);
      days.swap(days_);
    }
    for (auto& [day, attempt] : days) attempt.file.wait();
    std::error_code ec;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, ec);
  }
  /// Starts generating the day in the background unless it is ready or under way.
  void prepare(const providers::Scenario& day) {
    { const std::lock_guard lock(mutex_); if (!days_.empty()) return; }
    (void)start(day, day.date, day.seed);
  }
  /// The day's recording: ready, awaited, or generated now. A failure is retried next time.
  std::filesystem::path get(const providers::Scenario& day, md::Date date, std::uint64_t seed) {
    const auto key = day.id + "|" + md::format_date(date) + "|" + std::to_string(seed);
    const auto attempt = start(day, date, seed);
    try {
      return attempt.file.get();
    } catch (...) {
      const std::lock_guard lock(mutex_);
      if (const auto it = days_.find(key); it != days_.end() && it->second.number == attempt.number) days_.erase(it);
      throw;
    }
  }

 private:
  struct Attempt {
    unsigned number = 0;
    std::shared_future<std::filesystem::path> file;
  };
  Attempt start(const providers::Scenario& day, md::Date date, std::uint64_t seed) {
    const auto key = day.id + "|" + md::format_date(date) + "|" + std::to_string(seed);
    const std::lock_guard lock(mutex_);
    if (const auto it = days_.find(key); it != days_.end()) return it->second;
    if (directory_.empty()) {
      static std::atomic<unsigned> count{0};
      auto directory = std::filesystem::temp_directory_path() /
                       ("openport-demo-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
      std::filesystem::remove_all(directory);
      std::filesystem::create_directory(directory);
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
      directory_ = std::move(directory);
    }
    // Each attempt writes its own file, so a retry never meets a failed one's.
    const unsigned number = ++attempts_;
    const auto path = directory_ / (std::to_string(number) + ".oprec");
    Attempt attempt{number, std::async(std::launch::async, [path, day, date, seed] {
                              providers::write_scenario_recording(path, day, date, seed);
                              return path;
                            }).share()};
    days_.emplace(key, attempt);
    // Open replay readers retain their file descriptor after eviction (POSIX).
    // Pending generators are never removed while they are writing.
    while (days_.size() > 4) {
      auto oldest = days_.end();
      for (auto it = days_.begin(); it != days_.end(); ++it)
        if (it->first != key && it->second.file.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            (oldest == days_.end() || it->second.number < oldest->second.number)) oldest = it;
      if (oldest == days_.end()) break;
      try { std::filesystem::remove(oldest->second.file.get()); } catch (const std::exception&) {}
      days_.erase(oldest);
    }
    return attempt;
  }

  std::mutex mutex_;
  std::filesystem::path directory_;
  unsigned attempts_ = 0;
  std::map<std::string, Attempt> days_;
};

namespace {
int speed_field(const json& body) {
  const auto& value = body.at("speed");
  if (!value.is_number_integer() || value < 0 || value > 300 || !providers::ReplayProvider::valid_speed(value.get<int>()))
    throw std::invalid_argument("speed must be 0 (as fast as possible), 1, 2, 5, 10, 30, 60, 120 or 300");
  return value.get<int>();
}
}  // namespace

struct ReplayHost::Session {
  std::string file;
  bool demo = false;
  std::string id, scenario, seed, date, start_at, plan;
  int generator = 0;
  md::Timestamp target = 0;
  bool durable = false;
  /// A scenario run's sessions; empty for a recording.
  std::vector<providers::ScenarioWindow> windows;
  // The engine reads the provider, so it is declared after it and stops first.
  std::unique_ptr<providers::ReplayProvider> provider;
  std::unique_ptr<Engine> engine;

  [[nodiscard]] json state() const {
    const auto time = provider->time();
    json out = header_json(provider->header());
    out["file"] = file;
    out["demo"] = demo;
    out["id"] = id;
    out["scenario"] = scenario.empty() ? json(nullptr) : json(scenario);
    out["seed"] = seed.empty() ? json(nullptr) : json(seed);
    out["generator"] = generator == 0 ? json(nullptr) : json(generator);
    out["date"] = date;
    out["start_at"] = start_at;
    out["plan"] = plan;
    out["durable"] = durable;
    out["fast_forwarding"] = provider->fast_forwarding();
    out["stepping"] = provider->stepping();
    out["progress"] = target > provider->header().started ? std::clamp(
        static_cast<double>(time - provider->header().started) / static_cast<double>(target - provider->header().started), 0.0, 1.0) : 1.0;
    out["speed"] = provider->speed();
    out["paused"] = provider->paused();
    out["finished"] = provider->finished();
    const auto settled = provider->settled_through();
    out["settled_through"] = settled > 0 ? json(md::format_timestamp(settled)) : json(nullptr);
    out["time"] = time > 0 ? json(md::format_timestamp(time)) : json(nullptr);
    out["sessions"] = sessions_json(windows);
    out["end"] = windows.empty() ? json(nullptr) : json(md::format_timestamp(windows.back().last));
    return out;
  }
};

/// Read-only recovered accounts reuse all of the existing account and trade routes.
class ArchivedReplay final : public MetricsSource {
 public:
  explicit ArchivedReplay(const std::filesystem::path& file) {
    const auto recovery = trading::FileJournal::read(file.string());
    const auto session = trading::TradingSession::recover(recovery);
    view_ = std::make_shared<TradingView>();
    view_->snapshot = end_open_orders(session.snapshot());
    view_->config = session.config();
    view_->contracts = session.contracts();
    view_->valuations = session.valuations();
    // The equity history beside the journal, read as it is: an archive never compacts it.
    view_->equity_samples = read_equity_history(file.string() + ".equity.csv", view_->equity_error);
    // Replay definitions are journaled as inputs. Archives do not depend on the
    // current live catalogue or a mutable sidecar to explain historical trades.
    std::unique_ptr<Playbooks> playbooks;
    for (const auto& record : recovery.records) {
      if (record.type != "run_input") continue;
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events")) {
        if (event.at("type") != "run_input") continue;
        const auto& input = event.at("payload");
        if (input.at("kind") == "start") {
          playbooks = std::make_unique<Playbooks>(std::filesystem::path{}, input.value("playbooks", json(nullptr)));
          view_->run = run_identity(input.at("input").dump(), file.stem().string());
          view_->opening_settlement = input.value("driver", 1) >= 5;
        } else if (input.at("kind") == "command" && playbooks) {
          const auto& request = input.at("command");
          if (request.at("kind").get<int>() != static_cast<int>(TradingCommand::Kind::Playbook)) continue;
          const auto change = json::parse(request.at("note").get<std::string>());
          if (change.at("action") == "send" || change.at("action") == "dismiss") continue;
          // Invalid user commands are also recorded; leave the last valid catalogue.
          try { playbooks->change(change, "main", true); } catch (const std::exception&) {}
        }
      }
    }
    if (playbooks) view_->playbooks_json = playbooks->publication("main", false).dump();
    std::ifstream metadata(std::filesystem::path(file).replace_extension(".json"));
    if (metadata) simulated_ = json::parse(metadata).value("demo", false);
  }
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  using MetricsSource::trading_view;  // the account-id overload answers for the main account
  std::shared_ptr<const TradingView> trading_view() const override { return view_; }
  md::Timestamp wall_time() const override { return view_->snapshot->time; }
  EngineStatus status() const override {
    EngineStatus out;
    out.provider = simulated_ ? "replay (demo)" : "replay archive";
    out.trading.enabled = true;
    out.trading.reason = "REPLAY_READ_ONLY";
    out.trading.write = "disabled";
    out.trading.account_version = view_->snapshot->account_version;
    out.trading.initial_cash = view_->config.initial_cash;
    out.trading.fee_per_contract = view_->config.fee_per_contract;
    out.trading.plan = view_->config.rules.plan;
    out.accounts.push_back({"main", "Replay (read-only)", out.trading});
    return out;
  }
 private:
  std::shared_ptr<TradingView> view_;
  bool simulated_ = false;
};

class ReplayHost::History {
 public:
  explicit History(const Engine::Options& options)
      : directory_(options.paper_journal.empty() ? std::filesystem::path{} :
          std::filesystem::absolute(options.paper_journal).parent_path() / "replays"),
        writable_(options.paper_enabled && options.write_mode != "disabled" && !directory_.empty()) {}
  bool writable() const { return writable_; }
  std::filesystem::path create(const Session& session, const trading::SessionConfig&) {
    if (!writable_) return {};
    std::filesystem::create_directories(directory_);
    const auto path = directory_ / (session.id + ".jsonl");
    // The journal's exclusive create also prevents overwriting an earlier run.
    if (std::filesystem::exists(path)) throw std::runtime_error("Replay journal already exists");
    std::ofstream metadata(directory_ / (session.id + ".json"));
    metadata << session.state().dump() << '\n';
    metadata.close();
    if (!metadata) throw std::runtime_error("Cannot write replay metadata for " + session.id);
    return path;
  }
  /// Rewrites a retired run's metadata with its final playback state, which then
  /// reads finished. A crash leaves the start state that create wrote.
  void finish(const Session& session) const {
    if (!writable_ || !session.durable || session.id.empty()) return;
    const auto file = directory_ / (session.id + ".json");
    const auto staged = directory_ / (session.id + ".json.tmp");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return;
    {
      std::ofstream metadata(staged);
      metadata << session.state().dump() << '\n';
      metadata.close();
      if (!metadata) { std::filesystem::remove(staged, ec); return; }
    }
    std::filesystem::rename(staged, file, ec);
    if (ec) std::filesystem::remove(staged, ec);
  }
  /// A saved run's metadata as its last start or retirement wrote it, or null.
  json metadata(const std::string& id) const {
    if (directory_.empty() || !plain_name(id)) return nullptr;
    std::ifstream file(directory_ / (id + ".json"));
    if (!file) return nullptr;
    return json::parse(file, nullptr, false);
  }
  /// Removes what a resumed run rebuilds as it re-executes: its equity history and
  /// playbook catalogue, which would otherwise hold the later state it reaches again.
  void rebuild(const std::string& id) const {
    std::error_code ignored;
    for (const auto* suffix : {".playbooks.json", ".jsonl.equity.csv"}) std::filesystem::remove(directory_ / (id + suffix), ignored);
  }
  /// A saved run's journal, or empty when there is none by that id.
  std::filesystem::path journal(const std::string& id) const {
    if (directory_.empty() || !plain_name(id)) return {};
    auto path = directory_ / (id + ".jsonl");
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path{};
  }
  /// Removes what create and a run that failed to start left, so it lists no entry.
  void discard(const std::string& id) const {
    if (!writable_) return;
    std::error_code ignored;
    for (const auto* suffix : {".jsonl", ".json", ".playbooks.json", ".jsonl.equity.csv"})
      std::filesystem::remove(directory_ / (id + suffix), ignored);
  }
  std::shared_ptr<ArchivedReplay> open(const std::string& id) const {
    const auto path = journal(id);
    if (path.empty()) throw UnknownRun(id);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    const auto bytes = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) throw UnknownRun(id);
    const std::lock_guard lock(cache_mutex_);
    const auto found = cache_.find(id);
    if (found != cache_.end() && found->second.modified == modified && found->second.bytes == bytes) return found->second.account;
    auto account = recover(path);
    summaries_[id] = {modified, bytes, summarize(account->trading_view())};
    if (cache_.size() >= 16) cache_.erase(cache_.begin());
    cache_[id] = {modified, bytes, account};
    return account;
  }
  json list(const std::shared_ptr<Session>& active) const {
    json out = json::array();
    std::error_code ec;
    if (directory_.empty() || !std::filesystem::is_directory(directory_, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") files.push_back(entry.path());
    std::sort(files.begin(), files.end(), std::greater<>());
    for (const auto& file : files) {
      const auto id = file.stem().string();
      const bool current = active && active->id == id;
      if (current && !active->provider->finished()) continue;
      json item{{"id", id}, {"file", id}, {"demo", false}, {"result", "open"}, {"pnl", nullptr}};
      bool finalized = current;
      try {
        if (current) {
          item.update(active->state());
        } else {
          std::ifstream metadata(std::filesystem::path(file).replace_extension(".json"));
          if (metadata) item.update(json::parse(metadata));
          finalized = item.value("finished", false);
        }
        item.update(current ? summarize(active->engine->trading_view()) : summary(id, file));
      } catch (const std::exception& error) {
        if (!std::filesystem::exists(file, ec)) continue;  // deleted while listing
        item["error"] = error.what();
      }
      if (!item.contains("plan") && item.contains("plan_name")) item["plan"] = plan_id(item.at("plan_name").get<std::string>());
      // Metadata a crash left still holds the start state: the run is over, and it
      // settled through its last journaled time.
      if (!finalized && item.contains("fast_forwarding")) {
        item["fast_forwarding"] = false;
        if (item.contains("stepping")) item["stepping"] = false;
        item["progress"] = 1.0;
        item["paused"] = true;
        item["settled_through"] = item.value("time", json(nullptr));
      }
      // A run a crash interrupted, whose metadata still holds its start state, can
      // continue: POST /api/replay {"resume"}. A stopped or replaced run has ended.
      item["interrupted"] = !finalized && !item.contains("error");
      item["finished"] = true;
      item["read_only"] = true;
      out.push_back(std::move(item));
    }
    return out;
  }
  void remove(const std::string& id) const {
    if (!writable_) throw std::invalid_argument("Replay history is read-only");
    const auto file = journal(id);
    if (file.empty()) throw UnknownRun(id);
    // The writer's lock keeps another process's running replay; nothing is verified,
    // so an edited or torn journal is deleted too.
    trading::FileJournal::remove(file.string());
    std::error_code ec;
    for (const char* sidecar : {".json", ".playbooks.json", ".jsonl.equity.csv"})
      std::filesystem::remove(directory_ / (id + sidecar), ec);
    const std::lock_guard lock(cache_mutex_);
    cache_.erase(id);
    summaries_.erase(id);
  }
  std::uint64_t recoveries() const {
    const std::lock_guard lock(cache_mutex_);
    return recoveries_;
  }
 private:
  static json summarize(const std::shared_ptr<const TradingView>& view) {
    if (!view) throw std::runtime_error("Replay account unavailable");
    const auto result = view->snapshot->evaluation.status;
    return {{"result", result == trading::EvaluationStatus::Passed ? "pass" : result == trading::EvaluationStatus::Failed ? "fail" : "open"},
            {"pnl", (view->snapshot->equity - view->config.initial_cash).str()},
            {"valuation_complete", view->snapshot->valuation_complete}, {"plan_name", view->config.rules.plan},
            {"time", md::format_timestamp(view->snapshot->time)}};
  }
  std::shared_ptr<ArchivedReplay> recover(const std::filesystem::path& path) const {
    ++recoveries_;  // Caller holds cache_mutex_. Count attempts, including damaged journals.
    return std::make_shared<ArchivedReplay>(path);
  }
  json summary(const std::string& id, const std::filesystem::path& path) const {
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    const auto bytes = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) throw UnknownRun(id);
    const std::lock_guard lock(cache_mutex_);
    const auto found = summaries_.find(id);
    if (found != summaries_.end() && found->second.modified == modified && found->second.bytes == bytes) return found->second.value;
    json value;
    try {
      const auto cached = cache_.find(id);
      const auto account = cached != cache_.end() && cached->second.modified == modified && cached->second.bytes == bytes
          ? cached->second.account : recover(path);
      value = summarize(account->trading_view());
    } catch (const std::exception& error) { value = {{"error", error.what()}}; }
    summaries_[id] = {modified, bytes, value};
    return value;
  }
  struct Cached {
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes;
    std::shared_ptr<ArchivedReplay> account;
  };
  struct Summary {
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes;
    json value;
  };
  mutable std::mutex cache_mutex_;
  mutable std::map<std::string, Cached> cache_;
  mutable std::map<std::string, Summary> summaries_;
  mutable std::uint64_t recoveries_ = 0;
  std::filesystem::path directory_;
  bool writable_;
};

ReplayHost::ReplayHost(Options options)
    : options_(std::move(options)), demos_(std::make_unique<DemoRecordings>()),
      scenarios_(providers::load_scenarios(options_.scenario_dir,
          [](const auto& error) { std::fprintf(stderr, "scenario skipped: %s\n", error.c_str()); })),
      history_(std::make_unique<History>(options_.engine)) {
  worker_ = std::thread([this] { work(); });
}
ReplayHost::~ReplayHost() {
  std::deque<Job> left;
  {
    const std::lock_guard lock(jobs_mutex_);
    closing_ = true;
    left.swap(jobs_);
  }
  jobs_ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (auto& job : left) job.complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
  stop();
}

void ReplayHost::enqueue(const ApiRequest& request, const ApiCompletion& complete) {
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!closing_) {
      jobs_.push_back({request, complete});
      jobs_ready_.notify_one();
      return;
    }
  }
  complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
}

void ReplayHost::work() {
  while (true) {
    Job job;
    {
      std::unique_lock lock(jobs_mutex_);
      jobs_ready_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
      if (closing_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    const std::lock_guard control_lock(control_mutex_);
    if (std::string_view(job.request.target).starts_with("/api/replay/history/")) history(job.request, job.complete);
    else control(job.request, job.complete);
  }
}

std::uint64_t ReplayHost::history_recoveries() const { return history_->recoveries(); }

std::shared_ptr<ReplayHost::Session> ReplayHost::current() const {
  const std::lock_guard lock(mutex_);
  return session_;
}

void ReplayHost::set_dividends(std::vector<trading::Dividend> dividends) {
  const std::lock_guard lock(mutex_);
  options_.engine.dividends = std::move(dividends);
}

void ReplayHost::stop() {
  const std::lock_guard control_lock(control_mutex_);
  stop_session();
}

void ReplayHost::stop_session() {
  // A retiring journal becomes history only after Engine::stop has flushed it.
  const std::lock_guard handoff(handoff_mutex_);
  std::shared_ptr<Session> old;
  {
    const std::lock_guard lock(mutex_);
    old = std::move(session_);
  }
  // Drain callbacks while this thread still owns the session; its last reference
  // must not be released by a completion running on the engine thread.
  if (old) {
    old->engine->stop();
    history_->finish(*old);
  }
  old.reset();
}

bool ReplayHost::handle(const ApiRequest& request, const ApiCompletion& complete) {
  constexpr std::string_view prefix = "/api/replay";
  const std::string_view target = request.target;
  if (!target.starts_with(prefix)) return false;
  const auto rest = target.substr(prefix.size());
  if (!rest.empty() && rest.front() != '?' && rest.front() != '/') return false;
  const bool controls = rest.empty() || rest.front() == '?';
  const bool saved = rest.starts_with("/history/");
  // Changes wait their turn on the control thread; a step there never holds this one.
  if ((controls && request.method != "GET") || (saved && request.method == "DELETE")) {
    enqueue(request, complete);
    return true;
  }
  if (controls) {
    control(request, complete);
    return true;
  }
  if (saved) {
    history(request, complete);
    return true;
  }
  const auto session = current();
  if (!session) {
    complete(api_error(404, "NO_REPLAY", "No replay is running; start one at /api/replay"));
    return true;
  }
  if (request.method != "GET" && session->provider->fast_forwarding()) {
    complete(api_error(409, "REPLAY_FAST_FORWARD", "Wait until the replay reaches start_at before trading"));
    return true;
  }
  // A command during a step would land at whatever market time the step had reached.
  if (request.method != "GET" && session->provider->stepping()) {
    complete(api_error(409, "REPLAY_STEPPING", "Wait for the until step to settle; commands after it use the paused market time"));
    return true;
  }
  if (request.method != "GET" && session->provider->finished()) {
    complete(api_error(403, "REPLAY_READ_ONLY", "Finished replay accounts are read-only"));
    return true;
  }
  if (request.method != "GET" && options_.engine.write_mode == "disabled") {
    complete(api_error(403, "WRITE_DISABLED", "Replay writes are disabled"));
    return true;
  }
  ApiRequest forwarded = request;
  forwarded.target = "/api" + std::string(rest);
  handle_api_async(forwarded, *session->engine, [session, complete, status = rest == "/status"](ApiResponse response) {
    if (status && response.status == 200) {
      auto message = json::parse(response.body);
      replay_gate(message, *session->provider);
      response.body = message.dump();
    }
    complete(std::move(response));
  });
  return true;
}

void ReplayHost::history(const ApiRequest& request, const ApiCompletion& complete) {
  try {
    const auto route = std::string_view(request.target).substr(std::string_view("/api/replay/history/").size());
    const auto slash = route.find('/');
    const std::string id(route.substr(0, slash));
    auto session = current();
    if (session && session->id == id && !session->provider->finished()) {
      complete(api_error(409, "REPLAY_RUNNING", "Stop the replay before opening or deleting its history"));
      return;
    }
    const auto unknown = [&] { complete(api_error(404, "NOT_FOUND", "No saved replay run " + id)); };
    if (request.method == "DELETE" && slash == std::string_view::npos) {  // on the control thread
      if (history_->journal(id).empty()) return unknown();
      if (options_.engine.write_mode == "disabled" || !history_->writable()) {
        complete(api_error(403, "WRITE_DISABLED", "Replay history is read-only"));
        return;
      }
      if (session && session->id == id) stop_session();
      session.reset();
      const std::lock_guard handoff(handoff_mutex_);
      history_->remove(id);
      complete(ok({{"deleted", id}}));
      return;
    }
    const std::lock_guard handoff(handoff_mutex_);
    if (history_->journal(id).empty()) return unknown();
    if (request.method != "GET") {
      complete(api_error(403, "REPLAY_READ_ONLY", "Finished replay accounts are read-only"));
      return;
    }
    const auto archived = history_->open(id);
    ApiRequest forwarded = request;
    forwarded.target = slash == std::string_view::npos ? "/api/account" : "/api" + std::string(route.substr(slash));
    handle_api_async(forwarded, *archived, [archived, complete](ApiResponse response) { complete(std::move(response)); });
  } catch (const UnknownRun& error) {
    complete(api_error(404, "NOT_FOUND", error.what()));
  } catch (const trading::TradingError& error) {
    if (error.code() == trading::Reason::JOURNAL_LOCKED)
      complete(api_error(409, "REPLAY_RUNNING", "Another openportd is still writing this run"));
    else complete(api_error(422, "REPLAY_HISTORY_FAILED", error.what()));
  } catch (const std::filesystem::filesystem_error& error) {
    // Its what() names absolute paths; the code's message is enough.
    complete(api_error(422, "REPLAY_HISTORY_FAILED", error.code().message()));
  } catch (const std::exception& error) { complete(api_error(422, "REPLAY_HISTORY_FAILED", error.what())); }
}

void ReplayHost::resume(const std::string& id, int speed, bool paused, const ApiCompletion& complete) {
  const auto refuse = [&](const std::string& message) { complete(api_error(409, "REPLAY_NOT_RESUMABLE", message)); };
  try {
    if (const auto running = current(); running && running->id == id) {
      if (!running->provider->finished()) complete(api_error(409, "REPLAY_RUNNING", "This run is already playing"));
      else refuse("The run has ended: it played to its end or was stopped");
      return;
    }
    const auto file = history_->journal(id);
    if (file.empty()) {
      complete(api_error(404, "NOT_FOUND", "No saved replay run " + id));
      return;
    }
    if (!history_->writable()) {
      complete(api_error(403, "WRITE_DISABLED", "Replay history is read-only"));
      return;
    }
    const auto metadata = history_->metadata(id);
    if (!metadata.is_object()) return refuse("The run's metadata is missing or unreadable");
    if (metadata.value("finished", false)) return refuse("The run has ended: it played to its end or was stopped");
    auto recovery = trading::FileJournal::read(file.string());
    // A crash part way through a write leaves a torn last line; the repair keeps a copy.
    if (recovery.truncated_final_line) {
      (void)trading::FileJournal::repair(file.string());
      recovery = trading::FileJournal::read(file.string());
    }
    if (recovery.records.empty()) return refuse("The run's journal holds no transaction");
    std::vector<json> inputs;
    for (const auto& input : run_inputs(recovery)) inputs.push_back(json::parse(input));
    if (inputs.empty() || inputs.front().at("kind") != "start") return refuse("The journal has no reproducible-run metadata");
    const auto& start = inputs.front();
    // The run re-executes on this build's driver; one recorded on an older driver would differ.
    if (start.value("driver", 1) != 5) return refuse("The run was recorded by an older build's driver; verify it with --verify-run");
    md::Timestamp target = 0;
    for (const auto& input : inputs) {
      if (input.at("kind") == "source") return refuse("A run that changed its recording part way cannot resume yet");
      if (input.at("kind") == "boundary") target = input.at("driver_time").get<md::Timestamp>();
    }
    if (start.value("calendar", json::array()) != json(md::scheduled_days()))
      return refuse("The exchange calendar has changed since the run started");
    const auto& input = start.at("input");
    const providers::Scenario* day = nullptr;
    std::filesystem::path recording;
    md::Date date;
    std::uint64_t seed = 0;
    if (input.at("kind") == "scenario") {
      if (!options_.demo) {
        complete(api_error(404, "NOT_FOUND", "The demo market is off on this server"));
        return;
      }
      for (const auto& scenario : scenarios_) if (scenario.id == input.at("id").get<std::string>()) day = &scenario;
      if (!day) return refuse("The run's scenario is no longer listed");
      date = input.at("date").get<md::Date>();
      seed = input.at("seed").get<std::uint64_t>();
      if (json::parse(scenario_input(*day, date, seed)) != input) return refuse("The scenario or its generator has changed since the run started");
    } else {
      recording = input.at("file").get<std::string>();
      std::error_code ec;
      if (!std::filesystem::is_regular_file(recording, ec) || json::parse(recording_input(recording)) != input)
        return refuse("The run's recording has changed or moved");
    }
    auto session = std::make_shared<Session>();
    session->id = id;
    session->file = metadata.value("file", id);
    session->scenario = day ? day->id : "";
    session->seed = day ? std::to_string(seed) : "";
    session->generator = day ? day->generator : 0;
    session->plan = metadata.value("plan", std::string("practice"));
    session->date = metadata.value("date", std::string());
    session->start_at = metadata.value("start_at", std::string());
    session->target = target;
    if (day) session->windows = providers::scenario_windows(*day, date);
    providers::ReplayProvider::Options playback;
    playback.file = day ? demos_->get(*day, date, seed) : recording;
    playback.speed = speed;
    // The recorded batches replay unpaced, and playback continues from the last one.
    playback.start_at = target;
    playback.paused = paused;
    for (const auto& window : session->windows) if (session->windows.size() > 1) playback.max_gap = std::max(playback.max_gap, window.step);
    session->provider = std::make_unique<providers::ReplayProvider>(std::move(playback));
    session->demo = providers::simulated_provider(session->provider->header().provider);
    session->durable = true;
    auto engine = [&] {
      const std::lock_guard lock(mutex_);
      return options_.engine;
    }();
    const auto first = json::parse(recovery.records.front().payload);
    // Every option the run's start recorded comes from the run, not from this server's flags.
    engine.paper = first.at("state").at("config").get<trading::SessionConfig>();
    engine.initial_actor = first.value("actor", std::string("system"));
    engine.initial_playbooks = start.contains("playbooks") && !start.at("playbooks").is_null() ? start.at("playbooks").dump() : "";
    engine.analytics = start.at("analytics").get<analytics::AnalyticsOptions>();
    engine.dividends = start.at("dividends").get<std::vector<trading::Dividend>>();
    engine.resume = std::make_shared<const trading::JournalRecovery>(std::move(recovery));
    std::unique_lock handoff(handoff_mutex_);
    history_->rebuild(id);
    engine.paper_journal = file;
    engine.replay = true;
    engine.run_input = input.dump();
    engine.run_id = id;
    engine.paper_accounts.clear();
    engine.paper_sink.reset();
    engine.record_file.clear();
    engine.candles = std::make_shared<CandleStore>();
    auto* provider = session->provider.get();
    engine.clock = [provider] { return provider->time(); };
    const auto& header = provider->header();
    session->engine = std::make_unique<Engine>(*provider, md::Subscription{header.subscription.underlyings, 0, 0.0}, engine);
    session->engine->start();
    if (!session->engine->status().trading.enabled) {
      // The journal is as it was: nothing appends until every recorded transaction matched.
      const auto reason = session->engine->status().trading.reason;
      session->engine->stop();
      return refuse(reason);
    }
    std::shared_ptr<Session> old;
    {
      const std::lock_guard lock(mutex_);
      old = std::exchange(session_, session);
    }
    if (old) {
      old->engine->stop();
      history_->finish(*old);
    }
    // Its metadata now reads as a run playing again, as a crash would leave it.
    history_->finish(*session);
    handoff.unlock();
    old.reset();
    complete(ok({{"replay", session->state()}}, 201));
  } catch (const trading::TradingError& error) {
    if (error.code() == trading::Reason::JOURNAL_LOCKED) complete(api_error(409, "REPLAY_RUNNING", "Another openportd is still writing this run"));
    else refuse(error.what());
  } catch (const std::exception& error) { refuse(error.what()); }
}

void ReplayHost::control(const ApiRequest& request, const ApiCompletion& complete) {
  if (request.target != "/api/replay") {
    complete(api_error(400, "INVALID_REQUEST", "/api/replay takes no query parameters"));
    return;
  }
  try {
    if (request.method == "GET") {
      // Whoever lists the demo may start it next: have the default day ready.
      if (options_.demo) demos_->prepare(default_scenario(scenarios_));
      const std::lock_guard handoff(handoff_mutex_);
      const auto session = current();
      complete(ok({{"write", options_.engine.write_mode}, {"directory", options_.recordings.string()}, {"recordings", recordings_json(options_.recordings)},
                   {"demo", options_.demo ? demo_json(default_scenario(scenarios_)) : json(nullptr)},
                   {"demos", options_.demo ? demos_json(scenarios_) : json::array()},
                   {"replay", session ? session->state() : json(nullptr)}, {"history", history_->list(session)}}));
    } else if (options_.engine.write_mode == "disabled") {
      complete(api_error(403, "WRITE_DISABLED", "Replay writes are disabled"));
    } else if (request.method == "POST") {
      const auto body = parse_body(request, {"file", "demo", "scenario", "speed", "plan", "seed", "date", "start_at", "paused", "resume"});
      if (body.contains("resume")) {
        for (const auto& [key, value] : body.items())
          if (key != "resume" && key != "speed" && key != "paused") throw std::invalid_argument("resume takes only speed and paused besides it");
        if (!body.at("resume").is_string()) throw std::invalid_argument("resume must be a saved run's id");
        if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
        resume(body.at("resume").get<std::string>(), body.contains("speed") ? speed_field(body) : 1,
               body.value("paused", true), complete);
        return;
      }
      // demo: true plays the default day; a day's id plays that one.
      const providers::Scenario* day = nullptr;
      if (body.contains("demo") && body.contains("scenario")) throw std::invalid_argument("Give demo or scenario, not both");
      if (body.contains("demo") || body.contains("scenario")) {
        const auto& value = body.at(body.contains("scenario") ? "scenario" : "demo");
        if (value.is_boolean()) {
          if (value.get<bool>()) day = &default_scenario(scenarios_);
        } else if (value.is_string()) {
          for (const auto& scenario : scenarios_) if (scenario.id == value.get<std::string>()) day = &scenario;
          if (!day) throw std::invalid_argument("Unknown scenario id; see GET /api/replay");
        } else {
          throw std::invalid_argument("demo must be true or a demo day's id");
        }
      }
      const bool demo = day != nullptr;
      if (demo == body.contains("file")) throw std::invalid_argument("Give either file, a recording's name, or demo: true");
      if (demo && !options_.demo) {
        complete(api_error(404, "NOT_FOUND", "The demo market is off on this server"));
        return;
      }
      std::string name = demo ? "Demo market: " + day->title : std::string();
      std::filesystem::path path;
      if (!demo) {
        if (!body.at("file").is_string()) throw std::invalid_argument("file must be a recording's name");
        name = body.at("file").get<std::string>();
        path = options_.recordings / name;
        std::error_code ec;
        if (options_.recordings.empty() || !plain_name(name) || !std::filesystem::is_regular_file(path, ec)) {
          complete(api_error(404, "NOT_FOUND", "No recording " + name + " in the recordings directory"));
          return;
        }
      }
      const int speed = body.contains("speed") ? speed_field(body) : 1;
      if (body.contains("plan") && !body.at("plan").is_string()) throw std::invalid_argument("plan must be a plan id");
      const auto* plan = find_plan(body.contains("plan") && body.at("plan").is_string() ? body.at("plan").get<std::string>() : "practice");
      if (!plan || !plan->unlocked_by.empty()) throw std::invalid_argument("plan must be an evaluation or practice plan; see GET /api/plans");
      if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
      const bool paused = body.value("paused", false);
      if (!demo && (body.contains("seed") || body.contains("date"))) throw std::invalid_argument("seed and date apply only to scenarios");
      auto date = demo ? day->date : md::Date{};
      if (demo && body.contains("date")) {
        const auto value = body.at("date").get<std::string>();
        const auto parsed = md::parse_datetime(value + "T12:00:00", md::Zone::NewYork);
        if (value.size() != 10 || !parsed || md::trading_date(*parsed) != md::new_york_time(*parsed).date)
          throw std::invalid_argument("date must be a trading date, YYYY-MM-DD");
        date = md::new_york_time(*parsed).date;
      }
      const auto seed = demo ? (body.contains("seed") ? seed_field(body.at("seed"), *day) : fresh_seed()) : 0;
      auto session = std::make_shared<Session>();
      session->file = name;
      session->demo = demo;
      session->scenario = demo ? day->id : "";
      session->seed = demo ? std::to_string(seed) : "";
      session->generator = demo ? day->generator : 0;
      session->plan = plan->id;
      if (body.contains("start_at")) session->start_at = body.at("start_at").get<std::string>();
      if (demo) session->windows = providers::scenario_windows(*day, date);
      md::Timestamp first = demo ? session->windows.front().first : 0;
      md::Timestamp last = demo ? session->windows.back().last : 0;
      bool overnight = demo && day->overnight;
      if (!demo) {
        md::RecordingReader reader(path);
        first = reader.header().started;
        if (body.contains("start_at")) {
          bool seen = false;
          while (const auto event = reader.next()) {
            if (!seen) { first = event->received; seen = true; }
            last = std::max(last, event->received);
          }
          if (!reader.diagnostic().empty()) throw std::invalid_argument(reader.diagnostic());
        }
        date = md::trading_date(first);
        overnight = md::new_york_time(first).date < date;
      }
      session->date = md::format_date(date);
      if (body.contains("start_at")) {
        // A bare time is on the session's date, or its first occurrence in a run of several sessions.
        if (const auto dated = dated_time(session->start_at, "start_at")) session->target = *dated;
        else if (session->windows.size() > 1) session->target = session_time(session->start_at, session->windows, first - 1, "start_at");
        else {
          try { session->target = providers::scenario_time(session->start_at, date, overnight); }
          catch (const std::invalid_argument& error) { throw std::invalid_argument("start_at: " + std::string(error.what())); }
        }
        if (session->target < first || session->target > last)
          throw std::invalid_argument(session->windows.size() > 1 ? "start_at must be within the run's sessions" : "start_at must be within the recording's session");
      }
      static std::atomic<unsigned> runs{0};
      session->id = slug(demo ? day->id : name) + "-" + session->date + "-" + (demo ? session->seed : "recording") +
          "-" + (session->start_at.empty() ? "open" : slug(session->start_at)) + "-" + std::to_string(md::now()) + "-" + std::to_string(++runs);
      providers::ReplayProvider::Options playback;
      playback.file = demo ? demos_->get(*day, date, seed) : path;
      playback.speed = speed;
      playback.start_at = session->target;
      playback.paused = paused;
      // A run of several sessions passes the closed market between them in a step.
      if (session->windows.size() > 1)
        for (const auto& window : session->windows) playback.max_gap = std::max(playback.max_gap, window.step);
      session->provider = std::make_unique<providers::ReplayProvider>(std::move(playback));
      session->demo = providers::simulated_provider(session->provider->header().provider);
      auto engine = [&] {
        const std::lock_guard lock(mutex_);
        return options_.engine;
      }();
      session->durable = history_->writable();
      // Simulated dividends replace the server's on the same symbol/date; explicit
      // session entries already replace generated ones in this shared schedule.
      if (demo) {
        for (const auto& dividend : providers::scenario_dividends(*day, date)) {
          std::erase_if(engine.dividends, [&](const auto& d) {
            return d.symbol == dividend.symbol && d.ex_date == dividend.ex_date;
          });
          engine.dividends.push_back(dividend);
        }
      }
      engine.paper.rules = plan->rules;
      engine.paper.initial_cash = plan->initial_cash;
      // Copy definitions, never live-feed bindings, into this isolated run.
      if (!engine.paper_journal.empty()) {
        const auto definitions = engine.paper_journal.parent_path() / "playbooks.json";
        if (std::filesystem::exists(definitions)) {
          std::ifstream input(definitions);
          auto catalogue = json::parse(input);
          catalogue["modes"] = json::object();
          engine.initial_playbooks = catalogue.dump();
        }
      }
      // From its journal's creation until the run it replaces has flushed, both are
      // between running and history.
      std::unique_lock handoff(handoff_mutex_);
      engine.paper_journal = history_->create(*session, engine.paper);
      try {
        engine.replay = true;
        engine.initial_actor = request.actor;
        engine.run_input = demo ? scenario_input(*day, date, seed) : recording_input(path);
        engine.run_id = session->id;
        engine.paper_accounts.clear();
        engine.paper_sink.reset();
        engine.record_file.clear();
        engine.candles = std::make_shared<CandleStore>();
        // The replay's clock is the recording's: sessions and feed checks see that day.
        auto* provider = session->provider.get();
        engine.clock = [provider] { return provider->time(); };
        const auto& header = provider->header();
        session->engine = std::make_unique<Engine>(*provider, md::Subscription{header.subscription.underlyings, 0, 0.0}, engine);
        session->engine->start();
        if (engine.paper_enabled && !session->engine->status().trading.enabled)
          throw std::runtime_error(session->engine->status().trading.reason);
      } catch (...) {
        // A run that could not start, such as one whose first record a full disk
        // refused, leaves no history entry.
        if (session->engine) session->engine->stop();
        history_->discard(session->id);
        throw;
      }
      std::shared_ptr<Session> old;
      {
        const std::lock_guard lock(mutex_);
        old = std::exchange(session_, session);
      }
      if (old) {
        old->engine->stop();
        history_->finish(*old);
      }
      handoff.unlock();
      old.reset();
      complete(ok({{"replay", session->state()}}, 201));
    } else if (request.method == "PUT") {
      const auto body = parse_body(request, {"speed", "paused", "skip", "until"});
      const auto session = current();
      if (!session) {
        complete(api_error(404, "NO_REPLAY", "No replay is running"));
        return;
      }
      if (session->provider->fast_forwarding()) {
        complete(api_error(409, "REPLAY_FAST_FORWARD", "Wait until the replay reaches start_at before changing playback"));
        return;
      }
      if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
      if (body.contains("skip") && !body.at("skip").is_boolean()) throw std::invalid_argument("skip must be true or false");
      if (body.contains("until")) {
        if (body.size() != 1 || !body.at("until").is_string())
          throw std::invalid_argument("until must be a time string and the only control");
        auto value = body.at("until").get<std::string>();
        // A bare time is on the session's date; in a run of several sessions, its next
        // occurrence in one of them. A date and time is New York unless it names its zone.
        std::optional<md::Timestamp> target = dated_time(value, "until");
        if (!target && session->windows.size() > 1) {
          target = session_time(value, session->windows, std::max(session->provider->settled_through(), session->provider->market_time()), "until");
        } else if (!target) {
          const auto date = md::trading_date(session->provider->header().started);
          auto day = date;
          if (md::new_york_time(session->provider->header().started).date < date && value.substr(0, 5) >= "20:15")
            day = md::date_from_days(md::days_since_epoch(date) - 1);
          if (value.size() == 5) value += ":00";
          target = md::parse_datetime(md::format_date(day) + "T" + value, md::Zone::NewYork);
        }
        if (!target) throw std::invalid_argument("until must be New York HH:MM[:SS], or a date and time such as 2026-09-17T10:30");
        session->provider->until(*target);
        complete(ok({{"replay", session->state()}, {"settled_through", md::format_timestamp(session->provider->settled_through())}}));
        return;
      }
      if (body.contains("speed")) session->provider->set_speed(speed_field(body));
      if (body.contains("paused")) session->provider->set_paused(body.at("paused").get<bool>());
      if (body.contains("skip") && body.at("skip").get<bool>()) session->provider->skip();
      if (body.value("paused", false)) session->engine->synchronize().get();
      complete(ok({{"replay", session->state()}}));
    } else if (request.method == "DELETE") {
      stop_session();
      complete(ok({{"replay", nullptr}}));
    } else {
      complete(api_error(405, "METHOD_NOT_ALLOWED", "Use GET, POST, PUT or DELETE"));
    }
  } catch (const json::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::exception& error) {
    complete(api_error(422, "REPLAY_FAILED", error.what()));
  }
}

std::string ReplayHost::tick() const {
  const auto session = current();
  if (!session) return {};
  auto message = json::parse(tick_message(*session->engine));
  message["type"] = "replay_tick";
  message["replay"] = session->state();
  replay_gate(message, *session->provider);
  return message.dump();
}

}  // namespace openport::server
