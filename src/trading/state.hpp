#pragma once

#include <algorithm>
#include <array>
#include <set>
#include <nlohmann/json.hpp>
#include "openport/trading/history.hpp"
#include "openport/trading/session.hpp"

namespace nlohmann {
template <class T> struct adl_serializer<std::optional<T>> {
  static void to_json(json& j, const std::optional<T>& value) { if (value) j = *value; else j = nullptr; }
  static void from_json(const json& j, std::optional<T>& value) {
    if (j.is_null()) value.reset(); else value = j.get<T>();
  }
};
}  // namespace nlohmann
namespace openport::md {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Date, year, month, day)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OptionContract, root, underlying, expiry, strike, type, style, settlement, multiplier, standard)
}  // namespace openport::md
namespace openport::trading {
using Json = nlohmann::json;
inline void to_json(Json& j, Money m) { j = m.micros(); }
inline void from_json(const Json& j, Money& m) { m = Money::from_micros(j.get<std::int64_t>()); }
inline void to_json(Json& j, Reason r) { j = to_string(r); }
inline void from_json(const Json& j, Reason& r) {
  const auto text = j.get<std::string>();
  for (int i = 0; i <= static_cast<int>(kLastReason); ++i) {
    const auto candidate = static_cast<Reason>(i);
    if (to_string(candidate) == text) { r = candidate; return; }
  }
  throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded reason code");
}
/// Schema 1 records predate account rules. Their original keys stay required;
/// only keys introduced by schema 2 fall back to defaults.
template <class T> void added_field(const Json& j, const char* key, T& value) {
  if (const auto it = j.find(key); it != j.end()) it->get_to(value);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Decision, code, message, actual, limit, scope)
inline void to_json(Json& j, const Trigger& t) {
  j = Json{{"source", t.source}, {"direction", t.direction}, {"level", t.level}};
  // Only conditional triggers record their terms; every other trigger keeps its bytes.
  if (!t.symbol.empty()) j["symbol"] = t.symbol;
  if (!t.study.empty()) j["study"] = t.study;
  if (t.source == TriggerSource::Time) j["minute"] = t.minute;
}
inline void from_json(const Json& j, Trigger& t) {
  j.at("source").get_to(t.source); j.at("direction").get_to(t.direction); j.at("level").get_to(t.level);
  added_field(j, "symbol", t.symbol); added_field(j, "study", t.study); added_field(j, "minute", t.minute);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExitSpec, trigger, limit_price)
inline void to_json(Json& j, AlertScope scope) {
  constexpr const char* names[] = {"contract", "spread", "underlying", "account"};
  j = names[static_cast<int>(scope)];
}
inline void from_json(const Json& j, AlertScope& scope) {
  const auto name = j.get<std::string>();
  constexpr const char* names[] = {"contract", "spread", "underlying", "account"};
  for (int i = 0; i < 4; ++i) if (name == names[i]) { scope = static_cast<AlertScope>(i); return; }
  throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded alert scope");
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Bracket, stop_loss, take_profit)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Leg, symbol, side, ratio)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlertCondition, scope, metric, symbol, legs, direction, level)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlertSpec, label, condition, repeat)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Alert, id, spec, created, actor, armed, fired, fired_at, value)
inline void to_json(Json& j, const OrderRequest& r) {
  j = Json{{"client_order_id", r.client_order_id}, {"symbol", r.symbol}, {"side", r.side}, {"type", r.type}, {"tif", r.tif},
           {"quantity", r.quantity}, {"limit_price", r.limit_price}, {"trigger", r.trigger}, {"bracket", r.bracket},
           {"legs", r.legs}, {"tags", r.tags}, {"note", r.note}, {"exits_only", r.exits_only}};
  // Only an order naming a trade records it, so every other order keeps its bytes.
  if (!r.group.empty()) j["group"] = r.group;
  if (r.good_till) j["good_till"] = *r.good_till;
}
inline void from_json(const Json& j, OrderRequest& r) {
  j.at("client_order_id").get_to(r.client_order_id); j.at("symbol").get_to(r.symbol); j.at("side").get_to(r.side);
  j.at("type").get_to(r.type); j.at("tif").get_to(r.tif); j.at("quantity").get_to(r.quantity);
  j.at("limit_price").get_to(r.limit_price);
  added_field(j, "trigger", r.trigger); added_field(j, "bracket", r.bracket); added_field(j, "legs", r.legs);
  added_field(j, "tags", r.tags); added_field(j, "note", r.note); added_field(j, "exits_only", r.exits_only);
  added_field(j, "group", r.group);
  added_field(j, "good_till", r.good_till);
}
inline void to_json(Json& j, const OrderChangeRecord& c) {
  j = Json{{"time", c.time}, {"actor", c.actor}, {"previous_quantity", c.previous_quantity}};
  if (c.quantity) j["quantity"] = *c.quantity;
  if (c.limit_price) j["limit_price"] = *c.limit_price;
  if (c.trigger_level) j["trigger_level"] = *c.trigger_level;
  if (c.previous_limit_price) j["previous_limit_price"] = *c.previous_limit_price;
  if (c.previous_trigger_level) j["previous_trigger_level"] = *c.previous_trigger_level;
  if (!c.decision.ok()) j["decision"] = c.decision;
  if (c.time_in_force) j["time_in_force"] = *c.time_in_force;
  if (c.previous_time_in_force) j["previous_time_in_force"] = *c.previous_time_in_force;
}
inline void from_json(const Json& j, OrderChangeRecord& c) {
  j.at("time").get_to(c.time); j.at("actor").get_to(c.actor); j.at("previous_quantity").get_to(c.previous_quantity);
  added_field(j, "quantity", c.quantity); added_field(j, "limit_price", c.limit_price);
  added_field(j, "trigger_level", c.trigger_level); added_field(j, "previous_limit_price", c.previous_limit_price);
  added_field(j, "previous_trigger_level", c.previous_trigger_level); added_field(j, "decision", c.decision);
  added_field(j, "time_in_force", c.time_in_force); added_field(j, "previous_time_in_force", c.previous_time_in_force);
}
inline void to_json(Json& j, const Order& o) {
  j = Json{{"id", o.id}, {"request", o.request}, {"status", o.status}, {"filled_quantity", o.filled_quantity},
           {"filled_notional", o.filled_notional}, {"accepted_at", o.accepted_at}, {"day_end", o.day_end},
           {"reason", o.reason}, {"system", o.system}, {"role", o.role}, {"parent", o.parent}, {"oco", o.oco},
           {"stop_loss", o.stop_loss}, {"take_profit", o.take_profit}, {"triggered_at", o.triggered_at}, {"actor", o.actor}};
  // Only a changed order records its submitted terms; every other order keeps its bytes.
  if (o.submitted) j["submitted"] = *o.submitted;
  // Likewise an order's end and its changes, once it has them.
  if (o.ended_at != 0) j["ended_at"] = o.ended_at;
  if (!o.changes.empty()) j["changes"] = o.changes;
  if (o.reduce_only) j["reduce_only"] = true;
  if (o.limit_ticks) j["limit_ticks"] = *o.limit_ticks;
}
inline void from_json(const Json& j, Order& o) {
  j.at("id").get_to(o.id); j.at("request").get_to(o.request); j.at("status").get_to(o.status);
  j.at("filled_quantity").get_to(o.filled_quantity); j.at("filled_notional").get_to(o.filled_notional);
  j.at("accepted_at").get_to(o.accepted_at); j.at("day_end").get_to(o.day_end); j.at("reason").get_to(o.reason);
  added_field(j, "system", o.system); added_field(j, "role", o.role); added_field(j, "parent", o.parent);
  added_field(j, "oco", o.oco); added_field(j, "stop_loss", o.stop_loss); added_field(j, "take_profit", o.take_profit);
  added_field(j, "triggered_at", o.triggered_at); added_field(j, "actor", o.actor);
  added_field(j, "submitted", o.submitted); added_field(j, "reduce_only", o.reduce_only);
  added_field(j, "ended_at", o.ended_at); added_field(j, "changes", o.changes);
  added_field(j, "limit_ticks", o.limit_ticks);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FillContext, spot, spot_source, iv, delta, years, equity, floor_room, buying_power)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Excursion, pnl, time, spot)
