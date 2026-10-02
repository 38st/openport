#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/trading/money.hpp"
#include "openport/trading/shared.hpp"

namespace openport::trading {

using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using Timestamp = md::Timestamp;

enum class Reason {
  NONE, INVALID_MONEY, ARITHMETIC_OVERFLOW, INVALID_CONTRACT, AMERICAN_UNSUPPORTED,
  NONSTANDARD_UNSUPPORTED, ROOT_UNSUPPORTED, UNKNOWN_CONTRACT, INVALID_ORDER,
  DUPLICATE_CLIENT_ID, INVALID_TICK, INVALID_QUOTE, STALE_QUOTE, MISSING_VALUATION,
  MAX_ORDER_CONTRACTS, PRICE_BAND, DELTA_LIMIT, VEGA_LIMIT, DAILY_LOSS,
  KILL_SWITCH, RISK_CHANGED, IOC_REMAINDER, USER_CANCEL, SESSION_CLOSED, FEED_STALLED,
  DAY_END, EXPIRED, AWAITING_SETTLEMENT, INVALID_SETTLEMENT, ALREADY_SETTLED,
  UNKNOWN_ORDER, ORDER_TERMINAL, INVALID_LIMITS, INVALID_TIME, INVALID_SCENARIO,
  INVALID_REASON, JOURNAL_IO, JOURNAL_CORRUPT, JOURNAL_LOCKED,
  EVALUATION_CLOSED, BUYING_POWER, BUY_ONLY, EXPIRY_CUTOFF, ACCOUNT_RESET, INVALID_RULES,
  OCO_FILLED, POSITION_CLOSED, PAYOUT_UNAVAILABLE, PAYOUT_NOT_ELIGIBLE, INVALID_PAYOUT, PLAN_LOCKED,
  LIMIT_ONLY, INVALID_NOTE, UNKNOWN_TRADE, DEFINED_RISK, MARKET_HALTED, SOFT_FLOOR, TRADE_LIMIT, COOLDOWN, PROFIT_LOCK,
  RUN_ENDED
};
/// The last Reason; recorded codes are strings, so new codes append here.
inline constexpr Reason kLastReason = Reason::RUN_ENDED;
[[nodiscard]] std::string_view to_string(Reason reason) noexcept;

class TradingError : public std::runtime_error {
 public:
  TradingError(Reason code, std::string message) : std::runtime_error(message), code_(code) {}
  [[nodiscard]] Reason code() const noexcept { return code_; }
 private:
  Reason code_;
};

/// actual/limit are populated for numeric checks; scope is the underlying or "aggregate".
struct Decision {
  Reason code = Reason::NONE;
  std::string message;
  std::optional<double> actual;
  std::optional<double> limit;
  std::string scope;
  [[nodiscard]] bool ok() const { return code == Reason::NONE; }
};

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit };
enum class TimeInForce { Day, Ioc, Gtc };
/// Armed orders wait for their trigger; they are open (cancellable, reserving
/// risk and buying power) but never match until activated.
enum class OrderStatus { Working, PartiallyFilled, Filled, Cancelled, Rejected, Armed };

/// A price level that activates an order. Option triggers compare the order's
/// executable side (ask for buys, bid for sells); underlying triggers compare
/// the spot from the contract's fresh valuation. Levels are inclusive.
enum class TriggerSource { Option, Underlying, Combo };
enum class TriggerDirection { AtOrBelow, AtOrAbove };
struct Trigger {
  TriggerSource source = TriggerSource::Option;
  TriggerDirection direction = TriggerDirection::AtOrBelow;
  Money level;
  bool operator==(const Trigger&) const = default;
};
/// A bracket exit: a trigger makes it a market order when reached (a stop); a
/// limit price makes it a resting limit (a take-profit). Exactly one is set.
struct ExitSpec {
  std::optional<Trigger> trigger;
  std::optional<Money> limit_price;
  bool operator==(const ExitSpec&) const = default;
};
struct Bracket {
  std::optional<ExitSpec> stop_loss;
  std::optional<ExitSpec> take_profit;
  bool operator==(const Bracket&) const = default;
};

/// One leg of a multi-leg order: `ratio` contracts of `symbol` per unit, bought
/// or sold as `side` says.
struct Leg {
  std::string symbol;  ///< Canonical padded OSI of a registered definition.
  Side side = Side::Buy;
  Quantity ratio = 1;
  bool operator==(const Leg&) const = default;
};
inline constexpr std::size_t kMaxLegs = 4;
inline constexpr Quantity kMaxRatio = 10;

