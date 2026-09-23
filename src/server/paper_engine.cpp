#include "openport/server/engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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
}  // namespace

std::shared_ptr<const TradingView> Engine::trading_view() const {
  const std::lock_guard lock(mutex_);
  return trading_view_;
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
  try {
    std::shared_ptr<Journal> journal = options_.paper_sink;
    std::optional<JournalRecovery> recovery;
    if (!options_.paper_journal.empty()) {
      const auto parent = std::filesystem::absolute(options_.paper_journal).parent_path();
      auto existing = parent;
      while (!std::filesystem::exists(existing)) existing = existing.parent_path();
      std::filesystem::create_directories(parent);
      for (auto path = parent; path != existing; path = path.parent_path()) sync_directory(path);
      sync_directory(existing);
      if (std::filesystem::exists(options_.paper_journal)) {
        // Lock before reading, then recover the same verified head held by the writer.
        journal = FileJournal::resume(options_.paper_journal.string());
        recovery = FileJournal::read(options_.paper_journal.string());
      } else {
        journal = FileJournal::create(options_.paper_journal.string());
        sync_directory(parent);
      }
    }
    if (journal) journal = std::make_shared<SettlementJournal>(journal, settlement_source_);
    if (recovery) trading_ = std::make_unique<TradingSession>(TradingSession::recover(*recovery, journal));
    if (!trading_) trading_ = std::make_unique<TradingSession>(options_.paper, 0, journal);
    market_time_ = trading_->snapshot()->time;
    for (const auto& [symbol, contract] : trading_->contracts()) {
      if (const auto quote = trading_->quote(symbol)) observations_[symbol] = quote->observation;
    }
    publish_trading();
    const std::lock_guard lock(command_mutex_);
    accepting_commands_ = !stopping_;
  } catch (const TradingError& error) {
    fail_trading(std::string(to_string(error.code())) + ": " + error.what());
  } catch (const std::exception& error) {
    fail_trading(std::string("JOURNAL_IO: ") + error.what());
  }
}

void Engine::publish_trading() {
  std::shared_ptr<TradingView> view;
  if (trading_) {
    view = std::make_shared<TradingView>();
    view->snapshot = trading_->snapshot();
    view->config = trading_->config();
    view->contracts = trading_->contracts();
    view->valuations = trading_->valuations();
  }
  const std::lock_guard lock(mutex_);
  trading_view_ = std::move(view);
  auto& status = status_.trading;
  status.enabled = options_.paper_enabled;
  status.reason = trading_failure_;
  status.write = trading_failure_.empty() ? options_.write_mode : "disabled";
  if (trading_view_) {
    status.account_version = trading_view_->snapshot->account_version;
    status.kill_latched = trading_view_->snapshot->risk.kill_latched;
  }
}

void Engine::fail_trading(std::string reason) {
  trading_failure_ = std::move(reason);
  {
    const std::lock_guard lock(command_mutex_);
    accepting_commands_ = false;
  }
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
        fail_trading("ARITHMETIC_OVERFLOW: quote observation exhausted");
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
  if (!trading_ || !trading_failure_.empty()) return;
  if (batch.empty() && commands.empty()) return;
  std::set<std::string> symbols;
  for (const auto& p : trading_->snapshot()->positions) symbols.insert(p.position.contract.osi_symbol());
  for (const auto& order : trading_->snapshot()->open_orders) symbols.insert(order.request.symbol);
  for (const auto& pending : commands) {
    if (pending.command.kind == TradingCommand::Kind::Submit) symbols.insert(pending.command.order.symbol);
    if (pending.command.kind == TradingCommand::Kind::Settle) symbols.insert(pending.command.symbol);
  }
  std::vector<QuoteObservation> quotes;
  std::vector<Valuation> valuations;
  for (const auto& symbol : symbols) {
    const auto id = instruments_.find(symbol);
    const auto* option = id == instruments_.end() ? nullptr : book_.option(id->second);
    if (option && !trading_->contracts().contains(symbol)) {
      const auto result = trading_->define(option->contract, market_time_);
      if (!result.decision.ok()) continue;
    }
    const auto definition = trading_->contracts().find(symbol);
    if (definition == trading_->contracts().end()) continue;
    if (option) {
      const auto& a = option->contract;
      const auto& b = definition->second;
      if (a.root != b.root || a.underlying != b.underlying || a.expiry != b.expiry ||
          a.strike != b.strike || a.type != b.type || a.style != b.style ||
          a.settlement != b.settlement || a.multiplier != b.multiplier || a.standard != b.standard)
        throw TradingError(Reason::INVALID_CONTRACT, "INVALID_CONTRACT: listed terms conflict with registered definition");
    }
    if (option && option->has_quote && option->quote_ts >= 0) {
      quotes.push_back({symbol, observations_[symbol], option->quote_ts,
                        quote_price(option->bid), quote_price(option->ask),
                        whole_size(option->bid_size), whole_size(option->ask_size)});
    }
    auto valuation = valuation_for(symbol, definition->second, metrics(definition->second.underlying));
    // Missing live analytics after recovery must not overwrite a recorded frame.
    if (valuation.time > 0) valuations.push_back(std::move(valuation));
  }
  trading_->on_quotes(quotes, valuations, market_time_);
  // Underlying prints retain ingress order. The first valid post-expiry last on
  // the expiry date is our documented PM closing-print approximation.
  for (const auto& event : batch) {
    const auto* spot = std::get_if<md::UnderlyingQuote>(&event);
    if (!spot || !std::isfinite(spot->last) || spot->last <= 0) continue;
    const auto snapshot = trading_->snapshot();
    for (const auto& p : snapshot->positions) {
      const auto& contract = p.position.contract;
      if (contract.settlement == md::Settlement::PM && contract.underlying == spot->symbol &&
          spot->ts >= contract.expiry_time() && new_york_date(spot->ts) == contract.expiry) {
        settlement_source_ = {{"kind", "provider_closing_print"}, {"provider", std::string(provider_.name())},
                              {"symbol", spot->symbol}, {"quote_time", md::format_timestamp(spot->ts)}};
        trading_->settle(contract.osi_symbol(), Money::from_double(spot->last), market_time_);
      }
    }
  }
  const auto day = new_york_date(market_time_);
  if (!batch.empty() && day > trading_->trading_day() &&
      md::market_session(md::new_york_to_utc(day, 12, 0)).open &&
      trading_->snapshot()->valuation_complete) trading_->roll_day(market_time_);
  publish_trading();
}