inline void to_json(Json& j, const TradeReview& r) {
  j = Json{{"worst", r.worst}, {"best", r.best}, {"planned_risk", r.planned_risk}, {"finished", r.finished}};
  if (r.since) j["since"] = *r.since;
}
inline void from_json(const Json& j, TradeReview& r) {
  j.at("worst").get_to(r.worst); j.at("best").get_to(r.best); j.at("planned_risk").get_to(r.planned_risk);
  j.at("finished").get_to(r.finished); added_field(j, "since", r.since);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DayNote, plan, review, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FillQuote, bid, ask, bid_size, ask_size, left, quoted)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FillFees, commission, clearing, regulatory, index)
inline void to_json(Json& j, const Fill& f) {
  j = Json{{"id", f.id}, {"order_id", f.order_id}, {"symbol", f.symbol}, {"side", f.side}, {"quantity", f.quantity},
           {"price", f.price}, {"fee", f.fee}, {"observation", f.observation}, {"quote_time", f.quote_time},
           {"time", f.time}, {"context", f.context}, {"actor", f.actor}};
  if (f.quote) j["quote"] = *f.quote;
  if (f.fees) j["fees"] = *f.fees;
}
inline void from_json(const Json& j, Fill& f) {
  j.at("id").get_to(f.id); j.at("order_id").get_to(f.order_id); j.at("symbol").get_to(f.symbol);
  j.at("side").get_to(f.side); j.at("quantity").get_to(f.quantity); j.at("price").get_to(f.price);
  j.at("fee").get_to(f.fee); j.at("observation").get_to(f.observation);
  j.at("quote_time").get_to(f.quote_time); j.at("time").get_to(f.time);
  added_field(j, "context", f.context); added_field(j, "actor", f.actor); added_field(j, "quote", f.quote);
  added_field(j, "fees", f.fees);
}
inline void to_json(Json& j, const QuoteObservation& q) {
  j = Json{{"symbol", q.symbol}, {"observation", q.observation}, {"time", q.time}, {"bid", q.bid}, {"ask", q.ask},
           {"bid_size", q.bid_size}, {"ask_size", q.ask_size}};
  // Only a quote first given before its time records that; others keep their bytes.
  if (q.quoted != 0) j["quoted"] = q.quoted;
}
inline void from_json(const Json& j, QuoteObservation& q) {
  j.at("symbol").get_to(q.symbol); j.at("observation").get_to(q.observation); j.at("time").get_to(q.time);
  j.at("bid").get_to(q.bid); j.at("ask").get_to(q.ask); j.at("bid_size").get_to(q.bid_size);
  j.at("ask_size").get_to(q.ask_size); added_field(j, "quoted", q.quoted);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Valuation, symbol, time, delta, gamma, vega, theta, spot, forward, discount, years, smile_iv, valid, spot_source)
