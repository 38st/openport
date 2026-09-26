// openportd: runs one market-data provider, the analytics engine and the web
// terminal's HTTP/WebSocket server.
//
//   openportd                                   # Cboe delayed SPX, SPY, QQQ, IWM and DIA, no key needed
//   openportd --provider databento --symbols SPX,QQQ
//   openportd --provider massive --symbols SPY --poll-seconds 5
//
// API keys come from the environment (DATABENTO_API_KEY, MASSIVE_API_KEY) and never
// leave this machine except to authenticate with that provider.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "openport/providers/cboe.hpp"
#include "openport/providers/factory.hpp"
#include "openport/providers/massive.hpp"
#include "openport/providers/options.hpp"
#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/web_server.hpp"
#include "openport/server/web_policy.hpp"

namespace {

using namespace openport;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Settings {
  md::ProviderConfig provider{"cboe", "", {}};
  md::Subscription subscription{{"SPX", "SPY", "QQQ", "IWM", "DIA"}, 0, 0.0};
  std::string address = "127.0.0.1";
  unsigned short port = 8080;
  std::filesystem::path web_root;
  std::filesystem::path record_file;
  std::filesystem::path record_dir;
  std::optional<std::filesystem::path> candle_dir;
  bool history = true;
  bool cboe_holidays = true;
  bool paper_enabled = true;
  bool compact_journals = false;
  std::filesystem::path paper_journal;
  trading::SessionConfig paper;
  std::vector<trading::Dividend> dividends;
  bool massive_dividends = false;
  const server::PlanPreset* plan = server::find_plan("practice");
  std::optional<trading::Money> paper_cash;
  std::string write_token;
  int threads = 2;
  double rate = 0.04;
  std::vector<std::string> allowed_origins;
};

int usage(const char* error = nullptr) {
  if (error) std::fprintf(stderr, "openportd: %s\n\n", error);
  std::fprintf(
      stderr,
      "usage: openportd [--provider NAME] [--symbols SPX,SPY,QQQ,IWM,DIA] [--address ADDR] [--port N]\n"
      "                 [--web-root DIR] [--expiries N] [--window F] [--poll-seconds N]\n"
      "                 [--record FILE] [--record-dir DIR] [--rate R] [--option KEY=VALUE]... [--allowed-origin ORIGIN]...\n"
      "                 [--paper-journal PATH] [--plan ID] [--paper-cash DECIMAL] [--paper-fee DECIMAL]\n"
      "                 [--no-paper] [--write-token TOKEN] [--candle-dir DIR] [--no-history]\n"
      "                 [--dividends FILE|massive] [--no-cboe-holidays]\n"
      "       openportd --compact-journals [--paper-journal PATH]\n"
      "       openportd --version\n\n"
      "paper: durable paper trading on index, equity and ETF options; cash 100000, fee\n"
      "       0.65; more named accounts live in an accounts directory beside the journal\n"
      "compact: rewrite journals from builds before the compact format, keeping each\n"
      "         original as FILE.bak, then exit; stop openportd first\n"
      "plan: rules for a new journal (practice, intraday-25k|50k|100k, eod-25k|50k|100k,\n"
      "      funded-intraday-25k|50k|100k, funded-eod-25k|50k|100k); default practice;\n"
      "      --paper-cash then overrides its starting balance\n"
      "write token: --write-token overrides OPENPORT_WRITE_TOKEN; required for remote writes\n"
      "dividends: SYMBOL,YYYY-MM-DD,AMOUNT lines (ex-date, dollars a share); on each ex-date\n"
      "           held shares receive the dividend and short shares pay it. \"massive\" reads\n"
      "           them for the stock and ETF symbols from Massive's API instead, every six\n"
      "           hours, with MASSIVE_API_KEY (any stocks plan), whatever the provider\n"
      "rate: assumed flat zero rate in [-0.05, 0.25], default 0.04 (4%%)\n"
      "allowed origins: exact http[s]://host[:port], in addition to same-origin\n"
      "databento: --expiries and --window must be 0 (whole-chain upstream subscription)\n"
      "record: create a new compressed event file (existing files are never overwritten);\n"
      "        --record-dir names one per run by provider and start time there, and the\n"
      "        web terminal replays recordings from it (default ~/.openport/recordings)\n"
      "charts: one-minute bars persist in --candle-dir (default ~/.openport/candles; replay\n"
      "        keeps them in memory); Cboe's free delayed chart history backfills them\n"
      "        unless --no-history (always off for replay)\n"
      "holidays: Cboe's published holiday schedule is read daily, so a closure it announces\n"
      "          applies without a new build, unless --no-cboe-holidays (always off for replay)\n"
      "replay: --option file=PATH [--option speed=1|10|60|max] [--option loop=on|off]\n"
      "providers:");
  for (auto name : providers::provider_names()) {
    std::fprintf(stderr, " %.*s", static_cast<int>(name.size()), name.data());
  }
  std::fprintf(stderr,
               "\nkeys are read from the environment: DATABENTO_API_KEY, MASSIVE_API_KEY\n");
  return 2;
}

std::vector<std::string> split(const std::string& list) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    std::string item = list.substr(start, comma - start);
    for (char& c : item) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string env_key_for(std::string name) {
  for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const char* value = std::getenv((name + "_API_KEY").c_str());
  return value ? value : "";
}

/// The built web terminal: $OPENPORT_WEB_ROOT, next to the binary, or ./web/dist.
std::filesystem::path find_web_root(const char* argv0) {
  if (const char* env = std::getenv("OPENPORT_WEB_ROOT")) return env;
  std::error_code ec;
  const auto exe_dir = std::filesystem::weakly_canonical(argv0, ec).parent_path();
  for (const auto& candidate :
       {exe_dir / "web", exe_dir / "../../web/dist", exe_dir / "../share/openport/web",
        std::filesystem::current_path() / "web/dist"}) {
    if (std::filesystem::exists(candidate / "index.html", ec)) {
      return std::filesystem::weakly_canonical(candidate, ec);
    }
  }
  return std::filesystem::current_path() / "web/dist";
}

std::string size_text(std::uintmax_t bytes) {
  char text[32];
  if (bytes >= 1'000'000) std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1e6);
  else std::snprintf(text, sizeof text, "%.1f KB", static_cast<double>(bytes) / 1e3);
  return text;
}

