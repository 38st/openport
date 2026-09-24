#include "openport/server/engine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>

namespace openport::server {
namespace {
using namespace trading;

/// Provenance is an integration concern. Add it to the same durable transaction
/// as settlement, leaving the core's recorded outcomes and recovery schema intact.
class SettlementJournal final : public Journal {
 public:
  SettlementJournal(std::shared_ptr<Journal> sink, const std::map<std::string, std::string>& source)
      : sink_(std::move(sink)), source_(source) {}
  void append(Timestamp time, std::string_view type, std::string_view payload) override {
    if (type != "settlement") { sink_->append(time, type, payload); return; }
    auto record = nlohmann::json::parse(payload);
    record["settlement_source"] = source_;
    sink_->append(time, type, record.dump());
  }
  std::uint64_t sequence() const override { return sink_->sequence(); }
  std::string head() const override { return sink_->head(); }
 private:
  std::shared_ptr<Journal> sink_;
  const std::map<std::string, std::string>& source_;
};

void sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) throw TradingError(Reason::JOURNAL_IO, "Cannot open journal directory for durability");
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) throw TradingError(Reason::JOURNAL_IO, "Cannot sync journal directory");
}

md::Date new_york_date(md::Timestamp time) {
  auto date = md::date_from_days(time / md::kNanosPerDay);
  // New York is behind UTC; compare its actual midnight, including DST dates.
  if (time < md::new_york_to_utc(date, 0, 0))
    date = md::date_from_days(md::days_since_epoch(date) - 1);
  return date;
}
Quantity whole_size(double size) {
  if (!std::isfinite(size) || size <= 0 || size >= 9223372036854775808.0) return 0;
  return static_cast<Quantity>(std::floor(size));
}
std::optional<Money> quote_price(double price) {
  if (!std::isfinite(price) || price <= 0) return {};
  try { return Money::from_double(price); }
  catch (const TradingError&) { return {}; }
}
Valuation valuation_for(const std::string& symbol, const md::OptionContract& contract,
                         const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  Valuation result;
  result.symbol = symbol;
  result.valid = false;
  if (!metrics) return result;
  result.time = metrics->as_of;
  for (const auto& slice : metrics->slices) {
    if (slice.expiry_time != contract.expiry_time() || slice.style != contract.style) continue;
    for (const auto& strike : slice.strikes) {
      const auto& option = contract.type == pricing::OptionType::Call ? strike.call : strike.put;
      if (option.id == analytics::kNoInstrument || option.contract.osi_symbol() != symbol) continue;
      result = {symbol, metrics->as_of, option.delta, option.gamma, option.vega, option.theta,
                metrics->spot, slice.forward.forward, slice.forward.discount, slice.years,
                strike.iv, true};
      result.valid = valid_valuation(result);
      return result;
    }
  }
  return result;
}
/// Opens a journal for writing, making each directory created on the way durable:
/// an existing file is locked, then read for recovery; otherwise a new one is created.
std::pair<std::shared_ptr<Journal>, std::optional<JournalRecovery>> open_journal(const std::filesystem::path& file) {
  const auto parent = std::filesystem::absolute(file).parent_path();
  auto existing = parent;
  while (!std::filesystem::exists(existing)) existing = existing.parent_path();
  std::filesystem::create_directories(parent);
  for (auto path = parent; path != existing; path = path.parent_path()) sync_directory(path);
  sync_directory(existing);
  if (std::filesystem::exists(file)) {
    // Lock before reading, then recover the same verified head held by the writer.
    std::shared_ptr<Journal> journal = FileJournal::resume(file.string());
    return {journal, FileJournal::read(file.string())};
  }
  std::shared_ptr<Journal> journal = FileJournal::create(file.string());
  sync_directory(parent);
  return {journal, std::nullopt};
}

/// Account IDs name journal files: lowercase letters, digits and single hyphens.
bool account_id(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }) &&
         id.find("--") == std::string_view::npos;
}
std::string slug(std::string_view name) {
  std::string id;
  for (const char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id += c;
    else if (c >= 'A' && c <= 'Z') id += static_cast<char>(c - 'A' + 'a');
    else if (!id.empty() && id.back() != '-') id += '-';
    if (id.size() >= 32) break;
  }
  while (!id.empty() && id.back() == '-') id.pop_back();
  return id;
}
}  // namespace