inline void from_json(const Json& j, Valuation& v) {
  j.at("symbol").get_to(v.symbol); j.at("time").get_to(v.time); j.at("delta").get_to(v.delta);
  j.at("gamma").get_to(v.gamma); j.at("vega").get_to(v.vega); j.at("theta").get_to(v.theta);
  j.at("spot").get_to(v.spot); j.at("forward").get_to(v.forward); j.at("discount").get_to(v.discount);
  j.at("years").get_to(v.years); j.at("smile_iv").get_to(v.smile_iv); j.at("valid").get_to(v.valid);
  added_field(j, "spot_source", v.spot_source);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Exposure, dollar_delta, dollar_gamma_1pct, vega, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureLimits, dollar_delta, vega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Limits, max_order_contracts, price_band_absolute, price_band_relative, aggregate, per_underlying, underlying_overrides, max_daily_loss, max_quote_age, max_valuation_age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Guardrails, soft_floor, soft_floor_percent, max_opening_trades, cooldown_loss, cooldown_minutes, profit_lock)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(GuardrailState, opening_trades, cooldown_until, latched, owns_kill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(KillChange, time, action, reason, previous, actor)
inline void from_json(const Json& j, GuardrailState& g) {
  added_field(j, "opening_trades", g.opening_trades); added_field(j, "cooldown_until", g.cooldown_until);
  added_field(j, "latched", g.latched); added_field(j, "owns_kill", g.owns_kill);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioConfig, spot_percent, vol_points, vol_floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PayoutRules, qualifying_profit, qualifying_days, withdrawal_percent, split_percent, minimum, caps)
inline void to_json(Json& j, MarginMode mode) { j = mode == MarginMode::Portfolio ? "portfolio" : "strategy"; }
inline void from_json(const Json& j, MarginMode& mode) {
  const auto value = j.get<std::string>();
  if (value != "strategy" && value != "portfolio") throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded margin mode");
  mode = value == "portfolio" ? MarginMode::Portfolio : MarginMode::Strategy;
}
inline void to_json(Json& j, AccountType type) {
  j = type == AccountType::Cash ? "cash" : type == AccountType::Ira ? "ira" : "margin";
}
inline void from_json(const Json& j, AccountType& type) {
  const auto value = j.get<std::string>();
  if (value != "margin" && value != "cash" && value != "ira") throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded account type");
  type = value == "cash" ? AccountType::Cash : value == "ira" ? AccountType::Ira : AccountType::Margin;
}
/// A rule's named choice, recorded as text; an unknown name is a corrupt journal.
template <class E, std::size_t N>
void named_to_json(Json& j, E value, const std::array<const char*, N>& names) {
  j = names.at(static_cast<std::size_t>(value));
}
template <class E, std::size_t N>
void named_from_json(const Json& j, E& value, const std::array<const char*, N>& names) {
  const auto text = j.get<std::string>();
  for (std::size_t i = 0; i < N; ++i)
    if (text == names[i]) { value = static_cast<E>(i); return; }
  throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded rule choice: " + text);
}
inline constexpr std::array<const char*, 2> kProfitBases{"equity", "balance"};
inline constexpr std::array<const char*, 4> kDailyLossBases{"equity", "balance", "higher", "peak"};
inline constexpr std::array<const char*, 2> kBreachActions{"lock", "fail"};
inline constexpr std::array<const char*, 2> kConsistencyBases{"total", "positive_days"};
inline void to_json(Json& j, ProfitBasis v) { named_to_json(j, v, kProfitBases); }
inline void from_json(const Json& j, ProfitBasis& v) { named_from_json(j, v, kProfitBases); }
inline void to_json(Json& j, DailyLossBasis v) { named_to_json(j, v, kDailyLossBases); }
inline void from_json(const Json& j, DailyLossBasis& v) { named_from_json(j, v, kDailyLossBases); }
inline void to_json(Json& j, BreachAction v) { named_to_json(j, v, kBreachActions); }
inline void from_json(const Json& j, BreachAction& v) { named_from_json(j, v, kBreachActions); }
inline void to_json(Json& j, ConsistencyBasis v) { named_to_json(j, v, kConsistencyBases); }
inline void from_json(const Json& j, ConsistencyBasis& v) { named_from_json(j, v, kConsistencyBases); }
/// The rules recorded only when they differ from their defaults, with those
/// defaults: a plan without them keeps its journal bytes.
inline Json optional_rule_defaults() {
  const AccountRules d;
  return Json{{"fill_latency_ms", d.fill_latency_ms}, {"impact_ticks", d.impact_ticks}, {"lock_at_start", d.lock_at_start},
              {"profit_basis", d.profit_basis}, {"daily_loss_limit", d.daily_loss_limit},
              {"daily_loss_basis", d.daily_loss_basis}, {"daily_loss_action", d.daily_loss_action},
              {"consistency_percent", d.consistency_percent}, {"consistency_basis", d.consistency_basis},
              {"min_trading_days", d.min_trading_days}, {"min_profitable_days", d.min_profitable_days},
              {"profitable_day_profit", d.profitable_day_profit}, {"day_end_minutes", d.day_end_minutes},
              {"account_type", d.account_type}, {"house_margin_percent", d.house_margin_percent},
              {"pm_vol_shock", d.pm_vol_shock}};
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FeeSchedule, open, close, leg_cap, clearing, regulatory, index, exercise)
inline void to_json(Json& j, const AccountRules& r) {
  j = Json{{"plan", r.plan}, {"profit_target", r.profit_target}, {"max_drawdown", r.max_drawdown},
           {"drawdown_mode", r.drawdown_mode}, {"buy_only", r.buy_only}, {"buying_power", r.buying_power},
           {"expiry_cutoff", r.expiry_cutoff}, {"phase", r.phase}, {"lock_balance", r.lock_balance},
           {"payouts", r.payouts}, {"defined_risk", r.defined_risk}, {"slippage_ticks", r.slippage_ticks}, {"margin", r.margin}};
  // Preserve existing journal bytes when the optional models and rules are off.
  const Json all{{"fill_latency_ms", r.fill_latency_ms}, {"impact_ticks", r.impact_ticks}, {"lock_at_start", r.lock_at_start},
                 {"profit_basis", r.profit_basis}, {"daily_loss_limit", r.daily_loss_limit},
                 {"daily_loss_basis", r.daily_loss_basis}, {"daily_loss_action", r.daily_loss_action},
                 {"consistency_percent", r.consistency_percent}, {"consistency_basis", r.consistency_basis},
                 {"min_trading_days", r.min_trading_days}, {"min_profitable_days", r.min_profitable_days},
                 {"profitable_day_profit", r.profitable_day_profit}, {"day_end_minutes", r.day_end_minutes},
                 {"account_type", r.account_type}, {"house_margin_percent", r.house_margin_percent},
                 {"pm_vol_shock", r.pm_vol_shock}};
  static const auto defaults = optional_rule_defaults();
  for (auto it = all.begin(); it != all.end(); ++it)
    if (it.value() != defaults.at(it.key())) j[it.key()] = it.value();
  if (r.fees) j["fees"] = *r.fees;
}
inline void from_json(const Json& j, AccountRules& r) {
  j.at("plan").get_to(r.plan); j.at("profit_target").get_to(r.profit_target); j.at("max_drawdown").get_to(r.max_drawdown);
  j.at("drawdown_mode").get_to(r.drawdown_mode); j.at("buy_only").get_to(r.buy_only); j.at("buying_power").get_to(r.buying_power);
  j.at("expiry_cutoff").get_to(r.expiry_cutoff);
  added_field(j, "phase", r.phase); added_field(j, "lock_balance", r.lock_balance); added_field(j, "payouts", r.payouts);
  added_field(j, "defined_risk", r.defined_risk);
  if (const auto it = j.find("slippage_ticks"); it != j.end() && !it->is_number_integer())
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded slippage must be an integer");
  added_field(j, "slippage_ticks", r.slippage_ticks); added_field(j, "margin", r.margin);
  for (const auto* key : {"fill_latency_ms", "impact_ticks"})
    if (const auto it = j.find(key); it != j.end() && !it->is_number_integer())
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded fill settings must be integers");
  r.fill_latency_ms = j.value("fill_latency_ms", std::int64_t{0});
  r.impact_ticks = j.value("impact_ticks", std::int64_t{0});
  for (const auto* key : {"consistency_percent", "min_trading_days", "min_profitable_days", "day_end_minutes",
                          "house_margin_percent", "pm_vol_shock"})
    if (const auto it = j.find(key); it != j.end() && !it->is_number_integer())
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded rule counts must be integers");
  added_field(j, "lock_at_start", r.lock_at_start); added_field(j, "profit_basis", r.profit_basis);
  added_field(j, "daily_loss_limit", r.daily_loss_limit); added_field(j, "daily_loss_basis", r.daily_loss_basis);
  added_field(j, "daily_loss_action", r.daily_loss_action);
  added_field(j, "consistency_percent", r.consistency_percent); added_field(j, "consistency_basis", r.consistency_basis);
  added_field(j, "min_trading_days", r.min_trading_days); added_field(j, "min_profitable_days", r.min_profitable_days);
  added_field(j, "profitable_day_profit", r.profitable_day_profit); added_field(j, "day_end_minutes", r.day_end_minutes);
  added_field(j, "fees", r.fees);
  added_field(j, "account_type", r.account_type); added_field(j, "house_margin_percent", r.house_margin_percent);
  added_field(j, "pm_vol_shock", r.pm_vol_shock);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(SessionConfig, initial_cash, fee_per_contract, limits, scenarios, rules, guardrails)
inline void from_json(const Json& j, SessionConfig& c) {
  j.at("initial_cash").get_to(c.initial_cash); j.at("fee_per_contract").get_to(c.fee_per_contract);
  j.at("limits").get_to(c.limits); j.at("scenarios").get_to(c.scenarios);
  added_field(j, "rules", c.rules); added_field(j, "guardrails", c.guardrails);
}
inline void to_json(Json& j, const Attribution& a) {
  j = Json{{"delta", a.delta}, {"gamma", a.gamma}, {"vega", a.vega}, {"theta", a.theta}, {"other", a.other}, {"costs", a.costs}};
  // Only a fallback is recorded, so parts the Greeks split keep their bytes.
  if (a.fallback) j["fallback"] = true;
}
inline void from_json(const Json& j, Attribution& a) {
  j.at("delta").get_to(a.delta); j.at("gamma").get_to(a.gamma); j.at("vega").get_to(a.vega);
  j.at("theta").get_to(a.theta); j.at("other").get_to(a.other); j.at("costs").get_to(a.costs);
  a.fallback = j.value("fallback", false);
}
inline void to_json(Json& j, const EvaluationDay& d) {
  j = Json{{"day", d.day}, {"open_equity", d.open_equity}, {"close_equity", d.close_equity}, {"peak", d.peak},
           {"floor", d.floor}, {"realised", d.realised}, {"qualifying", d.qualifying}, {"attribution", d.attribution},
           {"low_equity", d.low_equity}, {"high_equity", d.high_equity}, {"low_at", d.low_at}, {"high_at", d.high_at}};
  // Kept only when a rule reads them, so other days keep their bytes.
  if (d.executions != 0) j["executions"] = d.executions;
  if (d.locked != Reason::NONE) j["locked"] = d.locked;
}
inline void from_json(const Json& j, EvaluationDay& d) {
  j.at("day").get_to(d.day); j.at("open_equity").get_to(d.open_equity); j.at("close_equity").get_to(d.close_equity);
  j.at("peak").get_to(d.peak); j.at("floor").get_to(d.floor);
  added_field(j, "realised", d.realised); added_field(j, "qualifying", d.qualifying);
  added_field(j, "attribution", d.attribution);
  added_field(j, "low_equity", d.low_equity); added_field(j, "high_equity", d.high_equity);
  added_field(j, "low_at", d.low_at); added_field(j, "high_at", d.high_at);
  added_field(j, "executions", d.executions); added_field(j, "locked", d.locked);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Payout, number, time, day, amount, trader_share, balance)
/// A decision's code, unless its status implies it (a pass on the target, a
/// failure on the floor), as every decision recorded before codes does.
inline bool implied_code(EvaluationStatus status, Reason code) {
  return code == Reason::NONE || (status == EvaluationStatus::Passed && code == Reason::PROFIT_TARGET) ||
         (status == EvaluationStatus::Failed && code == Reason::DRAWDOWN_FLOOR);
}
inline Reason decision_code_of(EvaluationStatus status, Reason recorded) {
  if (recorded != Reason::NONE || status == EvaluationStatus::Active) return recorded;
  return status == EvaluationStatus::Passed ? Reason::PROFIT_TARGET : Reason::DRAWDOWN_FLOOR;
}
inline void to_json(Json& j, const Evaluation& e) {
  j = Json{{"attempt", e.attempt}, {"started", e.started}, {"starting_balance", e.starting_balance}, {"peak", e.peak},
           {"floor", e.floor}, {"status", e.status}, {"decided_at", e.decided_at}, {"decided_equity", e.decided_equity},
           {"decision", e.decision}, {"first_order", e.first_order}, {"first_fill", e.first_fill}, {"day", e.day},
           {"day_open_equity", e.day_open_equity}, {"day_close_equity", e.day_close_equity}, {"days", e.days},
           {"floor_locked", e.floor_locked}, {"day_open_realised", e.day_open_realised}, {"qualifying_days", e.qualifying_days},
           {"cycle_started", e.cycle_started}, {"payouts", e.payouts}, {"day_low_equity", e.day_low_equity},
           {"day_high_equity", e.day_high_equity}, {"day_low_at", e.day_low_at}, {"day_high_at", e.day_high_at},
           {"closest_floor", e.closest_floor}, {"closest_floor_at", e.closest_floor_at}};
  // The plan rules' own state appears only once they act, so other journals keep their bytes.
  if (!implied_code(e.status, e.decision_code)) j["decision_code"] = e.decision_code;
  if (e.day_lock != Reason::NONE) { j["day_lock"] = e.day_lock; j["day_locked_at"] = e.day_locked_at; }
  if (e.day_executions != 0) j["day_executions"] = e.day_executions;
}
inline void from_json(const Json& j, Evaluation& e) {
  j.at("attempt").get_to(e.attempt); j.at("started").get_to(e.started); j.at("starting_balance").get_to(e.starting_balance);
  j.at("peak").get_to(e.peak); j.at("floor").get_to(e.floor); j.at("status").get_to(e.status);
  j.at("decided_at").get_to(e.decided_at); j.at("decided_equity").get_to(e.decided_equity); j.at("decision").get_to(e.decision);
  j.at("first_order").get_to(e.first_order); j.at("first_fill").get_to(e.first_fill); j.at("day").get_to(e.day);
  j.at("day_open_equity").get_to(e.day_open_equity); j.at("day_close_equity").get_to(e.day_close_equity); j.at("days").get_to(e.days);
  added_field(j, "floor_locked", e.floor_locked); added_field(j, "day_open_realised", e.day_open_realised);
  added_field(j, "qualifying_days", e.qualifying_days); added_field(j, "cycle_started", e.cycle_started);
  added_field(j, "payouts", e.payouts);
  added_field(j, "day_low_equity", e.day_low_equity); added_field(j, "day_high_equity", e.day_high_equity);
  added_field(j, "day_low_at", e.day_low_at); added_field(j, "day_high_at", e.day_high_at);
  added_field(j, "closest_floor", e.closest_floor); added_field(j, "closest_floor_at", e.closest_floor_at);
  added_field(j, "decision_code", e.decision_code);
  e.decision_code = decision_code_of(e.status, e.decision_code);
  added_field(j, "day_lock", e.day_lock); added_field(j, "day_locked_at", e.day_locked_at);
  added_field(j, "day_executions", e.day_executions);
}
inline void to_json(Json& j, const AttemptSummary& a) {
  j = Json{{"attempt", a.attempt}, {"plan", a.plan}, {"started", a.started}, {"ended", a.ended},
           {"starting_balance", a.starting_balance}, {"final_equity", a.final_equity}, {"status", a.status},
           {"decision", a.decision}, {"first_order", a.first_order}, {"first_fill", a.first_fill}};
  if (!implied_code(a.status, a.decision_code)) j["decision_code"] = a.decision_code;
}
inline void from_json(const Json& j, AttemptSummary& a) {
  j.at("attempt").get_to(a.attempt); j.at("plan").get_to(a.plan); j.at("started").get_to(a.started);
  j.at("ended").get_to(a.ended); j.at("starting_balance").get_to(a.starting_balance);
  j.at("final_equity").get_to(a.final_equity); j.at("status").get_to(a.status); j.at("decision").get_to(a.decision);
  j.at("first_order").get_to(a.first_order); j.at("first_fill").get_to(a.first_fill);
  added_field(j, "decision_code", a.decision_code);
  a.decision_code = decision_code_of(a.status, a.decision_code);
}
inline void to_json(Json& j, const Closure& c) {
  j = Json{{"symbol", c.symbol}, {"quantity", c.quantity}, {"price", c.price}, {"time", c.time}, {"kind", c.kind},
           {"after_fill", c.after_fill}};
  if (c.fee != Money{}) j["fee"] = c.fee;
}
inline void from_json(const Json& j, Closure& c) {
  j.at("symbol").get_to(c.symbol); j.at("quantity").get_to(c.quantity); j.at("price").get_to(c.price);
  j.at("time").get_to(c.time); j.at("kind").get_to(c.kind); j.at("after_fill").get_to(c.after_fill);
  added_field(j, "fee", c.fee);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StockFill, id, symbol, shares, price, time, source, option)
inline void to_json(Json& j, const DividendPayment& d) {
  j = Json{{"symbol", d.symbol}, {"ex_date", d.ex_date}, {"per_share", d.per_share}, {"shares", d.shares},
           {"amount", d.amount}, {"time", d.time}};
  if (d.after_stock_fill) j["after_stock_fill"] = *d.after_stock_fill;
}
inline void from_json(const Json& j, DividendPayment& d) {
  j.at("symbol").get_to(d.symbol); j.at("ex_date").get_to(d.ex_date); j.at("per_share").get_to(d.per_share);
  j.at("shares").get_to(d.shares); j.at("amount").get_to(d.amount); j.at("time").get_to(d.time);
  added_field(j, "after_stock_fill", d.after_stock_fill);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ClosingPrint, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Annotation, note, tags, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BuyingPower, available, reserved, short_requirement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Position, contract, quantity, basis, realised, fees)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Account, cash, realised, fees)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StockPosition, symbol, shares, basis, realised, fees)
