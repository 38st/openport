#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/trading/money.hpp"

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
  LIMIT_ONLY, INVALID_NOTE, UNKNOWN_TRADE
};
/// The last Reason; recorded codes are strings, so new codes append here.
inline constexpr Reason kLastReason = Reason::UNKNOWN_TRADE;
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
enum class TimeInForce { Day, Ioc };
/// Armed orders wait for their trigger; they are open (cancellable, reserving
/// risk and buying power) but never match until activated.
enum class OrderStatus { Working, PartiallyFilled, Filled, Cancelled, Rejected, Armed };

/// A price level that activates an order. Option triggers compare the order's
/// executable side (ask for buys, bid for sells); underlying triggers compare
/// the spot from the contract's fresh valuation. Levels are inclusive.
enum class TriggerSource { Option, Underlying };
enum class TriggerDirection { AtOrBelow, AtOrAbove };
struct Trigger {
  TriggerSource source = TriggerSource::Option;
  TriggerDirection direction = TriggerDirection::AtOrBelow;
  Money level;
};
/// A bracket exit: a trigger makes it a market order when reached (a stop); a
/// limit price makes it a resting limit (a take-profit). Exactly one is set.
struct ExitSpec {
  std::optional<Trigger> trigger;
  std::optional<Money> limit_price;
};
struct Bracket {
  std::optional<ExitSpec> stop_loss;
  std::optional<ExitSpec> take_profit;
};

/// One leg of a multi-leg order: `ratio` contracts of `symbol` per unit, bought
/// or sold as `side` says.
struct Leg {
  std::string symbol;  ///< Canonical padded OSI of a registered definition.
  Side side = Side::Buy;
  Quantity ratio = 1;
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
  /// negative a credit to receive at least. No trigger or bracket.
  std::vector<Leg> legs;
};
[[nodiscard]] inline bool multi_leg(const OrderRequest& request) { return !request.legs.empty(); }
/// Every contract an order trades: its symbol, or each leg's.
[[nodiscard]] std::vector<std::string> order_symbols(const OrderRequest& request);
enum class OrderRole { Normal, StopLoss, TakeProfit };
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
  Timestamp triggered_at = 0; ///< When an armed order activated.
  [[nodiscard]] Quantity remaining() const { return request.quantity - filled_quantity; }
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
};

/// Observation numbers strictly increase per OSI. Repeated/older observations
/// never refresh liquidity or marks. Sizes are whole contracts, not lots.
struct QuoteObservation {
  std::string symbol;
  std::uint64_t observation = 0;
  Timestamp time = 0;
  std::optional<Money> bid;
  std::optional<Money> ask;
  Quantity bid_size = 0;
  Quantity ask_size = 0;
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
};

/// Evaluation-account rules. The defaults describe an unrestricted paper
/// account: no target, no drawdown floor, any side, no buying-power check.
struct AccountRules {
  std::string plan;           ///< Display name only, e.g. "Intraday 100K"; at most 64 bytes.
  Money profit_target;        ///< Dollars above the starting balance; zero disables.
  Money max_drawdown;         ///< Trailing distance below peak equity; zero disables.
  DrawdownMode drawdown_mode = DrawdownMode::Intraday;
  bool buy_only = false;      ///< Sells may only reduce existing long positions.
  bool buying_power = false;  ///< Enforce cash buying power, with naked-short requirements.
  Timestamp expiry_cutoff = 0;  ///< Auto-close this long before contract expiry; zero disables.
  Phase phase = Phase::Evaluation;
  Money lock_balance;         ///< Once the floor reaches it, the floor stops trailing; zero disables.
  PayoutRules payouts;        ///< Funded phase only.
  [[nodiscard]] bool evaluation() const { return profit_target > Money{} || max_drawdown > Money{}; }
};

struct SessionConfig {
  Money initial_cash = Money::from_micros(100'000'000'000);
  Money fee_per_contract = Money::from_micros(650'000);
  Limits limits;
  ScenarioConfig scenarios;
  AccountRules rules;
};

[[nodiscard]] Decision eligible(const md::OptionContract& contract);
/// v1 policy, not an exchange routing rule; includes the >= $3 upper tier.
[[nodiscard]] Money tick_size(std::string_view root, Money price);
[[nodiscard]] bool valid_quote(const QuoteObservation& quote);
[[nodiscard]] bool valid_valuation(const Valuation& valuation);
void validate_limits(const Limits& limits);
/// Money amounts nonnegative, cutoff within [0, 1 day), plan name at most 64 bytes,
/// payout percentages 0-100 with positive caps; a funded phase has no profit
/// target and needs at least one qualifying day.
void validate_rules(const AccountRules& rules);

}  // namespace openport::trading