trading::Decision paper_acceptance(std::string_view underlying, md::Timestamp market_time,
    md::Timestamp wall_time, std::chrono::seconds delay, md::Timestamp max_quote_age) {
  if (market_time <= 0)
    return {Reason::INVALID_QUOTE, std::string(underlying) + " is waiting for market data", {}, {}, {}};
  const auto delay_seconds = std::max<std::int64_t>(0, delay.count());
  // A healthy feed shows the market as it was `delay` ago, and stops at the end
  // of a session. Data more than max_quote_age behind that is a stalled feed;
  // a feed that rightly shows a closed market (as in a session's first minutes
  // on a delayed feed) is not.
  const auto shown = delay_seconds >= wall_time / md::kNanosPerSecond ? 0 : wall_time - delay_seconds * md::kNanosPerSecond;
  const auto session = md::trading_session(underlying, market_time);
  auto expected = market_time;
  if (md::trading_session(underlying, shown).open) expected = shown;
  else if (session.open && session.end <= shown) expected = session.end;
  if (expected > market_time && expected - market_time > max_quote_age) {
    const auto lag = wall_time - market_time;
    const auto minutes = lag / md::kNanosPerMinute;
    const auto duration = minutes >= 60
        ? std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m"
        : minutes > 0 ? std::to_string(minutes) + "m"
                      : std::to_string(lag / md::kNanosPerSecond) + "s";
    return {Reason::FEED_STALLED, std::string(underlying) + " quotes are " + duration +
        " behind the market; the feed appears to have stalled", {}, {}, {}};
  }
  if (!session.open) {
    // Say why the market is closed now (a weekend, say), not at the feed's last close.
    const auto now = md::trading_session(underlying, shown);
    return {Reason::SESSION_CLOSED, std::string(underlying) + " options are " + (now.open ? session.note : now.note), {}, {}, {}};
  }
  return {};
}

namespace {
/// Rewrite one journal (see compact_paper_journals).
JournalCompaction compact_journal(const std::filesystem::path& file) {
  JournalCompaction result{file, 0, 0, {}, {}};
  const auto temporary = std::filesystem::path(file.string() + ".compacting");
  try {
    // The writer's lock, held until the rewrite is in place.
    const auto lock = FileJournal::resume(file.string());
    const auto recovery = FileJournal::read(file.string(), lock->head());
    result.bytes_before = result.bytes_after = std::filesystem::file_size(file);
    // Only records with whole states shrink; a final schema 1 record stays.
    bool whole = false;
    for (const auto& r : recovery.records) {
      const auto schema = nlohmann::json::parse(r.payload).at("schema");
      if (schema == 2 || (schema == 1 && r.seq < recovery.records.size())) { whole = true; break; }
    }
    if (!whole) return result;
    std::filesystem::remove(temporary);  // Left by a run that stopped part way.
    std::string expected;
    {
      const auto out = FileJournal::create(temporary.string());
      expected = TradingSession::compact(recovery, *out);
    }
    if (TradingSession::recover(FileJournal::read(temporary.string())).snapshot_json() != expected)
      throw TradingError(Reason::JOURNAL_CORRUPT, "the rewrite does not recover to the same account");
    auto backup = std::filesystem::path(file.string() + ".bak");
    for (int n = 2; std::filesystem::exists(backup); ++n) backup = file.string() + ".bak" + std::to_string(n);
    std::filesystem::create_hard_link(file, backup);
    std::filesystem::rename(temporary, file);
    sync_directory(std::filesystem::absolute(file).parent_path());
    result.backup = backup;
    result.bytes_after = std::filesystem::file_size(file);
  } catch (const TradingError& error) {
    result.error = std::string(to_string(error.code())) + ": " + error.what();
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  if (!result.error.empty()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  }
  return result;
}
}  // namespace

std::vector<JournalCompaction> compact_paper_journals(const std::filesystem::path& journal,
                                                      const std::filesystem::path& accounts) {
  std::vector<JournalCompaction> results;
  std::error_code ec;
  if (!journal.empty() && std::filesystem::exists(journal, ec)) results.push_back(compact_journal(journal));
  if (!accounts.empty() && std::filesystem::is_directory(accounts, ec)) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(accounts, ec))
      if (entry.path().extension() == ".jsonl" && account_id(entry.path().stem().string())) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto& file : files) results.push_back(compact_journal(file));
  }
  return results;
}