inline void to_json(Json& j, const Ledger& l) {
  j = Json{{"account", l.account()}, {"positions", l.positions()}, {"stocks", l.stocks()}};
}
inline void from_json(const Json& j, Ledger& l) {
  std::map<std::string, StockPosition> stocks;
  added_field(j, "stocks", stocks);
  l = Ledger::restore(j.at("account").get<Account>(), j.at("positions").get<std::map<std::string, Position>>(), std::move(stocks));
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureRange, delta_low, delta_high, vega_low, vega_high)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskBucket, position, reachable, limits, delta_utilisation, vega_utilisation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskSnapshot, aggregate, underlyings, complete, daily_loss, kill_latched, kill_reason, limits_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioCell, spot_percent, vol_points, pnl, clamped)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioGrid, cells, complete)
inline void to_json(Json& j, const MarkedPosition& p) {
  j = Json{{"position", p.position}, {"mark", p.mark}, {"mark_time", p.mark_time}, {"mark_age", p.mark_age},
           {"market_value", p.market_value}, {"unrealised", p.unrealised}, {"fresh", p.fresh},
           {"awaiting_settlement", p.awaiting_settlement}};
  // Older snapshots have neither; write them only when set.
  if (p.no_bid) j["no_bid"] = true;
  if (p.do_not_exercise) j["do_not_exercise"] = true;
}
inline void from_json(const Json& j, MarkedPosition& p) {
  j.at("position").get_to(p.position); j.at("mark").get_to(p.mark); j.at("mark_time").get_to(p.mark_time);
  j.at("mark_age").get_to(p.mark_age); j.at("market_value").get_to(p.market_value); j.at("unrealised").get_to(p.unrealised);
  j.at("fresh").get_to(p.fresh); j.at("awaiting_settlement").get_to(p.awaiting_settlement);
  added_field(j, "no_bid", p.no_bid); added_field(j, "do_not_exercise", p.do_not_exercise);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MarkedStock, position, mark, mark_time, market_value, unrealised, fresh)