struct OrderRequest {
  std::string client_order_id;
  std::string symbol;  ///< Canonical padded OSI of a registered definition; empty with legs.
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Day;
  Quantity quantity = 0;
  std::optional<Money> limit_price;
  /// Conditional order: armed until reached, good until contract expiry.
  std::optional<Trigger> trigger;
  /// Exits created as this entry fills, one cancelling the other.
  std::optional<Bracket> bracket;
  /// Multi-leg order: two to four legs on one underlying, filled together. The
  /// symbol is empty and the side Buy; `quantity` counts units and
  /// `limit_price` is the net per unit: positive a debit to pay at most,
  /// negative a credit to receive at least.
  std::vector<Leg> legs;
  std::vector<std::string> tags = {};
  std::string note = {};
  /// Attach the bracket to held closing legs instead of submitting an entry.
  bool exits_only = false;
  bool operator==(const OrderRequest&) const = default;
};
[[nodiscard]] inline bool multi_leg(const OrderRequest& request) { return !request.legs.empty(); }
/// Every contract an order trades: its symbol, or each leg's.
[[nodiscard]] std::vector<std::string> order_symbols(const OrderRequest& request);
enum class OrderRole { Normal, StopLoss, TakeProfit };
/// A change asked of a resting order: the terms requested (each one left empty
/// kept), the terms the order had then, who asked and when, and why it was
/// refused (NONE when it was applied). A refused change leaves the order as it was.
struct OrderChangeRecord {
  Timestamp time = 0;
  std::string actor;
  std::optional<Quantity> quantity;
  std::optional<Money> limit_price;
  std::optional<Money> trigger_level;
  Quantity previous_quantity = 0;
  std::optional<Money> previous_limit_price;
  std::optional<Money> previous_trigger_level;
  Decision decision;
};
struct Order {
  OrderId id = 0;  ///< Also the acceptance priority sequence; never reused.
  OrderRequest request;
  OrderStatus status = OrderStatus::Working;
  Quantity filled_quantity = 0;
  Money filled_notional;  ///< Price times contracts, without multiplier/fees.
  Timestamp accepted_at = 0;
  Timestamp day_end = 0;
  Decision reason;
  /// Reducer-generated closing order (rule liquidation or expiry auto-close).
  /// Always market IOC against a fresh book; never user-submitted.
  bool system = false;
  OrderRole role = OrderRole::Normal;
  OrderId parent = 0;         ///< Bracket exits: the entry that created them.
  OrderId oco = 0;            ///< Bracket exits: the other exit, cancelled when this one fills.
  OrderId stop_loss = 0;      ///< Bracket entries: their stop-loss exit, once created.
  OrderId take_profit = 0;    ///< Bracket entries: their take-profit exit, once created.
  std::string actor = "unknown"; ///< Originating actor; absent in older journals.
  Timestamp triggered_at = 0; ///< When an armed order activated.
  /// The request as submitted, once a change or resize replaced its terms, so a
  /// retry of the submission still finds it; absent in older journals.
  std::optional<OrderRequest> submitted;
  /// When it stopped working: filled, cancelled or rejected; zero while open and
  /// for orders that ended before older journals kept it.
  Timestamp ended_at = 0;
  /// Every change asked of it while it rested, applied or refused, oldest first.
  std::vector<OrderChangeRecord> changes;
  /// A close the account works for the trader, as a flatten's are: it only reduces,
  /// is kept within the position like a bracket exit and works on later quotes
  /// until it fills, its session ends, the position closes or it is cancelled.
  bool reduce_only = false;
  [[nodiscard]] Quantity remaining() const { return request.quantity - filled_quantity; }
  /// The terms a retry must repeat to be answered with this order.
  [[nodiscard]] const OrderRequest& submission() const { return submitted ? *submitted : request; }
  [[nodiscard]] bool open() const {
    return status == OrderStatus::Working || status == OrderStatus::PartiallyFilled || status == OrderStatus::Armed;
  }
};
/// A trader's note and tags on one trade.
struct Annotation {
  std::string note;
  std::vector<std::string> tags;
  Timestamp time = 0;  ///< When it last changed.
};
struct DayNote {
  std::string plan;
  std::string review;
  Timestamp time = 0;
};
/// Observed immediately before execution; absent values were not available.
struct FillContext {
  std::optional<double> spot;
  std::string spot_source;
  std::optional<double> iv;
  std::optional<double> delta;
  std::optional<double> years;
  std::optional<Money> equity;
  std::optional<Money> floor_room;
  std::optional<Money> buying_power;
};
struct Excursion {
  Money pnl;  ///< Total marked P&L, including realised P&L and fees.
  Timestamp time = 0;
  std::optional<double> spot;
};
struct TradeReview {
  std::optional<Excursion> worst;
  std::optional<Excursion> best;
  std::optional<Money> planned_risk;
  bool finished = false;
};
/// The book a fill traded against, as the account held it: both sides and their
/// displayed sizes, what was left of the taken side's paper budget before the
/// fill (below zero once simulated impact depth is in use), and when the quote
/// was first given (`Fill::quote_time` is when it was last confirmed current).
struct FillQuote {
  std::optional<Money> bid;
  std::optional<Money> ask;
  Quantity bid_size = 0;
  Quantity ask_size = 0;
  Quantity left = 0;
  Timestamp quoted = 0;
};
struct Fill {
  std::uint64_t id = 0;
  OrderId order_id = 0;
  std::string symbol;
  Side side = Side::Buy;
  Quantity quantity = 0;
  Money price;
  Money fee;
  std::uint64_t observation = 0;
  Timestamp quote_time = 0;
  Timestamp time = 0;
  std::optional<FillContext> context = std::nullopt;
  std::string actor = "unknown";
  /// Absent on fills recorded before the book was kept with them.
  std::optional<FillQuote> quote = std::nullopt;
};