int compact_journals(const std::filesystem::path& journal, const std::filesystem::path& accounts) {
  const auto results = server::compact_paper_journals(journal, accounts);
  if (results.empty()) std::printf("no paper journals at %s\n", journal.c_str());
  bool failed = false;
  for (const auto& r : results) {
    if (!r.error.empty()) {
      failed = true;
      std::fprintf(stderr, "%s: left as it was: %s\n", r.file.c_str(), r.error.c_str());
    } else if (r.backup.empty()) {
      std::printf("%s: already compact (%s)\n", r.file.c_str(), size_text(r.bytes_before).c_str());
    } else {
      std::printf("%s: %s -> %s; the original is %s\n", r.file.c_str(), size_text(r.bytes_before).c_str(),
                  size_text(r.bytes_after).c_str(), r.backup.filename().c_str());
    }
  }
  return failed ? 1 : 0;
}

}  // namespace

int run(int argc, char** argv) {
  Settings settings;
  settings.web_root = find_web_root(argv[0]);
  if (const auto* token = std::getenv("OPENPORT_WRITE_TOKEN")) settings.write_token = token;
  if (const auto* home = std::getenv("HOME")) settings.paper_journal = std::filesystem::path(home) / ".openport/paper-journal.jsonl";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--help" || arg == "-h") return usage();
    if (arg == "--version") { std::printf("openportd %s\n", OPENPORT_VERSION); return 0; }
    if (arg == "--no-paper") { settings.paper_enabled = false; continue; }
    if (arg == "--no-history") { settings.history = false; continue; }
    if (arg == "--no-cboe-holidays") { settings.cboe_holidays = false; continue; }
    if (arg == "--compact-journals") { settings.compact_journals = true; continue; }
    if (!has_value) return usage(("missing value for " + arg).c_str());
    const std::string value = argv[++i];
    if (arg == "--provider") {
      settings.provider.name = value;
    } else if (arg == "--symbols") {
      settings.subscription.underlyings = split(value);
    } else if (arg == "--address") {
      settings.address = value;
    } else if (arg == "--port") {
      settings.port =
          static_cast<unsigned short>(providers::parse_integer(value, "--port", 1, 65535));
    } else if (arg == "--allowed-origin") {
      settings.allowed_origins.push_back(value);
    } else if (arg == "--paper-journal") {
      if (value.empty()) return usage("--paper-journal requires a nonempty path");
      settings.paper_journal = value;
    } else if (arg == "--plan") {
      const auto* plan = server::find_plan(value);
      if (!plan) return usage("--plan must be practice or an evaluation or funded plan ID (see --help)");
      settings.plan = plan;
    } else if (arg == "--paper-cash") {
      settings.paper_cash = trading::Money::parse(value);
    } else if (arg == "--paper-fee") {
      settings.paper.fee_per_contract = trading::Money::parse(value);
      if (settings.paper.fee_per_contract < trading::Money{}) return usage("--paper-fee must be nonnegative");
    } else if (arg == "--write-token") {
      if (value.empty()) return usage("--write-token requires a nonempty token");
      settings.write_token = value;
    } else if (arg == "--record") {
      if (value.empty()) return usage("--record requires a nonempty path");
      settings.record_file = value;
    } else if (arg == "--record-dir") {
      if (value.empty()) return usage("--record-dir requires a nonempty path");
      settings.record_dir = value;
    } else if (arg == "--candle-dir") {
      if (value.empty()) return usage("--candle-dir requires a nonempty path");
      settings.candle_dir = value;
    } else if (arg == "--dividends" && value == "massive") {
      settings.massive_dividends = true;
    } else if (arg == "--dividends") {
      std::ifstream file(value);
      if (!file) return usage(("--dividends: cannot read " + value).c_str());
      try {
        settings.dividends = trading::parse_dividends(file);
      } catch (const std::invalid_argument& error) {
        return usage(error.what());
      }
    } else if (arg == "--web-root") {
      settings.web_root = value;
    } else if (arg == "--expiries") {
      settings.subscription.max_expiries = providers::parse_integer(value, "--expiries");
    } else if (arg == "--window") {
      settings.subscription.strike_window = providers::parse_fraction(value, "--window");
    } else if (arg == "--rate") {
      std::istringstream input(value);
      input.imbue(std::locale::classic());
      input >> std::noskipws >> settings.rate;
      if (!input || input.peek() != std::char_traits<char>::eof() ||
          !std::isfinite(settings.rate) || settings.rate < -0.05 || settings.rate > 0.25)
        return usage("--rate must be a finite number in [-0.05, 0.25]");
    } else if (arg == "--poll-seconds") {
      settings.provider.options["poll_seconds"] = value;
    } else if (arg == "--option") {
      const std::size_t eq = value.find('=');
      if (eq == std::string::npos) return usage("--option takes KEY=VALUE");
      settings.provider.options[value.substr(0, eq)] = value.substr(eq + 1);
    } else {
      return usage(("unknown option " + arg).c_str());
    }
  }
  // More named accounts live beside the main journal, one journal each.
  const auto paper_accounts = settings.paper_journal.empty()
      ? std::filesystem::path{} : std::filesystem::absolute(settings.paper_journal).parent_path() / "accounts";
  if (settings.compact_journals) {
    if (settings.paper_journal.empty()) return usage("HOME is unavailable; specify --paper-journal");
    return compact_journals(settings.paper_journal, paper_accounts);
  }
  if (settings.subscription.underlyings.empty()) return usage("no symbols");
  settings.provider.api_key = env_key_for(settings.provider.name);
  if (settings.massive_dividends && env_key_for("massive").empty())
    return usage("--dividends massive needs MASSIVE_API_KEY");

  if (settings.paper_enabled && settings.paper_journal.empty())
    return usage("HOME is unavailable; specify --paper-journal or --no-paper");
  providers::validate_subscription(settings.provider.name, settings.subscription);
  auto provider = providers::make_provider(settings.provider);

  // A replay's market times are in the past: its bars must not mix with live history.
  const bool replay = settings.provider.name == "replay";
  // Each run records to its own file, named by provider and UTC start time.
  if (!settings.record_dir.empty() && settings.record_file.empty()) {
    std::filesystem::create_directories(settings.record_dir);
    auto stamp = md::format_timestamp(md::now()).substr(0, 19);  // 2026-09-23T20:15:30
    stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
    settings.record_file = settings.record_dir / (settings.provider.name + "-" + stamp + "Z.oprec");
  }
  server::CandleStore::Options candle_options;
  if (settings.candle_dir)
    candle_options.directory = *settings.candle_dir;
  else if (const auto* home = std::getenv("HOME"); home && !replay)
    candle_options.directory = std::filesystem::path(home) / ".openport/candles";
  const auto candles = std::make_shared<server::CandleStore>(candle_options);
  if (const auto error = candles->error(); !error.empty())
    std::fprintf(stderr, "openportd: %s\n", error.c_str());

  server::Engine::Options engine_options;
  engine_options.candles = candles;
  engine_options.analytics.fallback_rate = settings.rate;
  engine_options.record_file = settings.record_file;
  engine_options.paper_enabled = settings.paper_enabled;
  engine_options.paper_journal = settings.paper_journal;
  engine_options.paper_accounts = paper_accounts;
  // Rules and cash seed new journals only; recovery restores the recorded configuration.
  settings.paper.rules = settings.plan->rules;
  settings.paper.initial_cash = settings.paper_cash.value_or(settings.plan->initial_cash);
  if (settings.paper.initial_cash <= trading::Money{}) return usage("--paper-cash must be positive");
  engine_options.paper = settings.paper;
  engine_options.dividends = settings.dividends;
  engine_options.write_mode = server::write_mode({settings.address, settings.write_token, settings.allowed_origins});
  server::Engine engine(*provider, settings.subscription, engine_options);
  engine.start();
  std::unique_ptr<providers::CboeChartHistory> history;
  if (settings.history && !replay) {
    history = std::make_unique<providers::CboeChartHistory>(
        settings.subscription.underlyings,
        [candles](const std::string& symbol, providers::CboeChart chart, std::vector<md::Bar> bars) {
          if (chart == providers::CboeChart::Intraday)
            candles->merge_minutes(symbol, bars);
          else
            candles->merge_days(symbol, bars);
        });
    history->start();
  }
  // Closures the exchange announces reach the calendar without a new build.
  std::unique_ptr<providers::CboeHolidaySchedule> holidays;
  if (settings.cboe_holidays && !replay) {
    holidays = std::make_unique<providers::CboeHolidaySchedule>(md::set_scheduled_days);
    holidays->start();
  }
  const auto trading_status = engine.status().trading;
  if (trading_status.reason.starts_with("JOURNAL_LOCKED:"))
    std::fprintf(stderr, "openportd: %s\n", trading_status.reason.c_str());
  // Recorded days replay beside the live feed, each with its own paper account.
  server::ReplayHost::Options replay_options;
  replay_options.engine = engine_options;
  if (!settings.record_dir.empty())
    replay_options.recordings = settings.record_dir;
  else if (const auto* home = std::getenv("HOME"))
    replay_options.recordings = std::filesystem::path(home) / ".openport/recordings";
  server::ReplayHost replays(replay_options);
  // Dividends from Massive reach the live engine and replays started after them.
  std::unique_ptr<providers::MassiveDividends> dividends;
  if (settings.massive_dividends) {
    std::vector<std::string> tickers;
    for (const auto& symbol : settings.subscription.underlyings)
      if (!md::is_index_underlying(symbol)) tickers.push_back(symbol);
    providers::MassiveDividends::Options options;
    options.api_key = env_key_for("massive");
    dividends = std::make_unique<providers::MassiveDividends>(
        tickers,
        [&engine, &replays](std::vector<providers::MassiveDividend> found) {
          // Two distributions going ex together are paid as one.
          std::map<std::pair<std::string, md::Date>, trading::Money> amounts;
          for (const auto& d : found) {
            try {
              auto& amount = amounts[{d.ticker, d.ex_date}];
              amount = amount + trading::Money::from_double(d.cash_amount);
            } catch (const std::exception&) {
            }
          }
          std::vector<trading::Dividend> list;
          for (const auto& [key, amount] : amounts)
            if (amount > trading::Money{}) list.push_back({key.first, key.second, amount});
          engine.set_dividends(list);
          replays.set_dividends(std::move(list));
        },
        options);
    dividends->start();
  }
  server::WebServer web(
      settings.address, settings.port, settings.web_root,
      [&engine, &replays](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (replays.handle(request, complete)) return;
        server::handle_api_async(request, engine, std::move(complete));
      }, settings.allowed_origins, settings.write_token);
  web.start(settings.threads);

  std::string symbols;
  for (const auto& symbol : settings.subscription.underlyings) {
    symbols += (symbols.empty() ? "" : ", ") + symbol;
  }
  std::printf("OpenPort on http://%s:%u  (provider %s: %s)\n", settings.address.c_str(), web.port(),
              settings.provider.name.c_str(), symbols.c_str());
  std::printf("web terminal: %s\n", settings.web_root.string().c_str());
  std::fflush(stdout);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::string recording_error;
  std::string history_error;
  std::string holidays_error;
  std::string dividends_error;
  std::string candle_error;
  std::string breaker_error;
  auto report = [](const std::string& error, std::string& reported) {
    if (!error.empty() && error != reported) std::fprintf(stderr, "openportd: %s\n", error.c_str());
    reported = error;
  };
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    web.broadcast(server::tick_message(engine));
    if (auto tick = replays.tick(); !tick.empty()) web.broadcast(tick);
    report(engine.recording_error(), recording_error);
    report(engine.status().circuit_breaker.error, breaker_error);
    if (history) report(history->error(), history_error);
    if (holidays) report(holidays->error(), holidays_error);
    if (dividends) report(dividends->error(), dividends_error);
    report(candles->error(), candle_error);
  }
  std::printf("\nshutting down\n");
  web.stop();
  replays.stop();
  if (history) history->stop();
  if (holidays) holidays->stop();
  if (dividends) dividends->stop();
  engine.stop();
  candles->flush();
  if (!settings.record_file.empty()) {
    const auto stats = engine.recording_stats();
    const double count = static_cast<double>(stats.events);
    std::printf("recording: %llu events, %llu bytes, %.1f ns/event publish, %.2f bytes/event\n",
                static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.bytes),
                count > 0 ? static_cast<double>(stats.publish_nanoseconds) / count : 0,
                count > 0 ? static_cast<double>(stats.bytes) / count : 0);
    if (const auto error = engine.recording_error(); !error.empty()) {
      std::fprintf(stderr, "openportd: %s\n", error.c_str());
      return 1;
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openportd: %s\n", error.what());
    return 2;
  } catch (...) {
    std::fprintf(stderr, "openportd: unknown startup failure\n");
    return 2;
  }
}