inline void to_json(Json& j, const TradingSnapshot& s) {
  j = Json{
      {"account_version", s.account_version}, {"time", s.time}, {"account", s.account},
      {"equity", s.equity}, {"start_of_day_equity", s.start_of_day_equity}, {"unrealised", s.unrealised},
      {"valuation_complete", s.valuation_complete}, {"journal_failed", s.journal_failed}, {"positions", s.positions},
      {"stocks", s.stocks}, {"open_orders", s.open_orders}, {"recent_orders", s.recent_orders},
      {"recent_fills", s.recent_fills}, {"risk", s.risk}, {"scenarios", s.scenarios},
      {"quality_flags", s.quality_flags}, {"evaluation", s.evaluation}, {"buying_power", s.buying_power},
      {"closures", s.closures}, {"attempts", s.attempts}, {"annotations", s.annotations},
      {"attribution", s.attribution}, {"attributions", s.attributions}, {"stock_fills", s.stock_fills},
      {"dividends", s.dividends}, {"closing_prints", s.closing_prints}, {"day_notes", s.day_notes},
      {"trade_reviews", s.trade_reviews}, {"strategy_reviews", s.strategy_reviews}, {"pending_limits", s.pending_limits},
      {"pending_guardrails", s.pending_guardrails}, {"guardrails", s.guardrails}, {"pending_applied_at", s.pending_applied_at},
      {"soft_floor", s.soft_floor}, {"trip_attributions", s.trip_attributions}, {"groups", s.groups},
      {"group_reviews", s.group_reviews}};
  if (!s.alerts.empty()) j["alerts"] = s.alerts;
}
inline void from_json(const Json& j, TradingSnapshot& s) {
  j.at("account_version").get_to(s.account_version); j.at("time").get_to(s.time); j.at("account").get_to(s.account);
  j.at("equity").get_to(s.equity); j.at("start_of_day_equity").get_to(s.start_of_day_equity);
  j.at("unrealised").get_to(s.unrealised); j.at("valuation_complete").get_to(s.valuation_complete);
  j.at("journal_failed").get_to(s.journal_failed); j.at("positions").get_to(s.positions);
  j.at("open_orders").get_to(s.open_orders); j.at("recent_orders").get_to(s.recent_orders);
  j.at("recent_fills").get_to(s.recent_fills); j.at("risk").get_to(s.risk); j.at("scenarios").get_to(s.scenarios);
  j.at("quality_flags").get_to(s.quality_flags);
  added_field(j, "evaluation", s.evaluation); added_field(j, "buying_power", s.buying_power);
  added_field(j, "closures", s.closures); added_field(j, "attempts", s.attempts);
  added_field(j, "annotations", s.annotations);
  added_field(j, "attribution", s.attribution); added_field(j, "attributions", s.attributions);
  added_field(j, "stocks", s.stocks); added_field(j, "stock_fills", s.stock_fills);
  added_field(j, "dividends", s.dividends); added_field(j, "closing_prints", s.closing_prints);
  added_field(j, "day_notes", s.day_notes); added_field(j, "trade_reviews", s.trade_reviews);
  added_field(j, "strategy_reviews", s.strategy_reviews);
  added_field(j, "pending_limits", s.pending_limits); added_field(j, "pending_guardrails", s.pending_guardrails);
  added_field(j, "guardrails", s.guardrails); added_field(j, "pending_applied_at", s.pending_applied_at);
  added_field(j, "soft_floor", s.soft_floor); added_field(j, "trip_attributions", s.trip_attributions);
  added_field(j, "groups", s.groups); added_field(j, "group_reviews", s.group_reviews);
  added_field(j, "alerts", s.alerts);
}