Engine::PaperAccount* Engine::find_account(std::string_view id) {
  if (id.empty()) id = kMainAccount;
  for (auto& account : accounts_)
    if (account.id == id) return &account;
  return nullptr;
}

std::shared_ptr<const TradingView> Engine::trading_view() const { return trading_view(kMainAccount); }

std::shared_ptr<const TradingView> Engine::trading_view(std::string_view account) const {
  const std::lock_guard lock(mutex_);
  const auto it = trading_views_.find(account.empty() ? kMainAccount : account);
  return it == trading_views_.end() ? nullptr : it->second;
}

bool Engine::post_trading(TradingCommand command, TradingCompletion completion) {
  const std::lock_guard lock(command_mutex_);
  if (!accepting_commands_ || stopping_ || commands_.size() >= options_.command_capacity ||
      next_command_ == std::numeric_limits<std::uint64_t>::max()) return false;
  commands_.push_back({next_command_++, std::move(command), std::move(completion)});
  return true;
}

void Engine::start_trading() {
  if (!options_.paper_enabled) return;
  // Each account opens on its own: one that fails reports why and the rest trade.
  const auto open = [&](PaperAccount& account, const std::filesystem::path& file, bool seed) {
    try {
      std::shared_ptr<Journal> journal = file.empty() ? options_.paper_sink : nullptr;
      std::optional<JournalRecovery> recovery;
      if (!file.empty()) std::tie(journal, recovery) = open_journal(file);
      if (journal) journal = std::make_shared<SettlementJournal>(journal, settlement_source_);
      if (recovery) account.session = std::make_unique<TradingSession>(TradingSession::recover(*recovery, journal));
      else if (seed) account.session = std::make_unique<TradingSession>(options_.paper, 0, journal);
      else throw TradingError(Reason::JOURNAL_CORRUPT, "The account journal is empty");
    } catch (const TradingError& error) {
      account.failure = std::string(to_string(error.code())) + ": " + error.what();
    } catch (const std::exception& error) {
      account.failure = std::string("JOURNAL_IO: ") + error.what();
    }
  };
  accounts_.push_back({std::string(kMainAccount), "Main", nullptr, {}});
  open(accounts_.back(), options_.paper_journal, true);
  std::error_code ec;
  if (!options_.paper_accounts.empty() && std::filesystem::is_directory(options_.paper_accounts, ec)) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(options_.paper_accounts, ec))
      if (entry.path().extension() == ".jsonl" && account_id(entry.path().stem().string())) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      PaperAccount account{file.stem().string(), file.stem().string(), nullptr, {}};
      std::ifstream named(std::filesystem::path(file).replace_extension(".name"));
      if (std::string name; named && std::getline(named, name) && !name.empty() && name.size() <= 64) account.name = name;
      open(account, file, false);
      accounts_.push_back(std::move(account));
    }
  }
  for (const auto& account : accounts_) {
    if (!account.session) continue;
    market_time_ = std::max(market_time_, account.session->snapshot()->time);
    for (const auto& [symbol, contract] : account.session->contracts())
      if (const auto quote = account.session->quote(symbol))
        observations_[symbol] = std::max(observations_[symbol], quote->observation);
  }
  publish_trading();
  const std::lock_guard lock(command_mutex_);
  accepting_commands_ = !stopping_;
}

