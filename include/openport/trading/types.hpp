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
  KILL_SWITCH, RISK_CHANGED, IOC_REMAINDER, USER_CANCEL, SESSION_CLOSED,
  DAY_END, EXPIRED, AWAITING_SETTLEMENT, INVALID_SETTLEMENT, ALREADY_SETTLED,
  UNKNOWN_ORDER, ORDER_TERMINAL, INVALID_LIMITS, INVALID_TIME, INVALID_SCENARIO,
  INVALID_REASON, JOURNAL_IO, JOURNAL_CORRUPT
};
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
enum class OrderStatus { Working, PartiallyFilled, Filled, Cancelled, Rejected };

struct OrderRequest {
  std::string client_order_id;
  std::string symbol;  ///< Canonical padded OSI of a registered definition.
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Day;
  Quantity quantity = 0;
  std::optional<Money> limit_price;
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
  [[nodiscard]] Quantity remaining() const { return request.quantity - filled_quantity; }
  [[nodiscard]] bool open() const {
    return status == OrderStatus::Working || status == OrderStatus::PartiallyFilled;
  }
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
struct SessionConfig {
  Money initial_cash = Money::from_micros(100'000'000'000);
  Money fee_per_contract = Money::from_micros(650'000);
  Limits limits;
  ScenarioConfig scenarios;
};

[[nodiscard]] Decision eligible(const md::OptionContract& contract);
/// v1 policy, not an exchange routing rule; includes the >= $3 upper tier.
[[nodiscard]] Money tick_size(std::string_view root, Money price);
[[nodiscard]] bool valid_quote(const QuoteObservation& quote);
[[nodiscard]] bool valid_valuation(const Valuation& valuation);
void validate_limits(const Limits& limits);

}  // namespace openport::trading