namespace detail {
struct Book {
  QuoteObservation quote;
  // With impact enabled, negative budgets count contracts beyond displayed size.
  Quantity bid_left = 0;
  Quantity ask_left = 0;
};
struct Mark {
  Money price;
  Timestamp time = 0;
};
/// The account's alerts, and how many it has created: IDs are never reused.
struct AlertBook {
  std::uint64_t created = 0;
  std::vector<Alert> items;
};
/// Where a held position's current stretch at one size began: its size, mark
/// and, when valid, valuation. Today's P&L by Greek runs from here.
struct Reference {
  Quantity quantity = 0;
  Money mark;
  std::optional<Valuation> valuation;
};
/// Derived, never journaled: the lifecycles whose reviews can still change, kept
/// up to date as fills and closures are recorded (`fills` and `closures` of them
/// applied so far), and how many reviews are unfinished. update_reviews then costs
/// the open positions rather than every fill. Built on first use after a load.
struct Reviewing {
  bool ready = false;
  std::size_t fills = 0;
  std::size_t closures = 0;
  LifecycleBuilder builder;
  std::size_t unfinished = 0;
};
struct State {
  std::string actor = "system"; ///< Transient command context, not persisted as account state.
  SessionConfig config;
  Timestamp time = 0;
  std::uint64_t version = 0;
  std::uint64_t limits_revision = 1;
  Ledger ledger;
  Money start_equity;
  md::Date day;
  Contracts contracts;
  SharedMap<std::string, Book> books;
  SharedMap<std::string, Mark> marks;
  Valuations valuations;
  SharedVector<Order> orders;
  SharedVector<Fill> fills;
  /// Transient commission already charged in a trial fill, before it has a Fill.
  std::map<std::pair<OrderId, std::string>, Money> projected_commission;
  SharedSet<std::string> settled;
  bool kill = false;
  std::string kill_reason;
  /// Every trip, reset and release of the kill latch, oldest first.
  std::vector<KillChange> kill_history;
  Evaluation evaluation;
  std::optional<Limits> pending_limits;
  std::optional<Guardrails> pending_guardrails;
  GuardrailState guardrails;
  Timestamp pending_applied_at = 0;
  std::vector<AttemptSummary> attempts;
  SharedVector<Closure> closures;
  SharedMap<std::string, Annotation> annotations;
  SharedMap<std::string, DayNote> day_notes;
  SharedMap<std::string, TradeReview> trade_reviews;
  SharedMap<std::string, TradeReview> strategy_reviews;
  /// The trade each round trip is in, by trade ID, where it is not its root's
  /// (see trade_group); and whole-trade reviews of trades with more than one entry.
  SharedMap<std::string, std::string> groups;
  SharedMap<std::string, TradeReview> group_reviews;
  /// The fills whose round trips have joined their trades: those recorded when an
  /// execution last opened one. A journal from before trades were grouped joins
  /// none of its earlier fills.
  std::uint64_t grouped = 0;
  /// Open stretches by held contract (or stock), and today's finished ones (and costs).
  std::map<std::string, Reference> references;
  std::map<std::string, Attribution> explained;
  /// The round trip each held contract or stock is in, by trade ID (an option's
  /// opening fill; "s" and the opening stock fill for shares), as lifecycles() and
  /// share_lifecycles() number them; and each round trip's P&L by Greek from its
  /// finished stretches and costs, kept across days.
  std::map<std::string, std::string> trips;
  SharedMap<std::string, Attribution> trip_attribution;
  /// The underlyings' latest prices, which mark and trade delivered shares.
  std::map<std::string, Mark> stock_marks;
  /// The latest values conditional triggers watch, by indicator_key. Entries are
  /// only replaced, never removed, and the field is journaled once it has one.
  std::map<std::string, Mark> indicators;
  /// Journaled once the account has created an alert, so other journals keep their bytes.
  AlertBook alerts;
  SharedVector<StockFill> stock_fills;
  SharedVector<DividendPayment> dividends;
  SharedMap<std::string, ClosingPrint> closing_prints;
  /// Derived, never journaled: the IDs of the orders open when the orders were
  /// last indexed, in ID order, and how many orders there were then. A closed
  /// order never reopens, so those still open and the orders placed since are
  /// every open order, and a scan for them costs the open orders, not the past.
  std::vector<OrderId> working;
  std::size_t indexed = 0;
  /// Derived, never journaled: the first order each client order ID named.
  SharedMap<std::string, OrderId> clients;
  /// Derived, never journaled: see Reviewing.
  Reviewing reviewing;
  /// Transient, never journaled: the settlement reference of each contract settled
  /// by the transaction in progress, the underlying's price for the last review
  /// sample of the round trip it closed. update_reviews consumes and clears it.
  std::map<std::string, Money> settling;
  /// Longs the trader instructed not to exercise at expiry, by symbol.
  SharedSet<std::string> do_not_exercise;
};
/// The open orders' IDs, in ID order.
inline std::vector<OrderId> open_ids(const State& s) {
  std::vector<OrderId> ids;
  for (const auto id : s.working)
    if (id <= s.orders.size() && s.orders[id - 1].open()) ids.push_back(id);
  for (auto i = s.indexed; i < s.orders.size(); ++i)
    if (s.orders[i].open()) ids.push_back(static_cast<OrderId>(i + 1));
  return ids;
}
inline void reindex(State& s) {
  s.working = open_ids(s);
  s.indexed = s.orders.size();
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Book, quote, bid_left, ask_left)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mark, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Reference, quantity, mark, valuation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlertBook, created, items)
/// Every journaled field of State, once: its JSON writer and the journal's
/// change finder (state_change) both come from this list.
#define OPENPORT_STATE_FIELDS(X) \
  X(config) X(time) X(version) X(limits_revision) X(ledger) X(start_equity) X(day) X(contracts) X(books) \
  X(marks) X(valuations) X(orders) X(fills) X(settled) X(kill) X(kill_reason) X(kill_history) X(evaluation) X(attempts) \
  X(closures) X(annotations) X(references) X(explained) X(stock_marks) X(stock_fills) X(dividends) \
  X(closing_prints) X(day_notes) X(trade_reviews) X(strategy_reviews) X(pending_limits) \
  X(pending_guardrails) X(guardrails) X(pending_applied_at) X(trips) X(trip_attribution) X(groups) X(group_reviews) X(grouped)
