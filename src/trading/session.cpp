#include "openport/trading/session.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

#include "state.hpp"

namespace openport::trading {
namespace {
using detail::State;
using Events = std::vector<Json>;
Decision failure(Reason code, std::string message) { return {code, std::move(message), {}, {}, {}}; }
void event(Events& events, std::string_view type, Json payload) {
  events.push_back(Json{{"type", type}, {"payload", std::move(payload)}});
}
md::Date local_date(Timestamp time) {
  // At regular trading hours UTC and New York share the same date; this also
  // handles explicit overnight rollover commands using the correct DST offset.
  auto date = md::date_from_days(time / md::kNanosPerDay);
  const auto offset = md::new_york_utc_offset_hours(date, 0);
  auto seconds = time / md::kNanosPerSecond + offset * 3600;
  auto days = seconds / 86400;
  if (seconds < 0 && seconds % 86400 != 0) --days;
  return md::date_from_days(days);
}
Timestamp regular_end(const md::OptionContract& c, Timestamp time) {
  const auto date = local_date(time);
  // The calendar defines every supported index's regular session through :15.
  const auto end = md::new_york_to_utc(date, md::regular_close_hour(date), 15);
  if (end == md::kInvalidTimestamp || md::trading_session(c.root, end - 1).name != "regular" ||
      md::trading_session(c.root, end).name == "regular")
    throw TradingError(Reason::SESSION_CLOSED, "No representable regular session end");
  return end;
}
Money mid(const QuoteObservation& q) {
  // Both sides are positive. Difference-first avoids overflowing their sum.
  return *q.bid + (*q.ask - *q.bid).prorate(1, 2);
}
Decision quote_check(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !valid_quote(it->second.quote))
    return failure(Reason::INVALID_QUOTE, "A noncrossed positive two-sided quote with both sizes is required");
  const auto time = it->second.quote.time;
  if (time > s.time || s.time - time > s.config.limits.max_quote_age)
    return failure(Reason::STALE_QUOTE, "Quote is outside the configured market-time freshness window");
  return {};
}
Decision price_check(const State& s, const QuoteObservation& q, Money price) {
  const auto middle = mid(q);
  const long double difference = std::abs(static_cast<long double>(price.micros()) - static_cast<long double>(middle.micros()));
  const long double band = std::max(static_cast<long double>(s.config.limits.price_band_absolute.micros()),
      static_cast<long double>(s.config.limits.price_band_relative) * static_cast<long double>(middle.micros()));
  if (difference > band)
    return {Reason::PRICE_BAND, "Price is outside the configured band around mid",
            static_cast<double>(difference / 1'000'000), static_cast<double>(band / 1'000'000), q.symbol};
  return {};
}
TradingSnapshot snapshot_of(const State& s) {
  TradingSnapshot out;
  out.account_version = s.version;
  out.time = s.time;
  out.account = s.ledger.account();
  out.equity = out.account.cash;
  out.start_of_day_equity = s.start_equity;
  out.recent_orders = s.orders;
  out.recent_fills = s.fills;
  for (const auto& order : s.orders) if (order.open()) out.open_orders.push_back(order);
  for (const auto& [symbol, position] : s.ledger.positions()) {
    MarkedPosition p;
    p.position = position;
    p.awaiting_settlement = s.time >= position.contract.expiry_time();
    const auto it = s.marks.find(symbol);
    if (it != s.marks.end()) {
      p.mark = it->second.price;
      p.mark_time = it->second.time;
      p.mark_age = s.time - p.mark_time;
      p.market_value = (*p.mark * 100) * position.quantity;
      p.unrealised = *p.market_value - position.basis;
      p.fresh = !p.awaiting_settlement && quote_check(s, symbol).ok();
      out.equity = out.equity + *p.market_value;
      out.unrealised = out.unrealised + *p.unrealised;
    }
    out.valuation_complete &= p.fresh;
    out.positions.push_back(std::move(p));
  }
  out.risk = portfolio_risk(s.ledger, s.orders, s.contracts, s.valuations, s.config.limits, s.time);
  out.risk.daily_loss = std::max(Money{}, s.start_equity - out.equity);
  out.risk.kill_latched = s.kill;
  out.risk.kill_reason = s.kill_reason;
  out.risk.limits_revision = s.limits_revision;
  out.scenarios = scenario_grid(s.ledger, s.valuations, s.config.scenarios, s.time, s.config.limits.max_valuation_age);
  if (!out.valuation_complete) out.quality_flags.push_back(Reason::STALE_QUOTE);
  if (!out.risk.complete || !out.scenarios.complete) out.quality_flags.push_back(Reason::MISSING_VALUATION);
  if (std::any_of(out.positions.begin(), out.positions.end(), [](const auto& p) { return p.awaiting_settlement; }))
    out.quality_flags.push_back(Reason::AWAITING_SETTLEMENT);
  return out;
}
void cancel_order(Order& order, Decision reason, Events& events) {
  if (!order.open()) return;
  order.status = OrderStatus::Cancelled;
  order.reason = std::move(reason);
  event(events, "cancel", order);
}
void trip(State& s, const std::string& reason, Events& events) {
  if (!s.kill) {
    s.kill = true;
    s.kill_reason = reason;
    event(events, "kill_trip", Json{{"reason", reason}});
  }
  for (auto& o : s.orders) cancel_order(o, failure(Reason::KILL_SWITCH, s.kill_reason), events);
}
Decision loss_check(const State& s, const TradingSnapshot& snapshot) {
  if (snapshot.risk.daily_loss > s.config.limits.max_daily_loss)
    return {Reason::DAILY_LOSS, "Marked loss from start-of-day equity exceeds limit",
        snapshot.risk.daily_loss.dollars(), s.config.limits.max_daily_loss.dollars(), "aggregate"};
  return {};
}
void monitor_loss(State& s, Events& events) {
  const auto snapshot = snapshot_of(s);
  if (!loss_check(s, snapshot).ok()) trip(s, "DAILY_LOSS", events);
}
void advance(State& s, Timestamp time, Events& events) {
  if (time < 0 || time < s.time) throw TradingError(Reason::INVALID_TIME, "Market time must be nonnegative and monotone");
  s.time = time;
  for (auto& o : s.orders) {
    if (!o.open()) continue;
    const auto& contract = s.contracts.at(o.request.symbol);
    if (time >= contract.expiry_time()) cancel_order(o, failure(Reason::EXPIRED, "Contract reached expiry and awaits settlement"), events);
    else if (time >= o.day_end) cancel_order(o, failure(Reason::DAY_END, "Regular session ended"), events);
  }
}
Decision order_check(const State& s, const Order& o, bool at_fill = false) {
  if (s.kill) return failure(Reason::KILL_SWITCH, s.kill_reason);
  const auto& request = o.request;
  const auto c = s.contracts.find(request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (md::trading_session(c->second.root, s.time).name != "regular")
    return failure(Reason::SESSION_CLOSED, "v1 accepts and executes only in the product regular session");
  if (request.client_order_id.empty() || request.quantity <= 0 ||
      (request.side != Side::Buy && request.side != Side::Sell) ||
      (request.type != OrderType::Market && request.type != OrderType::Limit) ||
      (request.tif != TimeInForce::Day && request.tif != TimeInForce::Ioc) ||
      (request.type == OrderType::Market && (request.tif != TimeInForce::Ioc || request.limit_price)) ||
      (request.type == OrderType::Limit && (!request.limit_price || *request.limit_price <= Money{})))
    return failure(Reason::INVALID_ORDER, "Positive quantity/client ID required; market is IOC without price; limit requires positive price");
  if (request.quantity > s.config.limits.max_order_contracts)
    return {Reason::MAX_ORDER_CONTRACTS, "Order contract count exceeds limit", static_cast<double>(request.quantity),
            static_cast<double>(s.config.limits.max_order_contracts), request.symbol};
  if (request.limit_price && request.limit_price->micros() % tick_size(c->second.root, *request.limit_price).micros() != 0)
    return failure(Reason::INVALID_TICK, "Limit price is not a positive multiple of the product tier tick");
  if (const auto d = quote_check(s, request.symbol); !d.ok()) return d;
  const auto& quote = s.books.at(request.symbol).quote;
  const auto price = !at_fill && request.limit_price ? *request.limit_price : (request.side == Side::Buy ? *quote.ask : *quote.bid);
  if (const auto d = price_check(s, quote, price); !d.ok()) return d;
  const auto snapshot = snapshot_of(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (const auto d = loss_check(s, snapshot); !d.ok()) return d;
  return check_exposure(snapshot.risk);
}
bool marketable(const Order& o, const QuoteObservation& q) {
  if (o.request.type == OrderType::Market) return true;
  return o.request.side == Side::Buy ? *q.ask <= *o.request.limit_price : *q.bid >= *o.request.limit_price;
}
void match_one(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open()) return;
  if (incoming != id && s.books.at(o.request.symbol).quote.time < o.accepted_at) return;
  if (!quote_check(s, o.request.symbol).ok() || !marketable(o, s.books.at(o.request.symbol).quote)) return;
  auto decision = order_check(s, o, true);
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(o, decision, events);
    return;
  }
  auto& book = s.books.at(o.request.symbol);
  if (!marketable(o, book.quote)) return;
  const Money price = o.request.side == Side::Buy ? *book.quote.ask : *book.quote.bid;
  auto& budget = o.request.side == Side::Buy ? book.ask_left : book.bid_left;
  const Quantity quantity = std::min(o.remaining(), budget);
  if (quantity <= 0) return;
  decision = price_check(s, book.quote, price);
  const Money fee = s.config.fee_per_contract * quantity;
  if (decision.ok()) {
    // Check the proposed accounting before committing any liquidity or fill.
    State projected = s;
    projected.ledger.fill(s.contracts.at(o.request.symbol), o.request.side == Side::Buy ? quantity : -quantity, price, fee);
    decision = loss_check(projected, snapshot_of(projected));
  }
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(o, decision, events);
    return;
  }
  s.ledger.fill(s.contracts.at(o.request.symbol), o.request.side == Side::Buy ? quantity : -quantity, price, fee);
  budget -= quantity;
  o.filled_quantity += quantity;
  o.filled_notional = o.filled_notional + price * quantity;
  o.status = o.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, o.request.symbol, o.request.side,
            quantity, price, fee, book.quote.observation, book.quote.time, s.time};
  s.fills.push_back(fill);
  event(events, "fill", fill);
}
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming = {}) {
  for (const auto& symbol : symbols) {
    for (const auto side : {Side::Buy, Side::Sell}) {
      std::vector<OrderId> priority;
      for (const auto& o : s.orders)
        if (o.open() && o.request.symbol == symbol && o.request.side == side) priority.push_back(o.id);
      std::stable_sort(priority.begin(), priority.end(), [&](OrderId a, OrderId b) {
        const auto& x = s.orders.at(static_cast<std::size_t>(a - 1));
        const auto& y = s.orders.at(static_cast<std::size_t>(b - 1));
        if (x.request.type != y.request.type) return x.request.type == OrderType::Market;
        if (x.request.limit_price == y.request.limit_price) return a < b;
        return side == Side::Buy ? x.request.limit_price > y.request.limit_price : x.request.limit_price < y.request.limit_price;
      });
      for (auto id : priority) match_one(s, id, events, incoming);
    }
  }
}
void require_reason(const std::string& reason) {
  if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
    throw TradingError(Reason::INVALID_REASON, "An explicit nonblank reason is required");
}
}  // namespace