/// Observation numbers strictly increase per OSI. Repeated/older observations
/// never refresh liquidity. A repeated one with a later time confirms the quote is
/// still current: its time and mark's advance. Sizes are whole contracts, not lots.
struct QuoteObservation {
  std::string symbol;
  std::uint64_t observation = 0;
  Timestamp time = 0;
  std::optional<Money> bid;
  std::optional<Money> ask;
  Quantity bid_size = 0;
  Quantity ask_size = 0;
  /// When the quote was first given, if before `time`: a snapshot feed vouches
  /// for an unchanged quote at later times. Zero means `time`. A confirmation of
  /// the same observation at a later time keeps the first time here.
  Timestamp quoted = 0;
  /// When the quote was first given.
  [[nodiscard]] Timestamp first_time() const { return quoted > 0 ? quoted : time; }
};

/// One coherent caller valuation at strike smile IV. Greeks are per unit:
/// spot delta/gamma, vega per vol point, theta per calendar day.
struct Valuation {
  std::string symbol;
  Timestamp time = 0;
  double delta = 0;
  double gamma = 0;
  double vega = 0;
  double theta = 0;
  double spot = 0;
  double forward = 0;
  double discount = 0;
  double years = 0;
  double smile_iv = 0;
  bool valid = true;
  std::string spot_source = {};  ///< quote, parity, or empty when unknown.
};
/// Registered contract definitions and their latest valuations, by padded OSI.
using Contracts = SharedMap<std::string, md::OptionContract>;
using Valuations = SharedMap<std::string, Valuation>;

/// An underlying's price, for the shares that exercise and assignment deliver.
struct StockPrice {
  std::string symbol;
  Timestamp time = 0;
  Money price;
};

/// P&L explained by the Greeks, in dollars (analytic, not accounting). Each
/// stretch a position is held at one size is split by the Greeks at its start:
/// delta times the underlying's move, half gamma times the move squared, vega
/// times the implied volatility change in points and theta times the days that
/// passed. `other` is what they leave unexplained (larger moves, the smile, marks
/// without valuations); `costs` is the spread paid against the mark at each
/// fill, and fees.
struct Attribution {
  double delta = 0;
  double gamma = 0;
  double vega = 0;
  double theta = 0;
  double other = 0;
  double costs = 0;
  /// Some of it fell back to `other`: a stretch whose mark moved without valid
  /// valuations at both ends, so the Greeks could not split it.
  bool fallback = false;
  [[nodiscard]] double total() const { return delta + gamma + vega + theta + other + costs; }
  Attribution& operator+=(const Attribution& a) {
    delta += a.delta; gamma += a.gamma; vega += a.vega; theta += a.theta; other += a.other; costs += a.costs;
    fallback = fallback || a.fallback;
    return *this;
  }
};