inline void to_json(Json& j, const State& s) {
#define OPENPORT_STATE_TO(field) j[#field] = s.field;
  OPENPORT_STATE_FIELDS(OPENPORT_STATE_TO)
#undef OPENPORT_STATE_TO
  // Written once used, so accounts that never give an instruction keep their bytes.
  if (!s.do_not_exercise.empty()) j["do_not_exercise"] = s.do_not_exercise;
  // Absent until a conditional trigger first reads one, so other journals keep their bytes.
  if (!s.indicators.empty()) j["indicators"] = s.indicators;
  if (s.alerts.created > 0) j["alerts"] = s.alerts;
}
inline void from_json(const Json& j, State& s) {
  j.at("config").get_to(s.config); j.at("time").get_to(s.time); j.at("version").get_to(s.version);
  j.at("limits_revision").get_to(s.limits_revision); j.at("ledger").get_to(s.ledger);
  j.at("start_equity").get_to(s.start_equity); j.at("day").get_to(s.day); j.at("contracts").get_to(s.contracts);
  j.at("books").get_to(s.books); j.at("marks").get_to(s.marks); j.at("valuations").get_to(s.valuations);
  j.at("orders").get_to(s.orders); j.at("fills").get_to(s.fills); j.at("settled").get_to(s.settled);
  j.at("kill").get_to(s.kill); j.at("kill_reason").get_to(s.kill_reason);
  added_field(j, "kill_history", s.kill_history);
  added_field(j, "evaluation", s.evaluation); added_field(j, "attempts", s.attempts);
  added_field(j, "closures", s.closures); added_field(j, "annotations", s.annotations);
  added_field(j, "references", s.references); added_field(j, "explained", s.explained);
  added_field(j, "stock_marks", s.stock_marks); added_field(j, "stock_fills", s.stock_fills);
  added_field(j, "dividends", s.dividends); added_field(j, "closing_prints", s.closing_prints);
  added_field(j, "day_notes", s.day_notes); added_field(j, "trade_reviews", s.trade_reviews);
  added_field(j, "strategy_reviews", s.strategy_reviews);
  added_field(j, "pending_limits", s.pending_limits); added_field(j, "pending_guardrails", s.pending_guardrails);
  added_field(j, "guardrails", s.guardrails); added_field(j, "pending_applied_at", s.pending_applied_at);
  added_field(j, "indicators", s.indicators);
  added_field(j, "alerts", s.alerts);
  added_field(j, "do_not_exercise", s.do_not_exercise);
  added_field(j, "trips", s.trips); added_field(j, "trip_attribution", s.trip_attribution);
  added_field(j, "groups", s.groups); added_field(j, "group_reviews", s.group_reviews);
  s.grouped = s.fills.size();
  added_field(j, "grouped", s.grouped);
  s.working.clear();
  s.indexed = 0;
  reindex(s);
  // Each client order ID names the first order that used it in the latest attempt
  // that did, as add_order kept it.
  s.clients.clear();
  std::vector<OrderId> starts;
  for (const auto& attempt : s.attempts) starts.push_back(attempt.first_order);
  starts.push_back(s.evaluation.first_order);
  for (const auto& order : s.orders) {
    const auto later = std::upper_bound(starts.begin(), starts.end(), order.id);
    const OrderId start = later == starts.begin() ? 0 : *(later - 1);
    const auto& key = order.request.client_order_id;
    if (const auto first = s.clients.find(key); first == s.clients.end() || first->second < start) s.clients[key] = order.id;
  }
  s.reviewing = {};
  s.settling.clear();
}
}  // namespace detail
}  // namespace openport::trading