void Engine::apply_command(PendingCommand& pending) {
  TradingReply reply;
  if (!trading_ || !trading_failure_.empty() || stopping_) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = trading_failure_.empty() ? "Trading is disabled or engine is stopping" : trading_failure_;
  } else {
    const auto before = trading_->snapshot();
    const auto& c = pending.command;
    try {
      CommandResult result;
      switch (c.kind) {
        case TradingCommand::Kind::Submit: {
          Decision rejection;
          const auto id = instruments_.find(c.order.symbol);
          if (id != instruments_.end()) {
            if (const auto* option = book_.option(id->second)) rejection = eligible(option->contract);
          }
          result = trading_->submit(c.order, market_time_, rejection);
          break;
        }
        case TradingCommand::Kind::Cancel: result = trading_->cancel(c.order_id, market_time_); break;
        case TradingCommand::Kind::Limits:
          if (c.expected_revision != before->risk.limits_revision) {
            reply.error_code = "LIMITS_REVISION";
            reply.decision.message = "Limits changed; refetch the current revision";
          } else result = trading_->set_limits(c.limits, market_time_);
          break;
        case TradingCommand::Kind::Trip: result = trading_->trip_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Reset: result = trading_->reset_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Settle: {
          const auto it = trading_->contracts().find(c.symbol);
          if (it != trading_->contracts().end() && it->second.settlement != md::Settlement::AM)
            result.decision = {Reason::INVALID_SETTLEMENT, "PM settlement uses the provider closing print", {}, {}, {}};
          else {
            settlement_source_ = {{"kind", "manual_am_import"}, {"symbol", c.symbol}};
            result = trading_->settle(c.symbol, c.settlement, market_time_);
          }
          break;
        }
      }
      if (reply.error_code.empty()) reply.decision = result.decision;
      reply.order_id = result.order_id;
      publish_trading();
      for (const auto& order : before->open_orders) {
        const auto& orders = trading_->snapshot()->recent_orders;
        if (orders.at(static_cast<std::size_t>(order.id - 1)).status == OrderStatus::Cancelled)
          reply.cancelled_orders.push_back(order.id);
      }
    } catch (const TradingError& error) {
      reply.decision = {error.code(), error.what(), {}, {}, {}};
      if (error.code() == Reason::JOURNAL_IO || error.code() == Reason::JOURNAL_CORRUPT) {
        fail_trading(std::string(to_string(error.code())) + ": " + error.what());
        reply.error_code = "TRADING_UNAVAILABLE";
      }
    } catch (const std::exception& error) {
      fail_trading(std::string("TRADING_UNAVAILABLE: ") + error.what());
      reply.error_code = "TRADING_UNAVAILABLE";
      reply.decision.message = trading_failure_;
    }
  }
  reply.view = trading_view();
  // A disconnected consumer cannot take down the single owner of the ledger.
  try { pending.completion(std::move(reply)); } catch (...) {}
}
}  // namespace openport::server
