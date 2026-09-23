#include "paper_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

#include "openport/server/plans.hpp"
#include "openport/trading/history.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
using namespace trading;

json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
json nullable(const std::string& value) { return value.empty() ? json(nullptr) : json(value); }
json money(const std::optional<Money>& value) { return value ? json(value->str()) : json(nullptr); }
json positive(Money value) { return value > Money{} ? json(value.str()) : json(nullptr); }
const char* status_name(EvaluationStatus status) {
  constexpr const char* names[] = {"active", "passed", "failed"};
  return names[static_cast<int>(status)];
}
json payout_rules_json(const PayoutRules& p) {
  json caps = json::array();
  for (const auto cap : p.caps) caps.push_back(cap.str());
  return {{"qualifying_profit", p.qualifying_profit.str()}, {"qualifying_days", p.qualifying_days},
          {"withdrawal_percent", p.withdrawal_percent}, {"split_percent", p.split_percent},
          {"minimum", p.minimum.str()}, {"caps", caps}};
}
json rules_json(const AccountRules& r) {
  const bool funded = r.phase == Phase::Funded;
  return {{"plan", nullable(r.plan)}, {"phase", funded ? "funded" : "evaluation"},
          {"profit_target", positive(r.profit_target)}, {"max_drawdown", positive(r.max_drawdown)},
          {"drawdown_mode", r.drawdown_mode == DrawdownMode::Intraday ? "intraday" : "end_of_day"},
          {"lock_balance", positive(r.lock_balance)},
          {"buy_only", r.buy_only}, {"buying_power", r.buying_power},
          {"expiry_cutoff_seconds", r.expiry_cutoff / md::kNanosPerSecond},
          {"payouts", funded ? payout_rules_json(r.payouts) : json(nullptr)}};
}
json decision_json(const Decision& d) {
  if (d.ok()) return nullptr;
  return {{"code", to_string(d.code)}, {"message", d.message},
          {"actual", d.actual ? number(*d.actual) : json(nullptr)}, {"limit", d.limit ? number(*d.limit) : json(nullptr)}};
}
/// The next payout's requirements, or null outside the funded phase.
json payout_json(const TradingView& view) {
  const auto& r = view.config.rules;
  const auto q = payout_quote(*view.snapshot, r);
  if (!q.funded) return nullptr;
  return {{"eligible", q.blocked.ok()}, {"blocked", decision_json(q.blocked)}, {"number", q.number},
          {"active", q.active}, {"flat", q.flat}, {"qualifying_days", q.qualifying_days},
          {"required_days", q.required_days}, {"qualifying_profit", r.payouts.qualifying_profit.str()},
          {"profit", q.profit.str()},
          {"withdrawable", q.withdrawable.str()}, {"cap", money(q.cap)}, {"maximum", q.maximum.str()},
          {"minimum", q.minimum.str()}, {"trader_share", q.trader_share.str()},
          {"withdrawal_percent", r.payouts.withdrawal_percent}, {"split_percent", r.payouts.split_percent}};
}
json buying_power_json(const BuyingPower& power) {
  return {{"available", power.available.str()}, {"reserved", power.reserved.str()},
          {"short_requirement", power.short_requirement.str()}};
}
/// Per-unit average of a notional over whole contracts, nearest micro-dollar.
json average(Money notional, Quantity contracts) {
  return contracts > 0 ? json(notional.prorate(1, contracts).str()) : json(nullptr);
}
std::string underlying(const TradingView& view, const std::string& symbol) {
  const auto it = view.contracts.find(symbol);
  if (it != view.contracts.end()) return it->second.underlying;
  const auto parsed = md::parse_osi(symbol);
  return parsed ? parsed->underlying : "";
}
json trigger_json(const std::optional<Trigger>& t) {
  if (!t) return nullptr;
  return {{"source", t->source == TriggerSource::Option ? "option" : "underlying"},
          {"direction", t->direction == TriggerDirection::AtOrBelow ? "at_or_below" : "at_or_above"},
          {"level", t->level.str()}};
}
json exit_json(const std::optional<ExitSpec>& e) {
  if (!e) return nullptr;
  return {{"trigger", trigger_json(e->trigger)}, {"limit_price", money(e->limit_price)}};
}
json id_or_null(OrderId id) { return id == 0 ? json(nullptr) : json(std::to_string(id)); }
const char* side_name(Side side) { return side == Side::Buy ? "buy" : "sell"; }
json order_json(const Order& o, const TradingView& view) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  constexpr const char* roles[] = {"", "stop_loss", "take_profit"};
  const bool multi = multi_leg(o.request);
  json legs = nullptr;
  if (multi) {
    legs = json::array();
    for (const auto& leg : o.request.legs)
      legs.push_back({{"symbol", leg.symbol}, {"side", side_name(leg.side)}, {"ratio", leg.ratio}});
  }
  return {{"id", std::to_string(o.id)}, {"client_order_id", o.request.client_order_id},
          {"symbol", multi ? json(nullptr) : json(o.request.symbol)},
          {"underlying", underlying(view, order_symbols(o.request).front())},
          {"side", multi ? json(nullptr) : json(side_name(o.request.side))}, {"legs", legs},
          {"type", o.request.type == OrderType::Limit ? "limit" : "market"},
          {"time_in_force", o.request.tif == TimeInForce::Day ? "day" : "ioc"},
          {"quantity", o.request.quantity}, {"filled_quantity", o.filled_quantity},
          {"remaining_quantity", o.remaining()}, {"limit_price", money(o.request.limit_price)},
          {"average_fill_price", o.filled_quantity > 0
              ? json(o.filled_notional.prorate(1, o.filled_quantity).str()) : json(nullptr)},
          {"status", statuses[static_cast<int>(o.status)]},
          {"reason", o.reason.ok() ? json(nullptr) : json{{"code", to_string(o.reason.code)}, {"message", o.reason.message}}},
          {"accepted_at", md::format_timestamp(o.accepted_at)},
          {"day_end", o.day_end > 0 ? json(md::format_timestamp(o.day_end)) : json(nullptr)},
          {"origin", o.system ? "system" : "user"},
          {"trigger", trigger_json(o.request.trigger)},
          {"triggered_at", o.triggered_at > 0 ? json(md::format_timestamp(o.triggered_at)) : json(nullptr)},
          {"bracket", o.request.bracket ? json{{"stop_loss", exit_json(o.request.bracket->stop_loss)},
                                               {"take_profit", exit_json(o.request.bracket->take_profit)}} : json(nullptr)},
          {"role", nullable(roles[static_cast<int>(o.role)])}, {"parent", id_or_null(o.parent)}, {"oco", id_or_null(o.oco)},
          {"stop_loss_order", id_or_null(o.stop_loss)}, {"take_profit_order", id_or_null(o.take_profit)}};
}
json fill_json(const Fill& f, const TradingView& view) {
  return {{"id", std::to_string(f.id)}, {"order_id", std::to_string(f.order_id)},
          {"symbol", f.symbol}, {"underlying", underlying(view, f.symbol)},
          {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity},
          {"price", f.price.str()}, {"fee", f.fee.str()},
          {"quote_time", md::format_timestamp(f.quote_time)}, {"time", md::format_timestamp(f.time)}};
}
json position_greeks(const MarkedPosition& p, const TradingView& view) {
  json out = {{"delta", nullptr}, {"gamma", nullptr}, {"vega", nullptr}, {"theta", nullptr},
              {"dollar_delta", nullptr}, {"dollar_gamma_1pct", nullptr},
              {"vega_dollars", nullptr}, {"theta_dollars", nullptr}};
  const auto it = view.valuations.find(p.position.contract.osi_symbol());
  if (it == view.valuations.end() || p.awaiting_settlement) return out;
  const auto& v = it->second;
  const auto time = view.snapshot->time;
  if (!valid_valuation(v) || v.time > time || time - v.time > view.config.limits.max_valuation_age) return out;
  const auto units = static_cast<double>(p.position.quantity) * 100;
  out["delta"] = number(v.delta); out["gamma"] = number(v.gamma);
  out["vega"] = number(v.vega); out["theta"] = number(v.theta);
  out["dollar_delta"] = number(units * v.delta * v.spot);
  out["dollar_gamma_1pct"] = number(units * v.gamma * v.spot * v.spot * .01);
  out["vega_dollars"] = number(units * v.vega);
  out["theta_dollars"] = number(units * v.theta);
  return out;
}
Money average_price(const Position& position) {
  // Divide basis by signed contracts * multiplier in one rounding step; two
  // successive Money divisions can double-round a micro-dollar average.
  __extension__ using Wide = __int128;
  Wide numerator = position.basis.micros();
  Wide denominator = static_cast<Wide>(position.quantity) * 100;
  if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
  const bool negative = numerator < 0;
  if (negative) numerator = -numerator;
  const auto rounded = static_cast<std::int64_t>((numerator + denominator / 2) / denominator);
  return Money::from_micros(negative ? -rounded : rounded);
}
json portfolio_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  json positions = json::array();
  for (const auto& p : s.positions) {
    const auto& position = p.position;
    const auto& c = position.contract;
    const auto q = position.quantity;
    const auto average = average_price(position);
    positions.push_back({{"symbol", c.osi_symbol()}, {"underlying", c.underlying},
        {"expiry", md::format_date(c.expiry)}, {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"},
        {"strike", c.strike}, {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"quantity", q}, {"average_price", average.str()}, {"basis", position.basis.str()},
        {"mark", money(p.mark)}, {"mark_age_seconds", p.mark ? json(static_cast<double>(p.mark_age) / md::kNanosPerSecond) : json(nullptr)},
        {"market_value", money(p.market_value)}, {"unrealised", money(p.unrealised)},
        {"realised", position.realised.str()}, {"fees", position.fees.str()}, {"fresh", p.fresh},
        {"awaiting_settlement", p.awaiting_settlement}, {"greeks", position_greeks(p, view)}});
  }
  json flags = json::array();
  for (auto code : s.quality_flags) flags.push_back(to_string(code));
  return {{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
          {"cash", s.account.cash.str()}, {"equity", s.equity.str()},
          {"start_of_day_equity", s.start_of_day_equity.str()}, {"day_pnl", (s.equity - s.start_of_day_equity).str()},
          {"realised", s.account.realised.str()}, {"unrealised", s.unrealised.str()}, {"fees", s.account.fees.str()},
          {"valuation_complete", s.valuation_complete}, {"quality_flags", flags}, {"positions", positions},
          {"buying_power", buying_power_json(s.buying_power)}};
}
json account_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  const auto& r = view.config.rules;
  const auto& e = s.evaluation;
  const bool marked = std::all_of(s.positions.begin(), s.positions.end(), [](const auto& p) { return p.market_value.has_value(); });
  const bool floor = r.max_drawdown > Money{};
  const bool target = r.profit_target > Money{};
  const bool decided = e.status != EvaluationStatus::Active;
  const Money target_equity = e.starting_balance + r.profit_target;
  json days = json::array();
  for (const auto& d : e.days)
    days.push_back({{"day", md::format_date(d.day)}, {"open_equity", d.open_equity.str()},
                    {"close_equity", d.close_equity.str()}, {"peak", d.peak.str()},
                    {"floor", floor ? json(d.floor.str()) : json(nullptr)},
                    {"realised", d.realised.str()}, {"qualifying", d.qualifying}});
  json payouts = json::array();
  for (const auto& p : e.payouts)
    payouts.push_back({{"number", p.number}, {"time", md::format_timestamp(p.time)}, {"day", md::format_date(p.day)}, {"amount", p.amount.str()},
                       {"trader_share", p.trader_share.str()}, {"balance", p.balance.str()}});
  json attempts = json::array();
  for (const auto& a : s.attempts)
    attempts.push_back({{"attempt", a.attempt}, {"plan", nullable(a.plan)}, {"started", md::format_timestamp(a.started)},
                        {"ended", md::format_timestamp(a.ended)}, {"starting_balance", a.starting_balance.str()},
                        {"final_equity", a.final_equity.str()}, {"status", status_name(a.status)},
                        {"decision", nullable(a.decision)}});
  return {{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
          {"rules", rules_json(r)},
          {"evaluation", {
              {"enabled", r.evaluation()}, {"attempt", e.attempt}, {"status", status_name(e.status)},
              {"started", md::format_timestamp(e.started)}, {"starting_balance", e.starting_balance.str()},
              {"equity", s.equity.str()}, {"marked", marked}, {"valuation_complete", s.valuation_complete},
              {"profit", (s.equity - e.starting_balance).str()}, {"peak", e.peak.str()},
              {"floor", floor ? json(e.floor.str()) : json(nullptr)},
              {"drawdown_buffer", floor ? json((s.equity - e.floor).str()) : json(nullptr)},
              {"target_equity", target ? json(target_equity.str()) : json(nullptr)},
              {"target_remaining", target ? json(std::max(Money{}, target_equity - s.equity).str()) : json(nullptr)},
              {"decided_at", decided ? json(md::format_timestamp(e.decided_at)) : json(nullptr)},
              {"decided_equity", decided ? json(e.decided_equity.str()) : json(nullptr)},
              {"decision", nullable(e.decision)},
              {"day", md::format_date(e.day)}, {"day_open_equity", e.day_open_equity.str()},
              {"day_close_equity", e.day_close_equity.str()}, {"days", days},
              {"floor_locked", floor && e.floor_locked}, {"qualifying_days", e.qualifying_days},
              {"cycle_started", md::format_timestamp(e.cycle_started)}, {"payouts", payouts}}},
          {"buying_power", buying_power_json(s.buying_power)},
          {"payout", payout_json(view)},
          {"attempts", attempts}};
}
json trades_json(const TradingView& view, std::string_view status, bool current_only) {
  const auto& s = *view.snapshot;
  const auto& e = s.evaluation;
  auto attempt_of = [&](std::uint64_t first_fill) {
    if (first_fill >= e.first_fill) return e.attempt;
    for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
      if (first_fill >= it->first_fill) return it->attempt;
    return std::uint64_t{1};
  };
  const auto all = lifecycles(s.recent_fills, s.closures, view.contracts);
  json trades = json::array();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = attempt_of(t.first_fill);
    if (current_only && attempt != e.attempt) continue;
    const auto& c = t.contract;
    const Money cost = t.open_notional * 100;
    const Money net = t.gross - t.fees;
    json mark = nullptr, unrealised = nullptr;
    if (open) {
      for (const auto& p : s.positions) {
        if (p.position.contract.osi_symbol() != t.symbol) continue;
        mark = money(p.mark);
        unrealised = money(p.unrealised);
      }
    }
    json fills = json::array();
    for (const auto id : t.fills) fills.push_back(std::to_string(id));
    trades.push_back({{"id", std::to_string(t.first_fill)}, {"attempt", attempt}, {"symbol", t.symbol},
        {"underlying", c.underlying}, {"expiry", md::format_date(c.expiry)},
        {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"}, {"strike", c.strike},
        {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"direction", t.direction > 0 ? "long" : "short"}, {"status", open ? "open" : "closed"},
        {"opened", md::format_timestamp(t.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(*t.closed))},
        {"duration_seconds", open ? json(nullptr) : json((*t.closed - t.opened) / md::kNanosPerSecond)},
        {"quantity", t.quantity}, {"max_quantity", t.max_quantity},
        {"opened_contracts", t.opened_contracts}, {"closed_contracts", t.closed_contracts},
        {"average_open", average(t.open_notional, t.opened_contracts)},
        {"average_close", average(t.close_notional, t.closed_contracts)},
        {"cost", cost.str()}, {"gross", t.gross.str()}, {"fees", t.fees.str()}, {"net", net.str()},
        {"return", open || cost == Money{} ? json(nullptr) : number(net.dollars() / cost.dollars())},
        {"mark", mark}, {"unrealised", unrealised},
        {"closure", !t.closure ? json(nullptr) : json(*t.closure == ClosureKind::Settlement ? "settlement" : "reset")},
        {"fills", fills}});
  }
  return {{"account_version", std::to_string(s.account_version)}, {"attempt", e.attempt}, {"trades", trades}};
}
json plans_json() {
  json plans = json::array();
  for (const auto& p : plan_presets())
    plans.push_back({{"id", p.id}, {"name", p.name}, {"summary", p.summary},
                     {"initial_cash", p.initial_cash.str()}, {"rules", rules_json(p.rules)},
                     {"unlocked_by", nullable(p.unlocked_by)}});
  return {{"plans", plans}};
}
json exposure_limits(const ExposureLimits& limits) {
  return {{"dollar_delta", limits.dollar_delta}, {"vega", limits.vega}};
}
json limits_json(const Limits& limits) {
  return {{"max_order_contracts", limits.max_order_contracts}, {"price_band_absolute", limits.price_band_absolute.str()},
          {"price_band_relative", limits.price_band_relative}, {"aggregate", exposure_limits(limits.aggregate)},
          {"per_underlying", exposure_limits(limits.per_underlying)}, {"max_daily_loss", limits.max_daily_loss.str()},
          {"max_quote_age_seconds", limits.max_quote_age / md::kNanosPerSecond},
          {"max_valuation_age_seconds", limits.max_valuation_age / md::kNanosPerSecond}};
}
json bucket_json(const RiskBucket& b) {
  return {{"dollar_delta", b.position.dollar_delta}, {"dollar_gamma_1pct", b.position.dollar_gamma_1pct},
          {"vega", b.position.vega}, {"theta", b.position.theta},
          {"reachable", {{"delta_low", b.reachable.delta_low}, {"delta_high", b.reachable.delta_high},
                         {"vega_low", b.reachable.vega_low}, {"vega_high", b.reachable.vega_high}}},
          {"limits", exposure_limits(b.limits)}, {"delta_utilisation", b.delta_utilisation}, {"vega_utilisation", b.vega_utilisation}};
}
json kill_json(const RiskSnapshot& risk) {
  return {{"latched", risk.kill_latched}, {"reason", nullable(risk.kill_reason)}};
}
json risk_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  json underlyings = json::object();
  for (const auto& [symbol, bucket] : s.risk.underlyings) underlyings[symbol] = bucket_json(bucket);
  json pnl = json::array(), clamped = json::array();
  std::size_t index = 0;
  for ([[maybe_unused]] double spot : view.config.scenarios.spot_percent) {
    json row = json::array(), clamps = json::array();
    for ([[maybe_unused]] double vol : view.config.scenarios.vol_points) {
      const auto& cell = s.scenarios.cells.at(index++);
      row.push_back(s.scenarios.complete ? number(cell.pnl) : json(nullptr));
      clamps.push_back(cell.clamped);
    }
    pnl.push_back(std::move(row)); clamped.push_back(std::move(clamps));
  }
  return {{"account_version", std::to_string(s.account_version)}, {"limits_revision", std::to_string(s.risk.limits_revision)},
          {"limits", limits_json(view.config.limits)}, {"complete", s.risk.complete}, {"daily_loss", s.risk.daily_loss.str()},
          {"kill", kill_json(s.risk)}, {"aggregate", bucket_json(s.risk.aggregate)}, {"underlyings", underlyings},
          {"scenarios", {{"spot_percent", view.config.scenarios.spot_percent}, {"vol_points", view.config.scenarios.vol_points},
                         {"pnl", pnl}, {"clamped", clamped}, {"complete", s.scenarios.complete}}}};
}

