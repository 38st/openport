#include "openport/trading/types.hpp"

#include <algorithm>
#include <cmath>

namespace openport::trading {
std::string_view to_string(Reason reason) noexcept {
#define CASE(name) case Reason::name: return #name
  switch (reason) {
    CASE(NONE); CASE(INVALID_MONEY); CASE(ARITHMETIC_OVERFLOW); CASE(INVALID_CONTRACT);
    CASE(AMERICAN_UNSUPPORTED); CASE(NONSTANDARD_UNSUPPORTED); CASE(ROOT_UNSUPPORTED);
    CASE(UNKNOWN_CONTRACT); CASE(INVALID_ORDER); CASE(DUPLICATE_CLIENT_ID);
    CASE(INVALID_TICK); CASE(INVALID_QUOTE); CASE(STALE_QUOTE); CASE(MISSING_VALUATION);
    CASE(MAX_ORDER_CONTRACTS); CASE(PRICE_BAND); CASE(DELTA_LIMIT); CASE(VEGA_LIMIT);
    CASE(DAILY_LOSS); CASE(KILL_SWITCH); CASE(RISK_CHANGED); CASE(IOC_REMAINDER);
    CASE(USER_CANCEL); CASE(SESSION_CLOSED); CASE(FEED_STALLED); CASE(DAY_END); CASE(EXPIRED);
    CASE(AWAITING_SETTLEMENT); CASE(INVALID_SETTLEMENT); CASE(ALREADY_SETTLED);
    CASE(UNKNOWN_ORDER); CASE(ORDER_TERMINAL); CASE(INVALID_LIMITS); CASE(INVALID_TIME);
    CASE(INVALID_SCENARIO); CASE(INVALID_REASON); CASE(JOURNAL_IO); CASE(JOURNAL_CORRUPT); CASE(JOURNAL_LOCKED);
    CASE(EVALUATION_CLOSED); CASE(BUYING_POWER); CASE(BUY_ONLY); CASE(EXPIRY_CUTOFF);
    CASE(ACCOUNT_RESET); CASE(INVALID_RULES); CASE(OCO_FILLED); CASE(POSITION_CLOSED);
    CASE(PAYOUT_UNAVAILABLE); CASE(PAYOUT_NOT_ELIGIBLE); CASE(INVALID_PAYOUT); CASE(PLAN_LOCKED);
    CASE(LIMIT_ONLY); CASE(INVALID_NOTE); CASE(UNKNOWN_TRADE); CASE(DEFINED_RISK); CASE(MARKET_HALTED);
    CASE(SOFT_FLOOR); CASE(TRADE_LIMIT); CASE(COOLDOWN); CASE(PROFIT_LOCK);
    CASE(RUN_ENDED); CASE(INVALID_GROUP); CASE(GTD_END);
    CASE(PROFIT_TARGET); CASE(DRAWDOWN_FLOOR); CASE(DAILY_LOSS_LIMIT); CASE(MIN_TRADING_DAYS);
    CASE(MIN_PROFITABLE_DAYS); CASE(CONSISTENCY); CASE(ACCOUNT_TYPE);
  }
#undef CASE
  return "UNKNOWN";
}
std::vector<std::string> order_symbols(const OrderRequest& request) {
  if (!multi_leg(request)) return {request.symbol};
  std::vector<std::string> symbols;
  for (const auto& leg : request.legs) symbols.push_back(leg.symbol);
  return symbols;
}
Decision eligible(const md::OptionContract& c) {
  const auto conventions = md::conventions_for_root(c.root);
  if (c.style == pricing::ExerciseStyle::American && conventions.style != pricing::ExerciseStyle::American)
    return {Reason::AMERICAN_UNSUPPORTED, "American exercise is not listed on this European-style root", {}, {}, {}};
  if (!c.standard) return {Reason::NONSTANDARD_UNSUPPORTED, "Adjusted deliverables are unsupported", {}, {}, {}};
  // American equity, ETF and OEX options trade too. Equity and ETF options deliver
  // shares when exercised or assigned; OEX settles in cash.
  constexpr std::string_view roots[] = {"SPX", "SPXW", "XSP", "NDX", "NDXP", "RUT", "RUTW", "XND", "MRUT", "DJX", "VIX", "VIXW"};
  if (c.style == pricing::ExerciseStyle::European && std::find(std::begin(roots), std::end(roots), c.root) == std::end(roots))
    return {Reason::ROOT_UNSUPPORTED, "Root is outside the cash-settled European index allowlist", {}, {}, {}};
  if (c.style != conventions.style || c.multiplier != 100 ||
      c.underlying != conventions.underlying ||
      (c.settlement != md::Settlement::AM && c.settlement != md::Settlement::PM) ||
      (c.type != pricing::OptionType::Call && c.type != pricing::OptionType::Put) ||
      !md::valid_date(c.expiry) || c.expiry.year < 2000 || c.expiry.year > 2099 ||
      !std::isfinite(c.strike) || c.strike <= 0 || c.strike > 99999.999 ||
      std::abs(c.strike * 1000 - std::round(c.strike * 1000)) > 1e-6 ||
      c.expiry_time() == md::kInvalidTimestamp)
    return {Reason::INVALID_CONTRACT, "Invalid standard contract terms or noncanonical OSI strike", {}, {}, {}};
  return {};
}
Money tick_size(std::string_view root, Money price) {
  const bool below = price < Money::from_micros(3'000'000);
  if (root == "SPX" || root == "SPXW" || root == "NDX" || root == "NDXP" || root == "RUT" || root == "RUTW" || root == "OEX")
    return Money::from_micros(below ? 50'000 : 100'000);
  if (root == "XSP" || root == "MRUT") return Money::from_micros(below ? 10'000 : 50'000);
  // Equity and ETF classes: SPY, QQQ and IWM quote in pennies at every price;
  // the others follow the penny-pilot tiers.
  if (!md::is_index_underlying(md::conventions_for_root(root).underlying) &&
      root != "SPY" && root != "QQQ" && root != "IWM")
    return Money::from_micros(below ? 10'000 : 50'000);
  return Money::from_micros(10'000);
}
bool valid_trigger(const Trigger& t) {
  if (t.direction != TriggerDirection::AtOrBelow && t.direction != TriggerDirection::AtOrAbove) return false;
  // An underlying symbol: 1-12 uppercase letters, digits, dots or underscores.
  const auto symbol_ok = [](std::string_view symbol) {
    return symbol.size() <= 12 && std::all_of(symbol.begin(), symbol.end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
    });
  };
  switch (t.source) {
    case TriggerSource::Option:
      return t.level > Money{} && t.symbol.empty() && t.study.empty() && t.minute == 0;
    case TriggerSource::Combo:
      return t.symbol.empty() && t.study.empty() && t.minute == 0;
    case TriggerSource::Underlying:
      return t.level > Money{} && symbol_ok(t.symbol) && t.study.empty() && t.minute == 0;
    case TriggerSource::Study:
      return t.level > Money{} && symbol_ok(t.symbol) && t.minute == 0 &&
             std::find(std::begin(kTriggerStudies), std::end(kTriggerStudies), t.study) != std::end(kTriggerStudies);
    case TriggerSource::Time:
      return t.level == Money{} && t.symbol.empty() && t.study.empty() && t.minute >= 0 && t.minute < 24 * 60;
  }
  return false;
}
bool valid_quote(const QuoteObservation& q) {
  return q.observation > 0 && q.bid && q.ask && *q.bid > Money{} && *q.ask >= *q.bid &&
         q.bid_size > 0 && q.ask_size > 0;
}
bool markable_quote(const QuoteObservation& q) {
  return valid_quote(q) ||
         (q.observation > 0 && !q.bid && q.bid_size == 0 && q.ask && *q.ask > Money{} && q.ask_size > 0);
}
bool valid_valuation(const Valuation& v) {
  return v.valid && std::isfinite(v.delta) && std::isfinite(v.gamma) &&
         std::isfinite(v.vega) && std::isfinite(v.theta) && std::isfinite(v.spot) &&
         std::isfinite(v.forward) && std::isfinite(v.discount) && std::isfinite(v.years) &&
         std::isfinite(v.smile_iv) && v.spot > 0 && v.forward > 0 && v.discount > 0 &&
         v.years >= 0 && v.smile_iv > 0;
}
void validate_limits(const Limits& l) {
  auto exposure_ok = [](const ExposureLimits& e) {
    return std::isfinite(e.dollar_delta) && e.dollar_delta >= 0 && std::isfinite(e.vega) && e.vega >= 0;
  };
  bool valid = l.max_order_contracts > 0 && l.price_band_absolute >= Money{} &&
               std::isfinite(l.price_band_relative) && l.price_band_relative >= 0 &&
               l.price_band_relative <= 100 && l.max_daily_loss >= Money{} &&
               l.max_quote_age >= 0 && l.max_valuation_age >= 0 &&
               exposure_ok(l.aggregate) && exposure_ok(l.per_underlying);
  for (const auto& [name, limit] : l.underlying_overrides) valid &= !name.empty() && exposure_ok(limit);
  if (!valid) throw TradingError(Reason::INVALID_LIMITS, "Limits must be finite and nonnegative; order size must be positive");
}
void validate_rules(const AccountRules& r) {
  const auto& p = r.payouts;
  const bool payouts_ok = p.qualifying_profit >= Money{} && p.qualifying_days >= 0 && p.qualifying_days <= 366 &&
      p.withdrawal_percent >= 0 && p.withdrawal_percent <= 100 && p.split_percent >= 0 && p.split_percent <= 100 &&
      p.minimum >= Money{} && p.caps.size() <= 64 &&
      std::all_of(p.caps.begin(), p.caps.end(), [](Money cap) { return cap > Money{}; });
  if (r.profit_target < Money{} || r.max_drawdown < Money{} || r.expiry_cutoff < 0 ||
      r.expiry_cutoff >= md::kNanosPerDay || r.plan.size() > 64 || r.lock_balance < Money{} || !payouts_ok ||
      (r.drawdown_mode != DrawdownMode::Intraday && r.drawdown_mode != DrawdownMode::EndOfDay &&
       r.drawdown_mode != DrawdownMode::Static) ||
      (r.phase != Phase::Evaluation && r.phase != Phase::Funded) ||
      r.slippage_ticks < 0 || r.slippage_ticks > 10 ||
      r.fill_latency_ms < 0 || r.fill_latency_ms > 60'000 || r.impact_ticks < 0 || r.impact_ticks > 10 ||
      (r.margin != MarginMode::Strategy && r.margin != MarginMode::Portfolio) ||
      (r.account_type != AccountType::Margin && r.account_type != AccountType::Cash && r.account_type != AccountType::Ira) ||
      r.house_margin_percent < 0 || r.house_margin_percent > 400 || r.pm_vol_shock < 0 || r.pm_vol_shock > 50 ||
      (r.account_type != AccountType::Margin && (r.margin != MarginMode::Strategy || !r.buying_power)) ||
      (r.phase == Phase::Funded && (r.profit_target > Money{} || p.qualifying_days < 1)))
    throw TradingError(Reason::INVALID_RULES,
        "Rule amounts must be nonnegative, percentages 0-100, caps positive, the expiry cutoff under one day and the plan name "
        "at most 64 bytes; slippage and impact are 0-10 ticks, fill latency is 0-60000 ms and margin is strategy or portfolio; "
        "house margin is 0-400%, the portfolio vol shock 0-50 points, and a cash or IRA account uses strategy margin and "
        "enforces buying power; a funded phase has no profit target and at least one qualifying day");
  if (r.drawdown_mode == DrawdownMode::Static && (r.lock_at_start || r.lock_balance > Money{}))
    throw TradingError(Reason::INVALID_RULES, "A static floor never trails, so it takes no lock");
  if (r.lock_at_start && r.lock_balance > Money{})
    throw TradingError(Reason::INVALID_RULES, "The floor locks at the starting balance or at lock_balance, not both");
  if (r.daily_loss_limit < Money{} || r.profitable_day_profit < Money{} ||
      (r.profit_basis != ProfitBasis::Equity && r.profit_basis != ProfitBasis::Balance) ||
      (r.daily_loss_basis != DailyLossBasis::Equity && r.daily_loss_basis != DailyLossBasis::Balance &&
       r.daily_loss_basis != DailyLossBasis::Higher && r.daily_loss_basis != DailyLossBasis::Peak) ||
      (r.daily_loss_action != BreachAction::Lock && r.daily_loss_action != BreachAction::Fail) ||
      (r.consistency_basis != ConsistencyBasis::Total && r.consistency_basis != ConsistencyBasis::PositiveDays) ||
      r.consistency_percent < 0 || r.consistency_percent > 100 || r.min_trading_days < 0 || r.min_trading_days > 366 ||
      r.min_profitable_days < 0 || r.min_profitable_days > 366)
    throw TradingError(Reason::INVALID_RULES,
        "The daily loss limit and profitable-day profit must be nonnegative, the consistency percentage 0-100 and minimum "
        "days 0-366");
  if (r.day_end_minutes < 16 * 60 + 15 || r.day_end_minutes > 24 * 60)
    throw TradingError(Reason::INVALID_RULES, "The trading day ends between 16:15 and 24:00 New York time");
  if (r.fees) {
    const auto& f = *r.fees;
    const auto amount = [](Money value) { return value >= Money{} && value <= Money::from_micros(1'000'000'000); };
    const bool roots = f.index.size() <= 16 && std::all_of(f.index.begin(), f.index.end(), [&](const auto& item) {
      return !item.first.empty() && item.first.size() <= 6 && amount(item.second) &&
             std::all_of(item.first.begin(), item.first.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    });
    if (!roots || !amount(f.open) || !amount(f.close) || !amount(f.leg_cap) || !amount(f.clearing) ||
        !amount(f.regulatory) || !amount(f.exercise))
      throw TradingError(Reason::INVALID_RULES,
          "Fee schedule amounts must be $0 to $1,000, with at most 16 index roots of 1 to 6 uppercase letters or digits");
  }
}
}  // namespace openport::trading