void Engine::create_account(const TradingCommand& c, TradingReply& reply) {
  if (!options_.paper_enabled || options_.paper_accounts.empty() || stopping_) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = "This server keeps a single paper account";
    return;
  }
  auto base = slug(c.name);
  if (base.empty() || base == kMainAccount) base = "account";
  auto id = base;
  for (int n = 2; find_account(id); ++n) id = base + "-" + std::to_string(n);
  PaperAccount account{id, c.name, nullptr, {}};
  try {
    auto config = options_.paper;
    config.rules = c.rules;
    config.initial_cash = c.initial_cash;
    const auto file = options_.paper_accounts / (id + ".jsonl");
    auto [journal, recovery] = open_journal(file);
    if (recovery) throw TradingError(Reason::JOURNAL_CORRUPT, "An account journal already exists at " + file.string());
    {
      const auto named = options_.paper_accounts / (id + ".name");
      std::ofstream out(named, std::ios::trunc);
      out << c.name << '\n';
      out.flush();
      if (!out) throw TradingError(Reason::JOURNAL_IO, "Cannot write " + named.string());
    }
    account.session = std::make_unique<TradingSession>(config, market_time_,
        std::make_shared<SettlementJournal>(journal, settlement_source_));
  } catch (const TradingError& error) {
    reply.decision = {error.code(), error.what(), {}, {}, {}};
    return;
  } catch (const std::exception& error) {
    reply.decision = {Reason::JOURNAL_IO, error.what(), {}, {}, {}};
    return;
  }
  accounts_.push_back(std::move(account));
  publish_trading();
  reply.account = id;
}

void Engine::publish_trading() {
  if (!options_.paper_enabled) return;
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> views;
  std::vector<AccountStatus> statuses;
  for (const auto& account : accounts_) {
    std::shared_ptr<TradingView> view;
    if (account.session) {
      const auto& session = *account.session;
      view = std::make_shared<TradingView>();
      view->snapshot = session.snapshot();
      view->config = session.config();
      view->contracts = session.contracts();
      view->valuations = session.valuations();
      for (const auto& [symbol, contract] : view->contracts) {
        if (const auto quote = session.quote(symbol)) {
          auto& time = view->market_times[contract.underlying];
          time = std::max(time, quote->time);
        }
      }
      for (const auto& [symbol, book] : book_.underlyings()) {
        auto& time = view->market_times[symbol];
        time = std::max(time, book.data_time);
      }
    }
    TradingStatus status;
    status.enabled = account.failure.empty() && view != nullptr;
    status.reason = account.failure;
    status.write = status.enabled ? options_.write_mode : "disabled";
    const auto& config = view ? view->config : options_.paper;
    status.fee_per_contract = config.fee_per_contract;
    status.initial_cash = config.initial_cash;
    status.plan = config.rules.plan;
    if (view) {
      status.account_version = view->snapshot->account_version;
      status.kill_latched = view->snapshot->risk.kill_latched;
      if (config.rules.evaluation()) {
        constexpr const char* names[] = {"active", "passed", "failed"};
        status.evaluation = names[static_cast<int>(view->snapshot->evaluation.status)];
      }
    }
    statuses.push_back({account.id, account.name, std::move(status)});
    if (view) views.emplace(account.id, std::move(view));
  }
  const std::lock_guard lock(mutex_);
  trading_views_ = std::move(views);
  if (!statuses.empty()) status_.trading = statuses.front().trading;
  status_.accounts = std::move(statuses);
}

void Engine::fail_trading(PaperAccount& account, std::string reason) {
  account.failure = std::move(reason);
  publish_trading();
}

void Engine::observe_trading(const md::Event& event) {
  if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
    instruments_[definition->contract.osi_symbol()] = definition->id;
  } else if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
    market_time_ = std::max(market_time_, quote->ts);
    if (const auto* option = book_.option(quote->id)) {
      auto& number = observations_[option->contract.osi_symbol()];
      if (number == std::numeric_limits<std::uint64_t>::max()) {
        for (auto& account : accounts_) fail_trading(account, "ARITHMETIC_OVERFLOW: quote observation exhausted");
      } else {
        ++number;
      }
    }
  } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
    market_time_ = std::max(market_time_, spot->ts);
  } else if (const auto* trade = std::get_if<md::OptionTrade>(&event)) {
    market_time_ = std::max(market_time_, trade->ts);
  }
}