int reason_status(Reason reason) {
  if (reason == Reason::UNKNOWN_ORDER || reason == Reason::UNKNOWN_CONTRACT) return 404;
  if (reason == Reason::ORDER_TERMINAL || reason == Reason::DUPLICATE_CLIENT_ID) return 409;
  if (reason == Reason::JOURNAL_IO || reason == Reason::JOURNAL_CORRUPT ||
      reason == Reason::JOURNAL_LOCKED) return 503;
  return 422;
}
ApiResponse command_response(const TradingCommand& command, const TradingReply& reply) {
  if (!reply.error_code.empty())
    return api_error(reply.error_code == "LIMITS_REVISION" ? 409 : 503, reply.error_code, reply.decision.message);
  if (!reply.decision.ok()) return api_error(reason_status(reply.decision.code),
      std::string(to_string(reply.decision.code)), reply.decision.message, reply.decision);
  if (!reply.view || !reply.view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "No trading publication");
  const auto& view = *reply.view;
  const auto& s = *view.snapshot;
  json body{{"account_version", std::to_string(s.account_version)}};
  int status = 200;
  switch (command.kind) {
    case TradingCommand::Kind::Submit:
    case TradingCommand::Kind::Cancel: {
      const auto it = std::find_if(s.recent_orders.begin(), s.recent_orders.end(), [&](const auto& o) { return o.id == reply.order_id; });
      if (it == s.recent_orders.end()) return api_error(503, "TRADING_UNAVAILABLE", "Order publication missing");
      body["order"] = order_json(*it, view);
      if (command.kind == TradingCommand::Kind::Submit) {
        status = 201; body["fills"] = json::array();
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == it->id) body["fills"].push_back(fill_json(fill, view));
      }
      break;
    }
    case TradingCommand::Kind::Limits: body = risk_json(view); break;
    case TradingCommand::Kind::Trip:
    case TradingCommand::Kind::Reset:
      body["kill"] = kill_json(s.risk); body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      break;
    case TradingCommand::Kind::Settle: body["position_closed"] = true; break;
    case TradingCommand::Kind::ResetAccount:
    case TradingCommand::Kind::Payout: body = account_json(view); break;
  }
  return {status, body.dump()};
}