struct TradingSession::Impl {
  State state;
  std::shared_ptr<Journal> journal;
  std::shared_ptr<const TradingSnapshot> snapshot;
  bool stopped = false;

  CommandResult transact(Timestamp time, std::string_view type,
                         const std::function<CommandResult(State&, Events&)>& action) {
    if (stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
    State next = state;
    Events events;
    advance(next, time, events);
    auto result = action(next, events);
    monitor_loss(next, events);
    if (next.version == std::numeric_limits<std::uint64_t>::max())
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Account version exhausted");
    ++next.version;
    auto publication = std::make_shared<TradingSnapshot>(snapshot_of(next));
    if (journal) {
      const Json payload{{"schema", 1}, {"tick_policy", "index-v1"}, {"events", events},
                         {"state", next}, {"snapshot", *publication}, {"decision", result.decision}};
      try { journal->append(time, type, payload.dump()); }
      catch (...) {
        stopped = true;
        auto failed = std::make_shared<TradingSnapshot>(*snapshot);
        failed->journal_failed = true;
        failed->quality_flags.push_back(Reason::JOURNAL_IO);
        snapshot = std::move(failed);
        throw TradingError(Reason::JOURNAL_IO, "Journal commit failed; no in-memory transition published; stop trading and recover");
      }
    }
    state = std::move(next);
    snapshot = std::move(publication);
    result.account_version = state.version;
    return result;
  }
};
TradingSession::TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal)
    : impl_(std::make_unique<Impl>()) {
  validate_limits(config.limits);
  validate_scenarios(config.scenarios);
  if (time < 0) throw TradingError(Reason::INVALID_TIME, "Negative session time");
  if (config.fee_per_contract < Money{}) throw TradingError(Reason::INVALID_MONEY, "Fee cannot be negative");
  if (journal && journal->sequence() != 0) throw TradingError(Reason::JOURNAL_CORRUPT, "Use recover for a nonempty journal");
  impl_->state.config = std::move(config);
  impl_->state.time = time;
  impl_->state.day = local_date(time);
  impl_->state.ledger = Ledger(impl_->state.config.initial_cash);
  impl_->state.start_equity = impl_->state.config.initial_cash;
  impl_->journal = std::move(journal);
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
  impl_->transact(time, "session_start", [](State& s, Events& events) {
    event(events, "session_start", s.config);
    return CommandResult{};
  });
}
TradingSession::TradingSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TradingSession::~TradingSession() = default;
TradingSession::TradingSession(TradingSession&&) noexcept = default;
TradingSession& TradingSession::operator=(TradingSession&&) noexcept = default;
CommandResult TradingSession::define(const md::OptionContract& contract, Timestamp time) {
  return impl_->transact(time, "definition", [&](State& s, Events& events) {
    const auto decision = eligible(contract);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    const auto symbol = contract.osi_symbol();
    const auto old = s.contracts.find(symbol);
    if (old != s.contracts.end() && Json(old->second) != Json(contract))
      return CommandResult{failure(Reason::INVALID_CONTRACT, "OSI definition conflicts with registered terms"), {}, 0};
    s.contracts[symbol] = contract;
    event(events, "definition", contract);
    return CommandResult{};
  });
}
CommandResult TradingSession::submit(OrderRequest request, Timestamp time, Decision rejection) {
  return impl_->transact(time, "submit", [&](State& s, Events& events) {
    monitor_loss(s, events);
    Order order;
    order.id = static_cast<OrderId>(s.orders.size() + 1);
    order.request = std::move(request);
    order.accepted_at = time;
    const bool duplicate = std::any_of(s.orders.begin(), s.orders.end(), [&](const auto& o) {
      return o.request.client_order_id == order.request.client_order_id;
    });
    s.orders.push_back(order);
    auto& stored = s.orders.back();
    auto decision = duplicate ? failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already used") : !rejection.ok() ? rejection : order_check(s, stored);
    if (!decision.ok()) {
      stored.status = OrderStatus::Rejected;
      stored.reason = decision;
      event(events, "order_rejected", stored);
      return CommandResult{decision, stored.id, 0};
    }
    stored.day_end = regular_end(s.contracts.at(stored.request.symbol), time);
    event(events, "order_accepted", stored);
    // Existing better orders share any remaining budget even on command ingress.
    match_symbols(s, {stored.request.symbol}, events, stored.id);
    if (stored.open() && stored.request.tif == TimeInForce::Ioc)
      cancel_order(stored, failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
    return CommandResult{{}, stored.id, 0};
  });
}
CommandResult TradingSession::cancel(OrderId id, Timestamp time) {
  return impl_->transact(time, "cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
    auto& order = s.orders.at(static_cast<std::size_t>(id - 1));
    if (!order.open()) return CommandResult{failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
    cancel_order(order, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{{}, id, 0};
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time) {
  return impl_->transact(time, "market", [&](State& s, Events& events) {
    std::set<std::string> seen;
    std::set<std::string> changed;
    for (const auto& quote : quotes) {
      if (!s.contracts.contains(quote.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Quote references unregistered OSI");
      if (quote.time < 0 || quote.time > time) throw TradingError(Reason::INVALID_TIME, "Quote is future-dated or negative");
      if (!seen.insert(quote.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One observation per contract per batch is required");
      auto& book = s.books[quote.symbol];
      if (quote.observation <= book.quote.observation || quote.time < book.quote.time) continue;
      book = {quote, valid_quote(quote) ? quote.bid_size : 0, valid_quote(quote) ? quote.ask_size : 0};
      changed.insert(quote.symbol);
      if (valid_quote(quote) && time - quote.time <= s.config.limits.max_quote_age)
        s.marks[quote.symbol] = {mid(quote), quote.time};
    }
    seen.clear();
    for (auto valuation : valuations) {
      if (!s.contracts.contains(valuation.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Valuation references unregistered OSI");
      if (valuation.time < 0 || valuation.time > time) throw TradingError(Reason::INVALID_TIME, "Valuation is future-dated or negative");
      if (!seen.insert(valuation.symbol).second) throw TradingError(Reason::MISSING_VALUATION, "One valuation per contract per batch is required");
      const auto prior = s.valuations.find(valuation.symbol);
      if (prior != s.valuations.end() && valuation.time < prior->second.time) continue;
      if (!valid_valuation(valuation)) {
        // Null/nonfinite data is explicitly invalid, never implicitly zero risk.
        valuation = Valuation{valuation.symbol, valuation.time, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
      }
      s.valuations[valuation.symbol] = valuation;
    }
    monitor_loss(s, events);
    // Invalid quotes provide no liquidity. Keep orders until a new valid quote
    // permits the fill-time risk check (or a clock/kill command cancels them).
    for (auto it = changed.begin(); it != changed.end();) {
      if (!quote_check(s, *it).ok()) it = changed.erase(it); else ++it;
    }
    if (!s.kill) match_symbols(s, changed, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::set_limits(Limits limits, Timestamp time) {
  validate_limits(limits);
  return impl_->transact(time, "limit_change", [&](State& s, Events& events) {
    s.config.limits = std::move(limits);
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "limit_change", s.config.limits);
    monitor_loss(s, events);
    for (auto& order : s.orders) {
      if (!order.open()) continue;
      auto d = order_check(s, order);
      if (!d.ok()) {
        d.message = std::string(to_string(d.code)) + ": " + d.message;
        d.code = Reason::RISK_CHANGED;
        cancel_order(order, d, events);
      }
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::trip_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_trip", [&](State& s, Events& events) {
    trip(s, reason, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_reset", [&](State& s, Events& events) {
    s.kill = false;
    s.kill_reason.clear();
    event(events, "kill_reset", Json{{"reason", reason}});
    monitor_loss(s, events);
    if (s.kill) return CommandResult{loss_check(s, snapshot_of(s)), {}, 0};
    return CommandResult{};
  });
}
CommandResult TradingSession::settle(const std::string& symbol, Money settlement, Timestamp time) {
  return impl_->transact(time, "settlement", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Unknown settlement contract"), {}, 0};
    if (s.settled.contains(symbol)) return CommandResult{failure(Reason::ALREADY_SETTLED, "Settlement is exactly once per OSI"), {}, 0};
    if (time < it->second.expiry_time() || settlement < Money{} || !s.ledger.positions().contains(symbol))
      return CommandResult{failure(Reason::INVALID_SETTLEMENT, "Settlement requires expiry, a position and nonnegative reference"), {}, 0};
    const Money strike = Money::from_double(it->second.strike);
    const Money intrinsic = std::max(Money{}, it->second.type == pricing::OptionType::Call ? settlement - strike : strike - settlement);
    s.ledger.settle(symbol, intrinsic);
    s.settled.insert(symbol);
    event(events, "settlement", Json{{"symbol", symbol}, {"reference", settlement}, {"intrinsic", intrinsic}});
    return CommandResult{};
  });
}
CommandResult TradingSession::roll_day(Timestamp time) {
  return impl_->transact(time, "day_rollover", [&](State& s, Events& events) {
    const auto day = local_date(time);
    if (day <= s.day) return CommandResult{failure(Reason::INVALID_TIME, "Rollover requires a later New York date"), {}, 0};
    monitor_loss(s, events);
    const auto snapshot = snapshot_of(s);
    if (!snapshot.valuation_complete) return CommandResult{failure(Reason::STALE_QUOTE, "Rollover requires complete marked equity"), {}, 0};
    s.start_equity = snapshot.equity;
    s.day = day;
    event(events, "day_rollover", Json{{"day", day}, {"equity", s.start_equity}});
    return CommandResult{};
  });
}
std::shared_ptr<const TradingSnapshot> TradingSession::snapshot() const { return impl_->snapshot; }
std::string TradingSession::snapshot_json() const { return Json(*impl_->snapshot).dump(); }
const SessionConfig& TradingSession::config() const { return impl_->state.config; }
const std::map<std::string, md::OptionContract>& TradingSession::contracts() const { return impl_->state.contracts; }
const std::map<std::string, Valuation>& TradingSession::valuations() const { return impl_->state.valuations; }
std::optional<QuoteObservation> TradingSession::quote(const std::string& symbol) const {
  const auto it = impl_->state.books.find(symbol);
  return it == impl_->state.books.end() ? std::nullopt : std::optional(it->second.quote);
}
md::Date TradingSession::trading_day() const { return impl_->state.day; }
TradingSession TradingSession::recover(const JournalRecovery& recovery, std::shared_ptr<Journal> journal) {
  if (recovery.records.empty()) throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery requires a session_start record");
  // Reverify even caller-constructed recovery objects instead of trusting them.
  std::string lines;
  try {
    for (const auto& r : recovery.records) {
      lines += Json{{"seq", r.seq}, {"time", r.time}, {"type", r.type}, {"payload", Json::parse(r.payload)},
                    {"prev_hash", r.prev_hash}, {"hash", r.hash}}.dump() + '\n';
    }
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  const auto verified = verify_journal(lines, recovery.head);
  if (verified.records.front().type != "session_start") throw TradingError(Reason::JOURNAL_CORRUPT, "Missing session start");
  if (journal && (recovery.truncated_final_line || journal->head() != verified.head || journal->sequence() != verified.records.size()))
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery sink does not match verified journal head");
  auto impl = std::make_unique<Impl>();
  try {
    for (const auto& r : verified.records) {
      const auto payload = Json::parse(r.payload);
      if (payload.at("schema") != 1 || payload.at("tick_policy") != "index-v1")
        throw TradingError(Reason::JOURNAL_CORRUPT, "Unsupported trading journal schema/policy");
      auto state = payload.at("state").get<State>();
      auto snapshot = payload.at("snapshot").get<TradingSnapshot>();
      if (state.version != r.seq || state.time != r.time || snapshot.account_version != state.version || snapshot.time != state.time)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded state version/time does not match transaction");
      validate_limits(state.config.limits);
      validate_scenarios(state.config.scenarios);
      impl->state = std::move(state);
      impl->snapshot = std::make_shared<TradingSnapshot>(std::move(snapshot));
    }
  } catch (const TradingError& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  impl->journal = std::move(journal);
  return TradingSession(std::move(impl));
}
}  // namespace openport::trading