void Engine::update_trading(const std::vector<md::Event>& batch,
                            std::deque<PendingCommand>& commands) {
  if (accounts_.empty()) return;
  if (batch.empty() && commands.empty()) return;
  const auto now = wall_time();
  // Underlying prints retain ingress order. The first valid last on a date at or
  // after its regular close (16:00, 13:00 early) is our documented PM
  // closing-print approximation.
  for (const auto& event : batch) {
    const auto* spot = std::get_if<md::UnderlyingQuote>(&event);
    if (!spot || !std::isfinite(spot->last) || spot->last <= 0) continue;
    const auto date = new_york_date(spot->ts);
    if (spot->ts >= md::new_york_to_utc(date, md::regular_close_hour(date), 0))
      closing_prints_.try_emplace({spot->symbol, date}, *spot);
  }
  for (auto& account : accounts_) {
    if (!account.session || !account.failure.empty()) continue;
    auto& session = *account.session;
    try {
      std::set<std::string> symbols;
      for (const auto& p : session.snapshot()->positions) symbols.insert(p.position.contract.osi_symbol());
      for (const auto& order : session.snapshot()->open_orders)
        for (const auto& symbol : order_symbols(order.request)) symbols.insert(symbol);
      for (const auto& pending : commands) {
        const auto& command = pending.command;
        if ((command.account.empty() ? kMainAccount : std::string_view(command.account)) != account.id) continue;
        if (command.kind == TradingCommand::Kind::Submit)
          for (const auto& symbol : order_symbols(command.order)) symbols.insert(symbol);
        if (command.kind == TradingCommand::Kind::Settle) symbols.insert(command.symbol);
      }
      std::vector<QuoteObservation> quotes;
      std::vector<Valuation> valuations;
      for (const auto& symbol : symbols) {
        const auto id = instruments_.find(symbol);
        const auto* option = id == instruments_.end() ? nullptr : book_.option(id->second);
        if (option && !session.contracts().contains(symbol)) {
          const auto result = session.define(option->contract, market_time_);
          if (!result.decision.ok()) continue;
        }
        const auto definition = session.contracts().find(symbol);
        if (definition == session.contracts().end()) continue;
        if (option) {
          const auto& a = option->contract;
          const auto& b = definition->second;
          if (a.root != b.root || a.underlying != b.underlying || a.expiry != b.expiry ||
              a.strike != b.strike || a.type != b.type || a.style != b.style ||
              a.settlement != b.settlement || a.multiplier != b.multiplier || a.standard != b.standard)
            throw TradingError(Reason::INVALID_CONTRACT, "INVALID_CONTRACT: listed terms conflict with registered definition");
        }
        // A stalled feed cannot replenish resting-order liquidity. Keep the orders
        // and reducer clock intact; ordinary market-time DAY/expiry rules still apply.
        if (option && option->has_quote && option->quote_ts >= 0 &&
            paper_acceptance(definition->second.underlying, option->quote_ts, now,
                status_.capabilities.delay, session.config().limits.max_quote_age).code != Reason::FEED_STALLED) {
          quotes.push_back({symbol, observations_[symbol], option->quote_ts,
                            quote_price(option->bid), quote_price(option->ask),
                            whole_size(option->bid_size), whole_size(option->ask_size)});
        }
        auto valuation = valuation_for(symbol, definition->second, metrics(definition->second.underlying));
        // Missing live analytics after recovery must not overwrite a recorded frame.
        if (valuation.time > 0) valuations.push_back(std::move(valuation));
      }
      // Underlyings price the shares that equity options deliver, and exercise.
      std::set<std::string> deliverable;
      for (const auto& p : session.snapshot()->positions) {
        const auto& c = p.position.contract;
        if (c.style == pricing::ExerciseStyle::American && !md::is_index_underlying(c.underlying)) deliverable.insert(c.underlying);
      }
      for (const auto& stock : session.snapshot()->stocks) deliverable.insert(stock.position.symbol);
      std::vector<StockPrice> stocks;
      for (const auto& symbol : deliverable) {
        const auto book = book_.underlyings().find(symbol);
        if (book == book_.underlyings().end() || book->second.spot_ts <= 0 || book->second.spot_ts > market_time_) continue;
        if (const auto price = quote_price(book->second.spot)) stocks.push_back({symbol, book->second.spot_ts, *price});
      }
      session.on_quotes(quotes, valuations, market_time_, stocks);
      // A PM contract settles on its expiry date's closing print once it expires:
      // at the close, or a quarter hour later for ETF options that trade until 16:15.
      for (const auto& p : session.snapshot()->positions) {
        const auto& contract = p.position.contract;
        if (contract.settlement != md::Settlement::PM || market_time_ < contract.expiry_time()) continue;
        const auto print = closing_prints_.find({contract.underlying, contract.expiry});
        if (print == closing_prints_.end()) continue;
        const auto& spot = print->second;
        settlement_source_ = {{"kind", "provider_closing_print"}, {"provider", std::string(provider_.name())},
                              {"symbol", spot.symbol}, {"quote_time", md::format_timestamp(spot.ts)}};
        session.settle(contract.osi_symbol(), Money::from_double(spot.last), market_time_);
      }
      // An overnight session belongs to the next trading date, so a day ends
      // when the last session of the one before (curb) does.
      const auto day = md::trading_date(market_time_);
      if (!batch.empty() && day > session.trading_day() &&
          md::market_session(md::new_york_to_utc(day, 12, 0)).open &&
          session.snapshot()->valuation_complete)
        session.roll_day(market_time_, trading::dividends_due(options_.dividends, session.trading_day(), day));
    } catch (const TradingError& error) {
      account.failure = std::string(to_string(error.code())) + ": " + error.what();
    } catch (const std::exception& error) {
      account.failure = std::string("TRADING_UNAVAILABLE: ") + error.what();
    }
  }
  publish_trading();
}