void fields(const json& object, std::initializer_list<std::string_view> required,
             std::initializer_list<std::string_view> optional = {}) {
  if (!object.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto field : required) if (!object.contains(field)) throw std::invalid_argument("Missing field: " + std::string(field));
  for (auto it = object.begin(); it != object.end(); ++it)
    if (std::find(required.begin(), required.end(), it.key()) == required.end() &&
        std::find(optional.begin(), optional.end(), it.key()) == optional.end())
      throw std::invalid_argument("Unknown field: " + it.key());
}
std::string string_field(const json& j, const char* key) {
  if (!j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
  return j.at(key).get<std::string>();
}
std::int64_t integer_field(const json& j, const char* key) {
  const auto& value = j.at(key);
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::uint64_t(INT64_MAX)))
    throw std::invalid_argument(std::string(key) + " must be a signed 64-bit integer");
  return value.get<std::int64_t>();
}
double number_field(const json& j, const char* key) {
  if (!j.at(key).is_number()) throw std::invalid_argument(std::string(key) + " must be a number");
  const double value = j.at(key).get<double>();
  if (!std::isfinite(value)) throw std::invalid_argument(std::string(key) + " must be finite");
  return value;
}
std::uint64_t identifier(std::string_view text) {
  std::uint64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("Expected an unsigned decimal ID string");
  return id;
}
Money decimal_field(const json& j, const char* key) {
  try { return Money::parse(string_field(j, key)); }
  catch (const TradingError& e) { throw std::invalid_argument(std::string(key) + ": " + e.what()); }
}
std::string symbol_field(const json& j) {
  const auto symbol = string_field(j, "symbol");
  const auto contract = md::parse_osi(symbol);
  if (!contract || contract->osi_symbol() != symbol) throw std::invalid_argument("symbol must be a canonical padded OSI");
  return symbol;
}
ExposureLimits parse_exposure(const json& j) {
  fields(j, {"dollar_delta", "vega"});
  return {number_field(j, "dollar_delta"), number_field(j, "vega")};
}
Limits parse_limits(const json& j) {
  fields(j, {"max_order_contracts", "price_band_absolute", "price_band_relative", "aggregate", "per_underlying",
             "max_daily_loss", "max_quote_age_seconds", "max_valuation_age_seconds"});
  Limits limits;
  limits.max_order_contracts = integer_field(j, "max_order_contracts");
  limits.price_band_absolute = decimal_field(j, "price_band_absolute");
  limits.price_band_relative = number_field(j, "price_band_relative");
  limits.aggregate = parse_exposure(j.at("aggregate"));
  limits.per_underlying = parse_exposure(j.at("per_underlying"));
  limits.max_daily_loss = decimal_field(j, "max_daily_loss");
  auto age = [&](const char* key) {
    const auto seconds = integer_field(j, key);
    if (seconds > INT64_MAX / md::kNanosPerSecond || seconds < 0)
      throw TradingError(Reason::INVALID_LIMITS, "Age must be nonnegative and representable in nanoseconds");
    return seconds * md::kNanosPerSecond;
  };
  limits.max_quote_age = age("max_quote_age_seconds");
  limits.max_valuation_age = age("max_valuation_age_seconds");
  return limits;
}
Trigger parse_trigger(const json& j) {
  fields(j, {"source", "direction", "level"});
  const auto source = string_field(j, "source"), direction = string_field(j, "direction");
  if ((source != "option" && source != "underlying") || (direction != "at_or_below" && direction != "at_or_above"))
    throw std::invalid_argument("trigger source must be option or underlying, direction at_or_below or at_or_above");
  return {source == "option" ? TriggerSource::Option : TriggerSource::Underlying,
          direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove, decimal_field(j, "level")};
}
ExitSpec parse_exit(const json& j) {
  fields(j, {}, {"trigger", "limit_price"});
  ExitSpec exit;
  if (j.contains("trigger")) exit.trigger = parse_trigger(j.at("trigger"));
  if (j.contains("limit_price")) exit.limit_price = decimal_field(j, "limit_price");
  if (exit.trigger.has_value() == exit.limit_price.has_value())
    throw std::invalid_argument("A bracket exit takes either a trigger or a limit_price");
  return exit;
}
bool boolean_field(const json& j, const char* key) {
  if (!j.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
  return j.at(key).get<bool>();
}
PayoutRules parse_payout_rules(const json& j) {
  fields(j, {"qualifying_profit", "qualifying_days", "withdrawal_percent", "split_percent", "minimum", "caps"});
  PayoutRules p;
  p.qualifying_profit = decimal_field(j, "qualifying_profit");
  p.qualifying_days = integer_field(j, "qualifying_days");
  if (p.qualifying_days < 1) throw std::invalid_argument("qualifying_days must be at least 1");
  p.withdrawal_percent = integer_field(j, "withdrawal_percent");
  p.split_percent = integer_field(j, "split_percent");
  p.minimum = decimal_field(j, "minimum");
  const auto& caps = j.at("caps");
  if (!caps.is_array() || caps.size() > 64) throw std::invalid_argument("caps must be an array of at most 64 amounts");
  for (const auto& cap : caps) {
    if (!cap.is_string()) throw std::invalid_argument("caps must be decimal strings");
    try { p.caps.push_back(Money::parse(cap.get<std::string>())); }
    catch (const TradingError& e) { throw std::invalid_argument(std::string("caps: ") + e.what()); }
  }
  return p;
}
/// Custom rules: nullable money for an absent target/drawdown, like rules_json.
/// The phase defaults to evaluation; a funded phase requires payout rules.
AccountRules parse_rules(const json& j) {
  fields(j, {"profit_target", "max_drawdown", "drawdown_mode", "buy_only", "buying_power", "expiry_cutoff_seconds"},
         {"plan", "phase", "lock_balance", "payouts"});
  AccountRules rules;
  if (j.contains("phase")) {
    const auto phase = string_field(j, "phase");
    if (phase != "evaluation" && phase != "funded") throw std::invalid_argument("phase must be evaluation or funded");
    rules.phase = phase == "funded" ? Phase::Funded : Phase::Evaluation;
  }
  const bool payouts = j.contains("payouts") && !j.at("payouts").is_null();
  if (payouts != (rules.phase == Phase::Funded))
    throw std::invalid_argument("payouts are required for the funded phase and forbidden otherwise");
  if (payouts) rules.payouts = parse_payout_rules(j.at("payouts"));
  if (j.contains("lock_balance") && !j.at("lock_balance").is_null()) rules.lock_balance = decimal_field(j, "lock_balance");
  if (j.contains("plan") && !j.at("plan").is_null()) rules.plan = string_field(j, "plan");
  auto optional_money = [&](const char* key) { return j.at(key).is_null() ? Money{} : decimal_field(j, key); };
  rules.profit_target = optional_money("profit_target");
  rules.max_drawdown = optional_money("max_drawdown");
  const auto mode = string_field(j, "drawdown_mode");
  if (mode != "intraday" && mode != "end_of_day") throw std::invalid_argument("drawdown_mode must be intraday or end_of_day");
  rules.drawdown_mode = mode == "intraday" ? DrawdownMode::Intraday : DrawdownMode::EndOfDay;
  rules.buy_only = boolean_field(j, "buy_only");
  rules.buying_power = boolean_field(j, "buying_power");
  const auto cutoff = integer_field(j, "expiry_cutoff_seconds");
  if (cutoff < 0 || cutoff >= 86'400) throw std::invalid_argument("expiry_cutoff_seconds must be in [0, 86400)");
  rules.expiry_cutoff = cutoff * md::kNanosPerSecond;
  validate_rules(rules);
  return rules;
}
json strict_json(const std::string& body) {
  // JSON parsers normally keep the last duplicate key; that is ambiguous for orders.
  std::vector<std::set<std::string>> keys;
  return json::parse(body, [&](int, json::parse_event_t event, json& parsed) {
    if (event == json::parse_event_t::object_start) keys.emplace_back();
    if (event == json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
      throw std::invalid_argument("Duplicate JSON field");
    if (event == json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
}
TradingCommand parse_command(const ApiRequest& request) {
  TradingCommand command;
  if (request.method == "DELETE") {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::Cancel;
    command.order_id = identifier(std::string_view(request.target).substr(std::string_view("/api/orders/").size()));
    return command;
  }
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  const auto body = strict_json(request.body);
  if (request.target == "/api/orders") {
    // A single contract (symbol and side), or legs for a multi-leg order.
    const bool legs = body.is_object() && body.contains("legs");
    if (legs) fields(body, {"client_order_id", "legs", "type", "quantity", "time_in_force"}, {"limit_price"});
    else fields(body, {"client_order_id", "symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket"});
    auto& order = command.order;
    order.client_order_id = string_field(body, "client_order_id");
    const auto type = string_field(body, "type"), tif = string_field(body, "time_in_force");
    if ((type != "limit" && type != "market") || (tif != "day" && tif != "ioc"))
      throw std::invalid_argument("Invalid type or time_in_force");
    order.type = type == "limit" ? OrderType::Limit : OrderType::Market;
    order.tif = tif == "day" ? TimeInForce::Day : TimeInForce::Ioc;
    order.quantity = integer_field(body, "quantity");
    if ((order.type == OrderType::Limit) != body.contains("limit_price"))
      throw std::invalid_argument("limit_price is required for limit orders and forbidden for market orders");
    if (body.contains("limit_price")) order.limit_price = decimal_field(body, "limit_price");
    if (legs) {
      const auto& list = body.at("legs");
      if (!list.is_array() || list.size() < 2 || list.size() > kMaxLegs)
        throw std::invalid_argument("legs must be an array of two to four legs");
      for (const auto& item : list) {
        fields(item, {"symbol", "side"}, {"ratio"});
        Leg leg;
        leg.symbol = symbol_field(item);
        const auto side = string_field(item, "side");
        if (side != "buy" && side != "sell") throw std::invalid_argument("Leg side must be buy or sell");
        leg.side = side == "buy" ? Side::Buy : Side::Sell;
        if (item.contains("ratio")) leg.ratio = integer_field(item, "ratio");
        order.legs.push_back(std::move(leg));
      }
      return command;
    }
    order.symbol = symbol_field(body);
    const auto side = string_field(body, "side");
    if (side != "buy" && side != "sell") throw std::invalid_argument("Invalid side, type or time_in_force");
    order.side = side == "buy" ? Side::Buy : Side::Sell;
    if (body.contains("trigger")) order.trigger = parse_trigger(body.at("trigger"));
    if (body.contains("bracket")) {
      const auto& bracket = body.at("bracket");
      fields(bracket, {}, {"stop_loss", "take_profit"});
      order.bracket = Bracket{};
      if (bracket.contains("stop_loss")) order.bracket->stop_loss = parse_exit(bracket.at("stop_loss"));
      if (bracket.contains("take_profit")) order.bracket->take_profit = parse_exit(bracket.at("take_profit"));
      if (!order.bracket->stop_loss && !order.bracket->take_profit)
        throw std::invalid_argument("A bracket needs a stop_loss, a take_profit or both");
    }
  } else if (request.target == "/api/risk/limits") {
    fields(body, {"expected_revision", "limits"});
    command.kind = TradingCommand::Kind::Limits;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    command.limits = parse_limits(body.at("limits"));
  } else if (request.target == "/api/risk/kill") {
    fields(body, {"action", "reason"});
    const auto action = string_field(body, "action");
    if (action != "trip" && action != "reset") throw std::invalid_argument("action must be trip or reset");
    command.kind = action == "trip" ? TradingCommand::Kind::Trip : TradingCommand::Kind::Reset;
    command.reason = string_field(body, "reason");
  } else if (request.target == "/api/account/reset") {
    // Either a preset ID, or a custom starting balance and complete rules.
    fields(body, {"reason"}, {"plan", "initial_cash", "rules"});
    command.kind = TradingCommand::Kind::ResetAccount;
    command.reason = string_field(body, "reason");
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules"))
        throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
      if (!plan->unlocked_by.empty()) command.required_pass = find_plan(plan->unlocked_by)->name;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
    }
  } else if (request.target == "/api/account/payout") {
    fields(body, {"amount"});
    command.kind = TradingCommand::Kind::Payout;
    command.amount = decimal_field(body, "amount");
  } else {
    fields(body, {"symbol", "value"});
    command.kind = TradingCommand::Kind::Settle;
    command.symbol = symbol_field(body);
    command.settlement = decimal_field(body, "value");
  }
  return command;
}
}  // namespace

ApiResponse api_error(int status, std::string code, std::string message, const Decision& evidence) {
  // Some reducer checks identify a single contract. The HTTP contract exposes
  // risk scope as the underlying, while the order itself carries the OSI.
  const auto contract = md::parse_osi(evidence.scope);
  const auto scope = contract ? contract->underlying : evidence.scope;
  return {status, json{{"error", {{"code", code}, {"message", message},
      {"actual", evidence.actual ? number(*evidence.actual) : json(nullptr)},
      {"limit", evidence.limit ? number(*evidence.limit) : json(nullptr)},
      {"scope", nullable(scope)}}}}.dump()};
}
json trading_status_json(const TradingStatus& status) {
  return {{"enabled", status.enabled}, {"reason", nullable(status.reason)},
          {"account_version", std::to_string(status.account_version)},
          {"kill_latched", status.kill_latched}, {"write", status.write},
          {"fee_per_contract", status.fee_per_contract.str()},
          {"initial_cash", status.initial_cash.str()},
          {"plan", nullable(status.plan)}, {"evaluation", nullable(status.evaluation)}};
}
std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source) {
  const auto question = request.target.find('?');
  const auto path = request.target.substr(0, question);
  if (path != "/api/portfolio" && path != "/api/orders" && path != "/api/fills" && path != "/api/risk" &&
      path != "/api/account" && path != "/api/trades" && path != "/api/plans") return {};
  const auto query = question == std::string::npos ? "" : request.target.substr(question + 1);
  // Trades accept status=open|closed|all and attempt=current|all, each at most once, in any order.
  std::string trade_status = "all", trade_attempt = "current";
  bool valid_query = query.empty() || (path == "/api/orders" && (query == "status=open" || query == "status=all"));
  if (path == "/api/trades" && !query.empty()) {
    valid_query = true;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (valid_query && start <= query.size()) {
      const auto end = std::min(query.find('&', start), query.size());
      const auto pair = query.substr(start, end - start);
      const auto equals = pair.find('=');
      const auto key = pair.substr(0, equals);
      const auto value = equals == std::string::npos ? "" : pair.substr(equals + 1);
      if (!seen.insert(key).second) valid_query = false;
      else if (key == "status" && (value == "open" || value == "closed" || value == "all")) trade_status = value;
      else if (key == "attempt" && (value == "current" || value == "all")) trade_attempt = value;
      else valid_query = false;
      start = end + 1;
    }
  }
  if (!valid_query) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  if (path == "/api/plans") return ApiResponse{200, plans_json().dump()};
  const auto view = source.trading_view();
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  if (path == "/api/portfolio") return ApiResponse{200, portfolio_json(*view).dump()};
  if (path == "/api/risk") return ApiResponse{200, risk_json(*view).dump()};
  if (path == "/api/account") return ApiResponse{200, account_json(*view).dump()};
  if (path == "/api/trades") return ApiResponse{200, trades_json(*view, trade_status, trade_attempt == "current").dump()};
  json body{{"account_version", std::to_string(s.account_version)}};
  if (path == "/api/orders") {
    body["orders"] = json::array();
    for (auto it = s.recent_orders.rbegin(); it != s.recent_orders.rend(); ++it)
      if (query != "status=open" || it->open()) body["orders"].push_back(order_json(*it, *view));
  } else {
    body["fills"] = json::array();
    for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) body["fills"].push_back(fill_json(*it, *view));
  }
  return ApiResponse{200, body.dump()};
}

void handle_api_async(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  if (request.method == "GET") { complete(handle_api(request, source)); return; }
  const bool route = (request.method == "POST" && (request.target == "/api/orders" ||
      request.target == "/api/risk/kill" || request.target == "/api/settlements" ||
      request.target == "/api/account/reset" || request.target == "/api/account/payout")) ||
      (request.method == "PUT" && request.target == "/api/risk/limits") ||
      (request.method == "DELETE" && request.target.starts_with("/api/orders/"));
  if (!route) { complete(api_error(404, "NOT_FOUND", "Unknown endpoint or method")); return; }
  const auto trading = source.status().trading;
  if (trading.reason.starts_with("JOURNAL_LOCKED:")) {
    complete(api_error(503, "TRADING_UNAVAILABLE", trading.reason));
    return;
  }
  try {
    const auto command = parse_command(request);
    if (!source.post_trading(command, [command, complete](TradingReply reply) {
          complete(command_response(command, reply));
        })) complete(api_error(503, "TRADING_UNAVAILABLE", "Command inbox full or trading unavailable"));
  } catch (const TradingError& error) {
    complete(api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what()));
  } catch (const std::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  }
}
}  // namespace openport::server