struct Exposure {
  double dollar_delta = 0;
  double dollar_gamma_1pct = 0;
  double vega = 0;
  double theta = 0;
};
struct ExposureLimits {
  double dollar_delta = 1'000'000;
  double vega = 10'000;
};
struct Limits {
  Quantity max_order_contracts = 100;
  Money price_band_absolute = Money::from_micros(500'000);
  double price_band_relative = 0.20;
  ExposureLimits aggregate;
  ExposureLimits per_underlying;
  std::map<std::string, ExposureLimits> underlying_overrides;
  Money max_daily_loss = Money::from_micros(10'000'000'000);
  Timestamp max_quote_age = 60 * md::kNanosPerSecond;
  Timestamp max_valuation_age = 60 * md::kNanosPerSecond;
};
/// Personal rules, independent of the plan. Zero disables a field. A percentage
/// floor keeps that share of the plan's drawdown distance above its current floor.
struct Guardrails {
  Money soft_floor;
  std::int64_t soft_floor_percent = 0;
  Quantity max_opening_trades = 0;
  Money cooldown_loss;
  std::int64_t cooldown_minutes = 0;
  Money profit_lock;
  bool operator==(const Guardrails&) const = default;
};
struct GuardrailState {
  bool owns_kill = false;  ///< This rule, rather than a manual/daily-loss trip, owns the shared latch.
  Quantity opening_trades = 0;
  Timestamp cooldown_until = 0;
  std::vector<Reason> latched;
};
void validate_guardrails(const Guardrails& rules);
[[nodiscard]] Limits tightened_limits(const Limits& current, const Limits& requested);
[[nodiscard]] Guardrails tightened_guardrails(const Guardrails& current, const Guardrails& requested);

struct ScenarioConfig {
  std::vector<double> spot_percent{-10, -5, -2, -1, 0, 1, 2, 5, 10};
  std::vector<double> vol_points{-5, 0, 5, 10};
  double vol_floor = 0.0001;
};

/// How often the trailing drawdown floor may rise. Breaches are always
/// monitored on every transaction; the mode only controls the ratchet.
enum class DrawdownMode { Intraday, EndOfDay };
/// An evaluation passes on its target; a funded account pays out instead.
enum class Phase { Evaluation, Funded };
enum class MarginMode { Strategy, Portfolio };

/// Funded-account withdrawals. A qualifying day ends with at least
/// `qualifying_profit` of net realised profit; each payout needs
/// `qualifying_days` of them since the previous one. Percentages are whole
/// numbers so every amount stays exact.
struct PayoutRules {
  Money qualifying_profit;
  std::int64_t qualifying_days = 0;
  std::int64_t withdrawal_percent = 50;  ///< Share of profit one payout may take.
  std::int64_t split_percent = 80;       ///< Trader's share of each payout.
  Money minimum;
  std::vector<Money> caps;  ///< Per payout number; the last repeats; empty is uncapped.
  bool operator==(const PayoutRules&) const = default;
};

/// Evaluation-account rules. The defaults describe an unrestricted paper
/// account: no target, no drawdown floor, any side, no buying-power check.
struct AccountRules {
  std::string plan;           ///< Display name only, e.g. "Intraday 100K"; at most 64 bytes.
  Money profit_target;        ///< Dollars above the starting balance; zero disables.
  Money max_drawdown;         ///< Trailing distance below peak equity; zero disables.
  DrawdownMode drawdown_mode = DrawdownMode::Intraday;
  bool buy_only = false;      ///< Sells may only reduce existing long positions.
  /// Every short option must be covered by a long of the same type on the same
  /// underlying that expires with it or later: an order may not add a naked short.
  bool defined_risk = false;
  std::int64_t slippage_ticks = 0;  ///< Adverse ticks per option fill, from 0 to 10.
  std::int64_t fill_latency_ms = 0; ///< Market-time delay before execution, from 0 to 60,000 ms.
  std::int64_t impact_ticks = 0;    ///< Extra adverse ticks per displayed-size block, from 0 to 10.
  MarginMode margin = MarginMode::Strategy;
  bool buying_power = false;  ///< Enforce cash buying power under the selected margin mode.
  Timestamp expiry_cutoff = 0;  ///< Auto-close this long before a contract's last trade; zero disables.
  Phase phase = Phase::Evaluation;
  Money lock_balance;         ///< Once the floor reaches it, the floor stops trailing; zero disables.
  PayoutRules payouts;        ///< Funded phase only.
  [[nodiscard]] bool evaluation() const { return profit_target > Money{} || max_drawdown > Money{}; }
  bool operator==(const AccountRules&) const = default;
};

struct SessionConfig {
  Money initial_cash = Money::from_micros(100'000'000'000);
  Money fee_per_contract = Money::from_micros(650'000);
  Limits limits;
  ScenarioConfig scenarios;
  AccountRules rules;
  Guardrails guardrails;
};

[[nodiscard]] Decision eligible(const md::OptionContract& contract);
/// v1 policy, not an exchange routing rule; includes the >= $3 upper tier.
[[nodiscard]] Money tick_size(std::string_view root, Money price);
[[nodiscard]] bool valid_quote(const QuoteObservation& quote);
/// A quote a position can be marked at: a valid one, or an ask with no bid (a far
/// option nobody bids for), marked halfway to the ask. Only valid quotes trade.
[[nodiscard]] bool markable_quote(const QuoteObservation& quote);
[[nodiscard]] bool valid_valuation(const Valuation& valuation);
void validate_limits(const Limits& limits);
/// Money amounts nonnegative, cutoff within [0, 1 day), plan name at most 64 bytes,
/// payout percentages 0-100 with positive caps; a funded phase has no profit
/// target and needs at least one qualifying day; slippage is 0-10 ticks.
void validate_rules(const AccountRules& rules);

}  // namespace openport::trading