void Engine::apply_command(PendingCommand& pending) {
  TradingReply reply;
  const auto& c = pending.command;
  auto* account = c.kind == TradingCommand::Kind::CreateAccount ? nullptr : find_account(c.account);
  if (c.kind == TradingCommand::Kind::CreateAccount) {
    create_account(c, reply);
    if (!reply.account.empty()) account = find_account(reply.account);
  } else if (!account) {
    reply.error_code = "UNKNOWN_ACCOUNT";
    reply.decision.message = "No paper account " + c.account;
  } else if (!account->session || !account->failure.empty() || stopping_) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = account->failure.empty() ? "Trading is disabled or engine is stopping" : account->failure;
  } else {
    auto& session = *account->session;
    const auto before = session.snapshot();
    // New orders need the underlying's feed to be current, as the ticket shows.
    const auto acceptance = [&](const std::string& underlying) {
      const auto view = trading_view(account->id);
      const auto time = view->market_times.find(underlying);
      return paper_acceptance(underlying, time == view->market_times.end() ? 0 : time->second,
                              wall_time(), status_.capabilities.delay, session.config().limits.max_quote_age);
    };
    try {
      CommandResult result;
      switch (c.kind) {
        case TradingCommand::Kind::Submit: {
          // Every contract the order trades (each leg of a multi-leg order).
          Decision rejection;
          for (const auto& symbol : order_symbols(c.order)) {
            const md::OptionContract* contract = nullptr;
            const auto id = instruments_.find(symbol);
            if (id != instruments_.end()) {
              if (const auto* option = book_.option(id->second)) contract = &option->contract;
            }
            if (!contract) {
              const auto saved = session.contracts().find(symbol);
              if (saved != session.contracts().end()) contract = &saved->second;
            }
            if (!contract) continue;
            rejection = eligible(*contract);
            if (rejection.ok()) rejection = acceptance(contract->underlying);
            if (!rejection.ok()) break;
          }
          result = session.submit(c.order, market_time_, rejection);
          break;
        }
        case TradingCommand::Kind::Modify: {
          // A change can trade at once, so it takes a new order's feed gate.
          Decision rejection;
          for (const auto& order : before->open_orders)
            if (order.id == c.order_id) {
              const auto contract = session.contracts().find(order_symbols(order.request).front());
              if (contract != session.contracts().end()) rejection = acceptance(contract->second.underlying);
            }
          result = session.modify(c.order_id, c.change, market_time_, rejection);
          break;
        }
        case TradingCommand::Kind::CancelAll:
          result = session.cancel_all(c.underlying.empty() ? std::nullopt : std::optional(c.underlying), market_time_);
          break;
        case TradingCommand::Kind::ClosePositions: {
          std::map<std::string, Decision> rejections;
          for (const auto& p : before->positions) {
            const auto& underlying = p.position.contract.underlying;
            if ((c.underlying.empty() || underlying == c.underlying) && !rejections.contains(underlying))
              if (auto rejection = acceptance(underlying); !rejection.ok()) rejections.emplace(underlying, std::move(rejection));
          }
          result = session.close_positions(c.underlying.empty() ? std::nullopt : std::optional(c.underlying),
                                           market_time_, rejections);
          break;
        }
        case TradingCommand::Kind::Cancel: result = session.cancel(c.order_id, market_time_); break;
        case TradingCommand::Kind::Limits:
          if (c.expected_revision != before->risk.limits_revision) {
            reply.error_code = "LIMITS_REVISION";
            reply.decision.message = "Limits changed; refetch the current revision";
          } else result = session.set_limits(c.limits, market_time_);
          break;
        case TradingCommand::Kind::Trip: result = session.trip_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Reset: result = session.reset_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Settle: {
          const auto it = session.contracts().find(c.symbol);
          if (it != session.contracts().end() && it->second.settlement != md::Settlement::AM)
            result.decision = {Reason::INVALID_SETTLEMENT, "PM settlement uses the provider closing print", {}, {}, {}};
          else {
            settlement_source_ = {{"kind", "manual_am_import"}, {"symbol", c.symbol}};
            result = session.settle(c.symbol, c.settlement, market_time_);
          }
          break;
        }
        case TradingCommand::Kind::ResetAccount:
          if (!c.required_pass.empty() && (before->evaluation.status != EvaluationStatus::Passed ||
                                           session.config().rules.plan != c.required_pass))
            result.decision = {Reason::PLAN_LOCKED, "Pass the " + c.required_pass + " evaluation to start this funded account",
                               {}, {}, {}};
          else result = session.reset_account(c.initial_cash, c.rules, c.reason, market_time_);
          break;
        case TradingCommand::Kind::Payout: result = session.request_payout(c.amount, market_time_); break;
        case TradingCommand::Kind::Annotate: result = session.annotate(c.trade, c.note, c.tags, market_time_); break;
        case TradingCommand::Kind::Exercise: {
          const auto contract = session.contracts().find(c.symbol);
          const auto gate = contract == session.contracts().end() ? Decision{} : acceptance(contract->second.underlying);
          if (gate.ok()) result = session.exercise(c.symbol, c.quantity, market_time_);
          else result.decision = gate;
          break;
        }
        case TradingCommand::Kind::CloseStock: {
          // Close all the shares, or the given number, in the direction that reduces them.
          Quantity held = 0;
          for (const auto& stock : session.snapshot()->stocks) if (stock.position.symbol == c.symbol) held = stock.position.shares;
          const Quantity shares = c.quantity > 0 ? c.quantity : (held < 0 ? -held : held);
          const auto gate = acceptance(c.symbol);
          if (held == 0) result.decision = {Reason::INVALID_ORDER, "The account holds no " + c.symbol + " shares", {}, {}, {}};
          else if (!gate.ok()) result.decision = gate;
          else result = session.trade_stock(c.symbol, held < 0 ? shares : -shares, market_time_);
          break;
        }
        case TradingCommand::Kind::CreateAccount: break;  // handled above
      }
      if (reply.error_code.empty()) reply.decision = result.decision;
      reply.order_id = result.order_id;
      publish_trading();
      const auto& orders = session.snapshot()->recent_orders;
      for (const auto& order : before->open_orders) {
        if (orders.at(static_cast<std::size_t>(order.id - 1)).status == OrderStatus::Cancelled)
          reply.cancelled_orders.push_back(order.id);
      }
      for (auto i = before->recent_orders.size(); i < orders.size(); ++i) reply.created_orders.push_back(orders[i].id);
    } catch (const TradingError& error) {
      reply.decision = {error.code(), error.what(), {}, {}, {}};
      if (error.code() == Reason::JOURNAL_IO || error.code() == Reason::JOURNAL_CORRUPT ||
          error.code() == Reason::JOURNAL_LOCKED) {
        fail_trading(*account, std::string(to_string(error.code())) + ": " + error.what());
        reply.error_code = "TRADING_UNAVAILABLE";
      }
    } catch (const std::exception& error) {
      fail_trading(*account, std::string("TRADING_UNAVAILABLE: ") + error.what());
      reply.error_code = "TRADING_UNAVAILABLE";
      reply.decision.message = account->failure;
    }
  }
  if (account) {
    reply.account = account->id;
    reply.view = trading_view(account->id);
  }
  // A disconnected consumer cannot take down the single owner of the ledger.
  try { pending.completion(std::move(reply)); } catch (...) {}
}
}  // namespace openport::server
