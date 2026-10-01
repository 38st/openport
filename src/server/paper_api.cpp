#include "paper_json.hpp"
#include "openport/server/sandboxes.hpp"
#include "playbook_api.hpp"
#include "paper_csv.hpp"

#include <algorithm>
#include <array>
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
  json result = {{"plan", nullable(r.plan)}, {"phase", funded ? "funded" : "evaluation"},
          {"profit_target", positive(r.profit_target)}, {"max_drawdown", positive(r.max_drawdown)},
          {"drawdown_mode", r.drawdown_mode == DrawdownMode::Intraday ? "intraday" : "end_of_day"},
          {"lock_balance", positive(r.lock_balance)},
          {"buy_only", r.buy_only}, {"defined_risk", r.defined_risk}, {"buying_power", r.buying_power},
          {"slippage_ticks", r.slippage_ticks}, {"margin", r.margin == MarginMode::Portfolio ? "portfolio" : "strategy"},
          {"expiry_cutoff_seconds", r.expiry_cutoff / md::kNanosPerSecond},
          {"payouts", funded ? payout_rules_json(r.payouts) : json(nullptr)}};
  if (r.fill_latency_ms != 0) result["fill_latency_ms"] = r.fill_latency_ms;
  if (r.impact_ticks != 0) result["impact_ticks"] = r.impact_ticks;
  return result;
}
/// A check one contract failed reports that contract's underlying as its scope, as
/// HTTP errors do; the journaled decision keeps the OSI, as the order itself does.
json scope_json(const std::string& scope) {
  const auto contract = md::parse_osi(scope);
  return nullable(contract ? contract->underlying : scope);
}
/// A reason with its numeric evidence: how far a check was exceeded, and where.
json decision_json(const Decision& d) {
  if (d.ok()) return nullptr;
  return {{"code", to_string(d.code)}, {"message", d.message},
          {"actual", d.actual ? number(*d.actual) : json(nullptr)}, {"limit", d.limit ? number(*d.limit) : json(nullptr)},
          {"scope", scope_json(d.scope)}};
}
json time_or_null(Timestamp time) { return time > 0 ? json(md::format_timestamp(time)) : json(nullptr); }
json guardrails_json(const Guardrails& g) {
  return {{"soft_floor", g.soft_floor.str()}, {"soft_floor_percent", g.soft_floor_percent},
          {"max_opening_trades", g.max_opening_trades}, {"cooldown_loss", g.cooldown_loss.str()},
          {"cooldown_minutes", g.cooldown_minutes}, {"profit_lock", g.profit_lock.str()}};
}
json guardrail_state_json(const TradingSnapshot& s) {
  json latched = json::array();
  for (auto reason : s.guardrails.latched) latched.push_back(to_string(reason));
  if (s.time < s.guardrails.cooldown_until) latched.push_back("COOLDOWN");
  return {{"opening_trades", s.guardrails.opening_trades}, {"latched", latched},
          {"cooldown_until", time_or_null(s.guardrails.cooldown_until)},
          {"cooldown_seconds", std::max<Timestamp>(0, s.guardrails.cooldown_until - s.time) / md::kNanosPerSecond},
          {"soft_floor", money(s.soft_floor)}};
}
json breach_json(const BreachRisk& b) {
  const auto level = [](const std::optional<BreachLevel>& value) -> json {
    if (!value) return nullptr;
    return {{"points", number(value->points)}, {"percent", number(value->percent)},
            {"touch_probability", value->touch_probability ? number(*value->touch_probability) : json(nullptr)}};
  };
  json underlyings = json::array();
  for (const auto& item : b.underlyings)
    underlyings.push_back({{"underlying", item.underlying}, {"spot", number(item.spot)}, {"complete", item.complete},
        {"close_sigma", item.close_sigma ? number(*item.close_sigma) : json(nullptr)}, {"down", level(item.down)}, {"up", level(item.up)},
        {"soft_down", level(item.soft_down)}, {"soft_up", level(item.soft_up)}});
  return {{"room", money(b.room)}, {"soft_room", money(b.soft_room)}, {"complete", b.complete},
          {"underlyings", underlyings}, {"model", "Driftless log-return reflection estimate; unchanged volatility, one underlying at a time"},
          {"scan_down_percent", -99.75}, {"scan_up_percent", 1000}};
}
json units(const std::optional<Quantity>& value) { return value ? json(*value) : json(nullptr); }
json warnings_json(const std::vector<RiskWarning>& warnings) {
  json out = json::array();
  for (const auto& w : warnings)
    out.push_back({{"code", w.code}, {"severity", w.severity}, {"scope", w.scope},
                   {"symbol", w.symbol.empty() ? json(nullptr) : json(w.symbol)}, {"message", w.message},
                   {"actual", w.actual ? number(*w.actual) : json(nullptr)}, {"limit", w.limit ? number(*w.limit) : json(nullptr)}});
  return out;
}
json preview_fills(const std::vector<PreviewFill>& fills) {
  json out = json::array();
  for (const auto& f : fills)
    out.push_back({{"symbol", f.symbol}, {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity}, {"price", f.price.str()}});
  return out;
}
json execution_json(const PreviewExecution& e) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  return {{"status", statuses[static_cast<int>(e.status)]}, {"filled_quantity", e.filled_quantity},
          {"remaining_quantity", e.remaining_quantity},
          {"reason", e.reason.ok() ? json(nullptr) : json{{"code", to_string(e.reason.code)}, {"message", e.reason.message}}},
          {"fills", preview_fills(e.fills)}, {"average_fill_price", money(e.average_fill_price)},
          {"schedule", preview_fills(e.schedule)}, {"average_price", money(e.average_price)}};
}
json preview_json(const OrderPreview& p) {
  json change = nullptr;
  if (p.exposure_change) change = {{"dollar_delta", number(p.exposure_change->dollar_delta)},
      {"dollar_gamma_1pct", number(p.exposure_change->dollar_gamma_1pct)},
      {"vega", number(p.exposure_change->vega)}, {"theta", number(p.exposure_change->theta)}};
  return {{"decision", p.decision.ok() ? "ok" : to_string(p.decision.code)}, {"reason", decision_json(p.decision)},
      {"buying_power", {{"required", p.buying_power_required.str()}, {"before", p.buying_power_before.str()},
                        {"working", money(p.buying_power_working)}, {"after", money(p.buying_power_after)}}},
      {"exposure_change", change}, {"max_loss", money(p.max_loss)}, {"max_loss_basis", nullable(p.max_loss_basis)},
      {"equity_at_max_loss", money(p.equity_at_max_loss)},
      {"breaches_floor", p.breaches_floor ? json(*p.breaches_floor) : json(nullptr)},
      {"breaches_soft_floor", p.breaches_soft_floor ? json(*p.breaches_soft_floor) : json(nullptr)},
      {"max_units", units(p.max_units)}, {"max_units_buying_power", units(p.max_units_buying_power)},
      {"max_units_floor", units(p.max_units_floor)}, {"breach", breach_json(p.breach)},
      {"execution", execution_json(p.execution)}, {"simulated", true}};
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
  // short_requirement keeps its historical name; requirement is the same amount
  // under a name that also fits portfolio margin, where it covers the whole book.
  return {{"available", power.available.str()}, {"reserved", power.reserved.str()},
          {"short_requirement", power.short_requirement.str()}, {"requirement", power.short_requirement.str()}};
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
  return {{"source", t->source == TriggerSource::Option ? "option" : t->source == TriggerSource::Combo ? "combo" : "underlying"},
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
  return {{"id", std::to_string(o.id)}, {"client_order_id", o.request.client_order_id}, {"actor", o.actor},
          {"symbol", multi ? json(nullptr) : json(o.request.symbol)},
          {"underlying", underlying(view, order_symbols(o.request).front())},
          {"side", multi ? json(nullptr) : json(side_name(o.request.side))}, {"legs", legs},
          {"type", o.request.type == OrderType::Limit ? "limit" : "market"},
          {"time_in_force", o.request.tif == TimeInForce::Day ? "day" : o.request.tif == TimeInForce::Gtc ? "gtc" : "ioc"},
          {"tags", o.request.tags}, {"note", o.request.note}, {"exits_only", o.request.exits_only},
          {"quantity", o.request.quantity}, {"filled_quantity", o.filled_quantity},
          {"remaining_quantity", o.remaining()}, {"limit_price", money(o.request.limit_price)},
          {"average_fill_price", o.filled_quantity > 0
              ? json(o.filled_notional.prorate(1, o.filled_quantity).str()) : json(nullptr)},
          {"status", statuses[static_cast<int>(o.status)]},
          {"reason", decision_json(o.reason)},
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
json context_json(const std::optional<FillContext>& context) {
  if (!context) return nullptr;
  const auto& c = *context;
  const auto optional_number = [](const std::optional<double>& n) { return n ? number(*n) : json(nullptr); };
  return {{"spot", optional_number(c.spot)}, {"spot_source", nullable(c.spot_source)},
          {"iv", optional_number(c.iv)}, {"delta", optional_number(c.delta)}, {"years", optional_number(c.years)},
          {"equity", money(c.equity)}, {"floor_room", money(c.floor_room)}, {"buying_power", money(c.buying_power)}};
}
json review_json(const TradeReview* r, std::optional<Money> net) {
  const auto extreme = [](const std::optional<Excursion>& e) -> json {
    if (!e) return nullptr;
    return {{"pnl", e->pnl.str()}, {"time", md::format_timestamp(e->time)}, {"spot", e->spot ? number(*e->spot) : json(nullptr)}};
  };
  const auto mae = r && r->worst ? std::optional(std::max(Money{}, -r->worst->pnl)) : std::nullopt;
  const auto mfe = r && r->best ? std::optional(std::max(Money{}, r->best->pnl)) : std::nullopt;
  const auto risk = r ? r->planned_risk : std::nullopt;
  return {{"mae", money(mae)}, {"mfe", money(mfe)},
          {"worst", r ? extreme(r->worst) : json(nullptr)}, {"best", r ? extreme(r->best) : json(nullptr)},
          {"planned_risk", money(risk)},
          {"give_back", mfe && net ? json(std::max(Money{}, *mfe - *net).str()) : json(nullptr)},
          {"heat", mae && risk && *risk > Money{} ? number(mae->dollars() / risk->dollars()) : json(nullptr)},
          {"r_multiple", net && risk && *risk > Money{} ? number(net->dollars() / risk->dollars()) : json(nullptr)}};
}
json day_notes_json(const TradingSnapshot& s) {
  json notes = json::object();
  for (const auto& [day, note] : s.day_notes)
    notes[day] = {{"plan", note.plan}, {"review", note.review}, {"time", md::format_timestamp(note.time)}};
  return notes;
}
/// The attempt an option fill belongs to: each attempt's fills start at its first_fill.
std::uint64_t fill_attempt(const TradingSnapshot& s, std::uint64_t fill) {
  if (fill >= s.evaluation.first_fill) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (fill >= it->first_fill) return it->attempt;
  return 1;
}
/// The trading date a trade closed in, holidays included; null while it is open.
json trading_day(const std::optional<Timestamp>& closed) {
  return closed ? json(md::format_date(md::trading_date(*closed))) : json(nullptr);
}
json fill_json(const Fill& f, const TradingView& view) {
  return {{"id", std::to_string(f.id)}, {"order_id", std::to_string(f.order_id)}, {"actor", f.actor},
          {"attempt", fill_attempt(*view.snapshot, f.id)},
          {"symbol", f.symbol}, {"underlying", underlying(view, f.symbol)},
          {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity},
          {"price", f.price.str()}, {"fee", f.fee.str()}, {"context", context_json(f.context)},
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
        {"expiry_time", md::format_timestamp(c.expiry_time())}, {"last_trade_time", md::format_timestamp(c.last_trade_time())},
        {"strike", c.strike}, {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"quantity", q}, {"average_price", average.str()}, {"basis", position.basis.str()},
        {"mark", money(p.mark)}, {"mark_age_seconds", p.mark ? json(static_cast<double>(p.mark_age) / md::kNanosPerSecond) : json(nullptr)},
        {"market_value", money(p.market_value)}, {"unrealised", money(p.unrealised)},
        {"realised", position.realised.str()}, {"fees", position.fees.str()}, {"fresh", p.fresh},
        {"awaiting_settlement", p.awaiting_settlement},
        // How an expired position settles: on the closing print, recorded or still awaited
        // (with the last print before the close as the fallback) for half an hour after the
        // close, or by a value entered by hand once no automatic source remains.
        {"settle_by", !p.awaiting_settlement ? json(nullptr)
            : c.settlement == md::Settlement::PM &&
              (s.closing_prints.contains(c.underlying + " " + md::format_date(c.expiry)) ||
               s.time < md::new_york_to_utc(c.expiry, md::regular_close_hour(c.expiry), 0) + kLastPrintWait)
                ? json("closing_print") : json("manual")},
        {"greeks", position_greeks(p, view)},
        {"attribution", s.attributions.contains(c.osi_symbol()) ? attribution_json(s.attributions.at(c.osi_symbol())) : json(nullptr)}});
  }
  json stocks = json::array();
  for (const auto& held : s.stocks) {
    const auto& p = held.position;
    const auto magnitude = p.shares < 0 ? -p.shares : p.shares;
    stocks.push_back({{"symbol", p.symbol}, {"shares", p.shares},
        {"average_price", (p.shares < 0 ? -p.basis : p.basis).prorate(1, magnitude).str()}, {"basis", p.basis.str()},
        {"mark", money(held.mark)}, {"mark_time", held.mark ? json(md::format_timestamp(held.mark_time)) : json(nullptr)},
        {"market_value", money(held.market_value)}, {"unrealised", money(held.unrealised)},
        {"realised", p.realised.str()}, {"fees", p.fees.str()}, {"fresh", held.fresh},
        {"attribution", s.attributions.contains(p.symbol) ? attribution_json(s.attributions.at(p.symbol)) : json(nullptr)}});
  }
  json flags = json::array();
  for (auto code : s.quality_flags) flags.push_back(to_string(code));
  return {{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
          {"cash", s.account.cash.str()}, {"equity", s.equity.str()},
          {"start_of_day_equity", s.start_of_day_equity.str()}, {"day_pnl", (s.equity - s.start_of_day_equity).str()},
          {"realised", s.account.realised.str()}, {"unrealised", s.unrealised.str()}, {"fees", s.account.fees.str()},
          {"valuation_complete", s.valuation_complete}, {"quality_flags", flags}, {"positions", positions}, {"stocks", stocks},
          {"buying_power", buying_power_json(s.buying_power)}, {"attribution", attribution_json(s.attribution)}};
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
                    {"realised", d.realised.str()}, {"qualifying", d.qualifying},
                    {"attribution", attribution_json(d.attribution)},
                    {"low_equity", money(d.low_equity)}, {"high_equity", money(d.high_equity)},
                    {"low_at", time_or_null(d.low_at)}, {"high_at", time_or_null(d.high_at)}});
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
          {"rules", rules_json(r)}, {"breach", breach_json(view.breach)}, {"warnings", warnings_json(view.warnings)},
          {"guardrails", guardrails_json(view.config.guardrails)}, {"guardrail_state", guardrail_state_json(s)},
          {"evaluation", {
              {"enabled", r.evaluation()}, {"attempt", e.attempt}, {"status", status_name(e.status)},
              {"started", md::format_timestamp(e.started)}, {"starting_balance", e.starting_balance.str()},
              {"equity", s.equity.str()}, {"marked", marked}, {"valuation_complete", s.valuation_complete},
              {"profit", (s.equity - e.starting_balance).str()}, {"peak", e.peak.str()},
              {"floor", floor ? json(e.floor.str()) : json(nullptr)},
              {"drawdown_buffer", floor ? json((s.equity - e.floor).str()) : json(nullptr)},
              {"target_equity", target ? json(target_equity.str()) : json(nullptr)},
              // Liquidating a pass at the bid can leave equity just under the target it reached.
              {"target_remaining", target ? json((e.status == EvaluationStatus::Passed ? Money{}
                                                 : std::max(Money{}, target_equity - s.equity)).str()) : json(nullptr)},
              {"decided_at", decided ? json(md::format_timestamp(e.decided_at)) : json(nullptr)},
              {"decided_equity", decided ? json(e.decided_equity.str()) : json(nullptr)},
              {"decision", nullable(e.decision)},
              {"day", md::format_date(e.day)}, {"day_open_equity", e.day_open_equity.str()},
              {"day_close_equity", e.day_close_equity.str()}, {"days", days},
              {"day_low_equity", money(e.day_low_equity)}, {"day_high_equity", money(e.day_high_equity)},
              {"day_low_at", time_or_null(e.day_low_at)}, {"day_high_at", time_or_null(e.day_high_at)},
              {"closest_floor", money(e.closest_floor)}, {"closest_floor_at", time_or_null(e.closest_floor_at)},
              {"floor_locked", floor && e.floor_locked}, {"qualifying_days", e.qualifying_days},
              {"cycle_started", md::format_timestamp(e.cycle_started)}, {"payouts", payouts}}},
          {"buying_power", buying_power_json(s.buying_power)},
          {"payout", payout_json(view)},
          {"attempts", attempts}};
}
/// How a change in shares came about.
json share_source(const StockFill& fill) {
  switch (fill.source) {
    case StockSource::Exercise: return "early_exercise";
    case StockSource::Delivery: {
      // Long calls and short puts buy at expiry; the short side is assigned.
      const auto option = md::parse_osi(fill.option);
      const bool call = option && option->type == pricing::OptionType::Call;
      return (call ? fill.shares < 0 : fill.shares > 0) ? "assignment" : "expiry_exercise";
    }
    case StockSource::Assignment: return "assignment";
    case StockSource::Trade: return "trade";
    case StockSource::Rule: return "rule";
    case StockSource::Reset: return "reset";
  }
  return nullptr;
}
json stock_fill_json(const StockFill& fill) {
  return {{"id", std::to_string(fill.id)}, {"symbol", fill.symbol}, {"shares", fill.shares},
          {"price", fill.price.str()}, {"time", md::format_timestamp(fill.time)}, {"source", share_source(fill)},
          {"option", nullable(fill.option)}};
}
const char* closure_name(ClosureKind kind) {
  return kind == ClosureKind::Settlement ? "settlement" : kind == ClosureKind::Exercise ? "exercise"
       : kind == ClosureKind::Assignment ? "assignment" : "reset";
}
/// What closed a round trip, and for a reducer liquidation why: the closure that ended
/// it, or its last reducing fill's order. A system order's client ID is system:<why>:<id>.
std::pair<json, json> closed_by(const TradingSnapshot& s, const Lifecycle& t) {
  if (!t.closed) return {nullptr, nullptr};
  if (t.closure) return {closure_name(*t.closure), nullptr};
  if (t.exit_order == 0 || t.exit_order > s.recent_orders.size()) return {nullptr, nullptr};
  const auto& order = s.recent_orders[t.exit_order - 1];
  const auto& client = order.request.client_order_id;
  if (order.system) {
    const auto first = client.find(':'), last = client.rfind(':');
    return {"system", first != std::string::npos && last > first ? json(client.substr(first + 1, last - first - 1)) : json(nullptr)};
  }
  if (order.role == OrderRole::StopLoss) return {"stop_loss", nullptr};
  if (order.role == OrderRole::TakeProfit) return {"take_profit", nullptr};
  if (client.starts_with("openport-close-")) return {"flatten", nullptr};
  if (client.starts_with("pb-close:")) return {"playbook", nullptr};
  return {"order", nullptr};
}
/// The replay run behind an account's trades, or null for a live account.
json run_json(const std::optional<RunIdentity>& run) {
  if (!run) return nullptr;
  return {{"id", nullable(run->id)}, {"scenario", nullable(run->scenario)}, {"seed", nullable(run->seed)},
          {"recording", nullable(run->recording)}, {"date", nullable(run->date)}};
}
json trades_json(const TradingView& view, std::string_view status, bool current_only) {
  const auto& s = *view.snapshot;
  const auto& e = s.evaluation;
  const auto all = lifecycles(s.recent_fills, s.closures, view.contracts);
  json trades = json::array();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = fill_attempt(s, t.first_fill);
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
    const auto a = s.annotations.find(std::to_string(t.first_fill));
    const auto exit = closed_by(s, t);
    const auto review = s.trade_reviews.find(std::to_string(t.first_fill));
    const auto order_id = s.recent_fills.at(t.first_fill - 1).order_id;
    const auto strategy = s.strategy_reviews.find(std::to_string(order_id));
    std::optional<Money> strategy_net;
    if (strategy != s.strategy_reviews.end() && strategy->second.finished) {
      strategy_net = Money{};
      for (const auto& leg : all)
        if (s.recent_fills.at(leg.first_fill - 1).order_id == order_id) *strategy_net = *strategy_net + leg.gross - leg.fees;
    }
    trades.push_back({{"entry_context", context_json(t.entry_context)}, {"exit_context", context_json(t.exit_context)},
        {"review", review_json(review == s.trade_reviews.end() ? nullptr : &review->second, open ? std::nullopt : std::optional(net))},
        {"strategy_id", strategy == s.strategy_reviews.end() ? json(nullptr) : json(std::to_string(order_id))},
        {"strategy_review", strategy == s.strategy_reviews.end() ? json(nullptr) : review_json(&strategy->second, strategy_net)},
        {"id", std::to_string(t.first_fill)}, {"attempt", attempt}, {"symbol", t.symbol},
        {"underlying", c.underlying}, {"expiry", md::format_date(c.expiry)},
        {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"}, {"strike", c.strike},
        {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"direction", t.direction > 0 ? "long" : "short"}, {"status", open ? "open" : "closed"},
        {"opened", md::format_timestamp(t.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(*t.closed))}, {"trading_day", trading_day(t.closed)},
        {"duration_seconds", open ? json(nullptr) : json((*t.closed - t.opened) / md::kNanosPerSecond)},
        {"quantity", t.quantity}, {"max_quantity", t.max_quantity},
        {"opened_contracts", t.opened_contracts}, {"closed_contracts", t.closed_contracts},
        {"average_open", average(t.open_notional, t.opened_contracts)},
        {"average_close", average(t.close_notional, t.closed_contracts)},
        {"cost", cost.str()}, {"gross", t.gross.str()}, {"fees", t.fees.str()}, {"net", net.str()},
        {"return", open || cost == Money{} ? json(nullptr) : number(net.dollars() / cost.dollars())},
        {"mark", mark}, {"unrealised", unrealised},
        {"closure", !t.closure ? json(nullptr) : json(closure_name(*t.closure))},
        {"closed_by", exit.first}, {"system_reason", exit.second},
        {"fills", fills},
        {"note", a == s.annotations.end() ? std::string{} : a->second.note},
        {"tags", a == s.annotations.end() ? json::array() : json(a->second.tags)}});
  }
  // Shares from exercise and assignment, round trip by round trip, newest first.
  auto attempt_at = [&](Timestamp time) {
    if (time >= e.started) return e.attempt;
    for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
      if (time >= it->started) return it->attempt;
    return std::uint64_t{1};
  };
  const auto source = [&](std::uint64_t id) -> json {
    if (id == 0 || id > s.stock_fills.size()) return nullptr;
    return share_source(s.stock_fills[id - 1]);
  };
  const auto option_of = [&](std::uint64_t id) -> json {
    if (id == 0 || id > s.stock_fills.size() || s.stock_fills[id - 1].option.empty()) return nullptr;
    return s.stock_fills[id - 1].option;
  };
  const auto shares = share_lifecycles(s.stock_fills, s.dividends);
  json share_trades = json::array();
  for (auto it = shares.rbegin(); it != shares.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = attempt_at(t.opened);
    if (current_only && attempt != e.attempt) continue;
    json mark = nullptr, unrealised = nullptr;
    if (open) {
      for (const auto& held : s.stocks) {
        if (held.position.symbol != t.symbol) continue;
        mark = money(held.mark);
        unrealised = money(held.unrealised);
      }
    }
    // Stock fill IDs carry the "s" of the share trade IDs, apart from option fills'.
    json fills = json::array();
    for (const auto id : t.fills) fills.push_back("s" + std::to_string(id));
    const auto first = t.fills.front();
    const auto last = open ? std::uint64_t{0} : t.fills.back();
    const auto note = s.annotations.find("s" + std::to_string(first));
    share_trades.push_back({{"kind", "shares"}, {"id", "s" + std::to_string(first)}, {"attempt", attempt},
        {"symbol", t.symbol}, {"direction", t.direction > 0 ? "long" : "short"}, {"status", open ? "open" : "closed"},
        {"opened", md::format_timestamp(t.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(*t.closed))}, {"trading_day", trading_day(t.closed)},
        {"duration_seconds", open ? json(nullptr) : json((*t.closed - t.opened) / md::kNanosPerSecond)},
        {"shares", t.shares}, {"max_shares", t.max_shares},
        {"opened_shares", t.opened_shares}, {"closed_shares", t.closed_shares},
        {"average_open", average(t.open_notional, t.opened_shares)},
        {"average_close", average(t.close_notional, t.closed_shares)},
        {"cost", t.open_notional.str()}, {"gross", t.gross.str()}, {"dividends", t.dividends.str()},
        {"fees", Money{}.str()}, {"net", (t.gross + t.dividends).str()},
        {"return", open || t.open_notional == Money{} ? json(nullptr) : number((t.gross + t.dividends).dollars() / t.open_notional.dollars())},
        {"mark", mark}, {"unrealised", unrealised},
        {"opened_by", source(first)}, {"option", option_of(first)},
        {"closed_by", source(last)}, {"closing_option", option_of(last)}, {"fills", fills},
        {"note", note == s.annotations.end() ? std::string{} : note->second.note},
        {"tags", note == s.annotations.end() ? json::array() : json(note->second.tags)}});
  }
  // Every change in shares and every dividend, oldest first, for the terminal's alerts.
  json stock_fills = json::array();
  for (const auto& fill : s.stock_fills) stock_fills.push_back(stock_fill_json(fill));
  json dividends = json::array();
  for (const auto& d : s.dividends)
    dividends.push_back({{"symbol", d.symbol}, {"ex_date", md::format_date(d.ex_date)}, {"per_share", d.per_share.str()},
        {"shares", d.shares}, {"amount", d.amount.str()}, {"time", md::format_timestamp(d.time)}});
  return {{"account_version", std::to_string(s.account_version)}, {"attempt", e.attempt}, {"trades", trades},
          {"share_trades", share_trades}, {"stock_fills", stock_fills}, {"dividends", dividends}, {"day_notes", day_notes_json(s)},
          {"run", run_json(view.run)}};
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
          {"limits", limits_json(view.config.limits)},
          {"pending_limits", s.pending_limits ? limits_json(*s.pending_limits) : json(nullptr)},
          {"guardrails", guardrails_json(view.config.guardrails)},
          {"pending_guardrails", s.pending_guardrails ? guardrails_json(*s.pending_guardrails) : json(nullptr)},
          {"guardrail_state", guardrail_state_json(s)}, {"pending_applied_at", time_or_null(s.pending_applied_at)},
          {"pending_applied_day", s.pending_applied_at > 0 ? json(md::format_date(md::trading_date(s.pending_applied_at))) : json(nullptr)},
          {"pending_effective", s.pending_limits || s.pending_guardrails ? json("next_trading_day") : json(nullptr)},
          {"time", md::format_timestamp(s.time)}, {"breach", breach_json(view.breach)}, {"warnings", warnings_json(view.warnings)},
          {"complete", s.risk.complete}, {"daily_loss", s.risk.daily_loss.str()},
          {"kill", kill_json(s.risk)}, {"aggregate", bucket_json(s.risk.aggregate)}, {"underlyings", underlyings},
          {"scenarios", {{"spot_percent", view.config.scenarios.spot_percent}, {"vol_points", view.config.scenarios.vol_points},
                         {"pnl", pnl}, {"clamped", clamped}, {"complete", s.scenarios.complete}}}};
}

int reason_status(Reason reason) {
  if (reason == Reason::UNKNOWN_ORDER || reason == Reason::UNKNOWN_CONTRACT || reason == Reason::UNKNOWN_TRADE) return 404;
  if (reason == Reason::ORDER_TERMINAL || reason == Reason::DUPLICATE_CLIENT_ID) return 409;
  if (reason == Reason::JOURNAL_IO || reason == Reason::JOURNAL_CORRUPT ||
      reason == Reason::JOURNAL_LOCKED) return 503;
  return 422;
}
ApiResponse command_response(const TradingCommand& command, const TradingReply& reply) {
  if (!reply.error_code.empty())
    return api_error(reply.error_code == "LIMITS_REVISION" || reply.error_code == "ACCOUNTS_UNSUPPORTED" ? 409
                     : reply.error_code == "UNKNOWN_ACCOUNT" ? 404 : 503,
                     reply.error_code, reply.decision.message);
  if (!reply.decision.ok()) return api_error(reason_status(reply.decision.code),
      std::string(to_string(reply.decision.code)), reply.decision.message, reply.decision);
  if (!reply.view || !reply.view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "No trading publication");
  const auto& view = *reply.view;
  const auto& s = *view.snapshot;
  json body{{"account_version", std::to_string(s.account_version)}};
  int status = 200;
  switch (command.kind) {
    case TradingCommand::Kind::Submit:
    case TradingCommand::Kind::Cancel:
    case TradingCommand::Kind::Modify: {
      const auto it = std::find_if(s.recent_orders.begin(), s.recent_orders.end(), [&](const auto& o) { return o.id == reply.order_id; });
      if (it == s.recent_orders.end()) return api_error(503, "TRADING_UNAVAILABLE", "Order publication missing");
      body["order"] = order_json(*it, view);
      if (command.kind != TradingCommand::Kind::Cancel) {
        if (command.kind == TradingCommand::Kind::Submit && !reply.replayed) status = 201;
        body["fills"] = json::array();
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == it->id) body["fills"].push_back(fill_json(fill, view));
      }
      break;
    }
    case TradingCommand::Kind::CancelAll:
    case TradingCommand::Kind::ClosePositions: {
      body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      if (command.kind == TradingCommand::Kind::CancelAll) break;
      // Each closing order with its outcome: a rejection carries its reason.
      body["orders"] = json::array();
      body["fills"] = json::array();
      for (auto id : reply.created_orders) {
        const auto& order = s.recent_orders.at(static_cast<std::size_t>(id - 1));
        body["orders"].push_back(order_json(order, view));
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == id) body["fills"].push_back(fill_json(fill, view));
      }
      // Delivered shares: those it closed, and those still held that it could not.
      body["stock_fills"] = json::array();
      for (auto id : reply.stock_fills) body["stock_fills"].push_back(stock_fill_json(s.stock_fills.at(static_cast<std::size_t>(id - 1))));
      body["kept_stocks"] = json::array();
      for (const auto& stock : s.stocks)
        if (const auto kept = reply.kept_stocks.find(stock.position.symbol); kept != reply.kept_stocks.end())
          body["kept_stocks"].push_back({{"symbol", stock.position.symbol}, {"shares", stock.position.shares},
              {"reason", decision_json(kept->second)}});
      break;
    }
    case TradingCommand::Kind::Playbook: body = json::parse(reply.playbook_result); break;
    case TradingCommand::Kind::Preview:
    case TradingCommand::Kind::PreviewChange:
      if (!reply.preview) return api_error(503, "TRADING_UNAVAILABLE", "No preview available");
      body = preview_json(*reply.preview);
      body["account_version"] = std::to_string(s.account_version);
      break;
    case TradingCommand::Kind::Guardrails:
    case TradingCommand::Kind::Limits: body = risk_json(view); break;
    case TradingCommand::Kind::Trip:
    case TradingCommand::Kind::Reset:
      body["kill"] = kill_json(s.risk); body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      break;
    case TradingCommand::Kind::Settle: body["position_closed"] = true; break;
    case TradingCommand::Kind::ResetAccount:
    case TradingCommand::Kind::Payout: body = account_json(view); break;
    case TradingCommand::Kind::Exercise:
    case TradingCommand::Kind::CloseStock: body = portfolio_json(view); break;
    case TradingCommand::Kind::DayNote: {
      const auto key = md::format_date(command.day);
      body["day"] = key;
      body["note"] = day_notes_json(s).value(key, json{{"plan", ""}, {"review", ""}, {"time", nullptr}});
      break;
    }
    case TradingCommand::Kind::Annotate: {
      const auto key = (command.shares ? "s" : "") + std::to_string(command.trade);
      const auto a = s.annotations.find(key);
      body["trade"] = key;
      body["note"] = a == s.annotations.end() ? std::string{} : a->second.note;
      body["tags"] = a == s.annotations.end() ? json::array() : json(a->second.tags);
      break;
    }
    case TradingCommand::Kind::CreateSandbox:
    case TradingCommand::Kind::CreateAccount:
      status = 201;
      body = {{"account", {{"id", reply.account}, {"name", command.name},
                           {"account_version", std::to_string(s.account_version)},
                           {"plan", nullable(view.config.rules.plan)}, {"equity", s.equity.str()}}}};
      break;
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
  if ((source != "option" && source != "underlying" && source != "combo") || (direction != "at_or_below" && direction != "at_or_above"))
    throw std::invalid_argument("trigger source must be option, combo or underlying, direction at_or_below or at_or_above");
  return {source == "option" ? TriggerSource::Option : source == "combo" ? TriggerSource::Combo : TriggerSource::Underlying,
          direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove, decimal_field(j, "level")};
}
ExitSpec parse_exit(const json& j) {
  fields(j, {}, {"trigger", "limit_price"});
  ExitSpec exit;
  if (j.contains("trigger")) exit.trigger = parse_trigger(j.at("trigger"));
  if (j.contains("limit_price")) exit.limit_price = decimal_field(j, "limit_price");
  if (!exit.trigger && !exit.limit_price)
    throw std::invalid_argument("A bracket exit takes a trigger, a limit_price, or both for a stop-limit");
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
         {"plan", "phase", "lock_balance", "payouts", "defined_risk", "slippage_ticks", "margin", "fill_latency_ms", "impact_ticks"});
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
  if (j.contains("defined_risk")) rules.defined_risk = boolean_field(j, "defined_risk");
  if (j.contains("slippage_ticks")) rules.slippage_ticks = integer_field(j, "slippage_ticks");
  if (j.contains("fill_latency_ms")) rules.fill_latency_ms = integer_field(j, "fill_latency_ms");
  if (j.contains("impact_ticks")) rules.impact_ticks = integer_field(j, "impact_ticks");
  if (j.contains("margin")) {
    const auto margin = string_field(j, "margin");
    if (margin != "strategy" && margin != "portfolio") throw std::invalid_argument("margin must be strategy or portfolio");
    rules.margin = margin == "portfolio" ? MarginMode::Portfolio : MarginMode::Strategy;
  }
  rules.buying_power = boolean_field(j, "buying_power");
  // Out of range is a rule error (INVALID_RULES from validate_rules), not a malformed
  // request; saturate first so the conversion to nanoseconds cannot overflow.
  rules.expiry_cutoff = std::clamp<std::int64_t>(integer_field(j, "expiry_cutoff_seconds"), -1, 86'400) * md::kNanosPerSecond;
  validate_rules(rules);
  return rules;
}
/// A preset's name belongs to its own balance and rules, so an attempt recorded under
/// it (and the funded plan it unlocks) is that preset's; the fill model may differ.
void check_plan_name(Money initial_cash, const AccountRules& rules) {
  if (const auto* preset = find_plan_named(rules.plan); preset && !follows_plan(*preset, initial_cash, rules))
    throw TradingError(Reason::INVALID_RULES, "\"" + rules.plan + "\" is a preset's name: reset with plan " + preset->id +
                       ", or give these rules another name");
}
/// Presets override only execution settings; all evaluation rules stay intact.
void fill_model(const json& body, AccountRules& rules) {
  if (!body.contains("fill_model")) return;
  const auto model = string_field(body, "fill_model");
  if (model != "as_displayed" && model != "conservative")
    throw std::invalid_argument("fill_model must be as_displayed or conservative");
  rules.fill_latency_ms = model == "conservative" ? 1000 : 0;
  rules.impact_ticks = model == "conservative" ? 1 : 0;
  rules.slippage_ticks = model == "conservative" ? 1 : 0;
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
/// The optional scope of a bulk command: one underlying, or every one.
std::string scope_field(const json& body) {
  fields(body, {}, {"underlying"});
  if (!body.contains("underlying")) return {};
  auto underlying = string_field(body, "underlying");
  if (underlying.empty() || underlying.size() > 16 ||
      !std::all_of(underlying.begin(), underlying.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
    throw std::invalid_argument("underlying must be an uppercase symbol such as SPX");
  return underlying;
}
/// Client order IDs: 1 to 128 bytes of text, none of them a control character.
bool valid_client_order_id(std::string_view id) {
  return !id.empty() && id.size() <= 128 &&
         std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
/// Account IDs are lowercase letters, digits and single hyphens, as their journals are named.
bool valid_account(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' && id.find("--") == std::string_view::npos &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
TradingCommand parse_command(const ApiRequest& request, std::string_view path) {
  TradingCommand command;
  if (request.method == "DELETE") {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::Cancel;
    command.order_id = identifier(path.substr(std::string_view("/api/orders/").size()));
    return command;
  }
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = strict_json(request.body);
  // POST /api/orders/{id}/preview: a change's terms, as PUT takes them, and floor_share.
  const bool change_preview = request.method == "POST" && path != "/api/orders/preview" &&
      path.starts_with("/api/orders/") && path.ends_with("/preview");
  if ((request.method == "PUT" && path.starts_with("/api/orders/")) || change_preview) {
    if (change_preview) fields(body, {}, {"quantity", "limit_price", "trigger_level", "floor_share"});
    else fields(body, {}, {"quantity", "limit_price", "trigger_level"});
    command.kind = change_preview ? TradingCommand::Kind::PreviewChange : TradingCommand::Kind::Modify;
    auto id = path.substr(std::string_view("/api/orders/").size());
    if (change_preview) id = id.substr(0, id.size() - std::string_view("/preview").size());
    command.order_id = identifier(id);
    if (change_preview && body.contains("floor_share")) {
      command.floor_share = number_field(body, "floor_share");
      if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
    }
    if (body.contains("quantity")) command.change.quantity = integer_field(body, "quantity");
    if (body.contains("limit_price")) command.change.limit_price = decimal_field(body, "limit_price");
    if (body.contains("trigger_level")) command.change.trigger_level = decimal_field(body, "trigger_level");
    if (command.change.empty()) throw std::invalid_argument("Give quantity, limit_price or trigger_level");
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/days/") && path.ends_with("/note")) {
    fields(body, {"plan", "review"});
    const auto date = csv_date(path.substr(10, path.size() - 15));
    if (!date) throw std::invalid_argument("Expected a valid YYYY-MM-DD date");
    command.kind = TradingCommand::Kind::DayNote;
    command.day = *date;
    command.plan = string_field(body, "plan");
    command.review = string_field(body, "review");
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/trades/")) {
    // PUT /api/trades/{id}/note: the note and tags replace the trade's.
    fields(body, {}, {"note", "tags"});
    command.kind = TradingCommand::Kind::Annotate;
    auto id = path.substr(std::string_view("/api/trades/").size());
    id = id.substr(0, id.size() - std::string_view("/note").size());
    // Share round trips are named "s" and their opening stock fill.
    command.shares = id.starts_with("s");
    command.trade = identifier(command.shares ? id.substr(1) : id);
    if (body.contains("note")) command.note = string_field(body, "note");
    if (body.contains("tags")) {
      const auto& tags = body.at("tags");
      if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
      for (const auto& tag : tags) {
        if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
        command.tags.push_back(tag.get<std::string>());
      }
    }
    return command;
  }
  if (path == "/api/positions/exercise") {
    fields(body, {"symbol", "quantity"});
    command.kind = TradingCommand::Kind::Exercise;
    command.symbol = symbol_field(body);
    command.quantity = integer_field(body, "quantity");
    if (command.quantity <= 0) throw std::invalid_argument("quantity must be a positive number of contracts");
    return command;
  }
  if (path == "/api/stocks/close") {
    fields(body, {"symbol"}, {"shares"});
    command.kind = TradingCommand::Kind::CloseStock;
    command.symbol = string_field(body, "symbol");
    if (command.symbol.empty() || command.symbol.size() > 16 ||
        !std::all_of(command.symbol.begin(), command.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument("symbol must be an uppercase stock symbol such as SPY");
    if (body.contains("shares")) {
      command.quantity = integer_field(body, "shares");
      if (command.quantity <= 0) throw std::invalid_argument("shares must be positive; leave it out to close them all");
    }
    return command;
  }
  if (path == "/api/orders/cancel") {
    command.kind = TradingCommand::Kind::CancelAll;
    command.underlying = scope_field(body);
    return command;
  }
  if (path == "/api/positions/close") {
    command.kind = TradingCommand::Kind::ClosePositions;
    command.underlying = scope_field(body);
    return command;
  }
  if (path == "/api/accounts") {
    // A name, and either a preset plan or a starting balance and complete rules.
    fields(body, {"name"}, {"plan", "initial_cash", "rules", "fill_model"});
    command.kind = TradingCommand::Kind::CreateAccount;
    command.name = string_field(body, "name");
    if (!valid_account_name(command.name))
      throw std::invalid_argument("name must be 1 to 64 characters, none of them a control character");
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules")) throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      if (!plan->unlocked_by.empty())
        throw std::invalid_argument("A funded plan starts from an account that passed its evaluation; reset that account instead");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
      if (command.rules.phase == Phase::Funded)
        throw std::invalid_argument("A funded account starts from an account that passed its evaluation");
      check_plan_name(command.initial_cash, command.rules);
    }
    fill_model(body, command.rules);
    return command;
  }
  if (path == "/api/orders" || path == "/api/orders/preview") {
    if (path == "/api/orders/preview") {
      command.kind = TradingCommand::Kind::Preview;
      if (body.contains("floor_share")) {
        command.floor_share = number_field(body, "floor_share");
        if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
        body.erase("floor_share");
      }
    }
    // A single contract (symbol and side), or legs for a multi-leg order.
    const bool legs = body.is_object() && body.contains("legs");
    if (legs) fields(body, {"client_order_id", "legs", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "exits_only"});
    else fields(body, {"client_order_id", "symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note"});
    // A body no market could make a valid order is malformed: 400, and nothing is
    // recorded, so its client_order_id stays free. The reducer's own checks (422,
    // recorded) are those that depend on the account and the market.
    auto& order = command.order;
    order.client_order_id = string_field(body, "client_order_id");
    if (!valid_client_order_id(order.client_order_id))
      throw std::invalid_argument("client_order_id must be 1 to 128 bytes of text without control characters");
    const auto type = string_field(body, "type"), tif = string_field(body, "time_in_force");
    if ((type != "limit" && type != "market") || (tif != "day" && tif != "ioc" && tif != "gtc"))
      throw std::invalid_argument("Invalid type or time_in_force");
    order.type = type == "limit" ? OrderType::Limit : OrderType::Market;
    order.tif = tif == "day" ? TimeInForce::Day : tif == "gtc" ? TimeInForce::Gtc : TimeInForce::Ioc;
    if (order.type == OrderType::Market && order.tif != TimeInForce::Ioc)
      throw std::invalid_argument("A market order's time_in_force must be ioc");
    order.quantity = integer_field(body, "quantity");
    if (order.quantity < 1) throw std::invalid_argument("quantity must be a positive whole number of contracts or units");
    if ((order.type == OrderType::Limit) != body.contains("limit_price"))
      throw std::invalid_argument("limit_price is required for limit orders and forbidden for market orders");
    if (body.contains("limit_price")) order.limit_price = decimal_field(body, "limit_price");
    if (legs) {
      const auto& list = body.at("legs");
      if (!list.is_array() || list.size() < 2 || list.size() > kMaxLegs)
        throw std::invalid_argument("legs must be an array of two to four legs");
      std::set<std::string> symbols;
      for (const auto& item : list) {
        fields(item, {"symbol", "side"}, {"ratio"});
        Leg leg;
        leg.symbol = symbol_field(item);
        const auto side = string_field(item, "side");
        if (side != "buy" && side != "sell") throw std::invalid_argument("Leg side must be buy or sell");
        leg.side = side == "buy" ? Side::Buy : Side::Sell;
        if (item.contains("ratio")) leg.ratio = integer_field(item, "ratio");
        if (leg.ratio < 1 || leg.ratio > kMaxRatio) throw std::invalid_argument("A leg's ratio must be 1 to 10");
        if (!symbols.insert(leg.symbol).second) throw std::invalid_argument("Each leg must name a different contract");
        order.legs.push_back(std::move(leg));
      }
    } else {
      if (order.limit_price && *order.limit_price <= Money{})
        throw std::invalid_argument("A single contract's limit_price must be positive");
      order.symbol = symbol_field(body);
      const auto side = string_field(body, "side");
      if (side != "buy" && side != "sell") throw std::invalid_argument("Invalid side, type or time_in_force");
      order.side = side == "buy" ? Side::Buy : Side::Sell;
    }
    if (body.contains("exits_only")) order.exits_only = boolean_field(body, "exits_only");
    if (body.contains("note")) order.note = string_field(body, "note");
    if (body.contains("tags")) {
      const auto& tags = body.at("tags");
      if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
      for (const auto& tag : tags) {
        if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
        order.tags.push_back(tag.get<std::string>());
      }
    }
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
  } else if (path == "/api/risk/guardrails") {
    fields(body, {"expected_revision", "guardrails"});
    command.kind = TradingCommand::Kind::Guardrails;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    const auto& g = body.at("guardrails");
    fields(g, {"soft_floor", "soft_floor_percent", "max_opening_trades", "cooldown_loss", "cooldown_minutes", "profit_lock"});
    command.guardrails = {decimal_field(g, "soft_floor"), integer_field(g, "soft_floor_percent"), integer_field(g, "max_opening_trades"),
        decimal_field(g, "cooldown_loss"), integer_field(g, "cooldown_minutes"), decimal_field(g, "profit_lock")};
    validate_guardrails(command.guardrails);
  } else if (path == "/api/risk/limits") {
    fields(body, {"expected_revision", "limits"});
    command.kind = TradingCommand::Kind::Limits;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    command.limits = parse_limits(body.at("limits"));
  } else if (path == "/api/risk/kill") {
    fields(body, {"action", "reason"});
    const auto action = string_field(body, "action");
    if (action != "trip" && action != "reset") throw std::invalid_argument("action must be trip or reset");
    command.kind = action == "trip" ? TradingCommand::Kind::Trip : TradingCommand::Kind::Reset;
    command.reason = string_field(body, "reason");
  } else if (path == "/api/account/reset") {
    // Either a preset ID, or a custom starting balance and complete rules.
    fields(body, {"reason"}, {"plan", "initial_cash", "rules", "fill_model"});
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
      check_plan_name(command.initial_cash, command.rules);
    }
    fill_model(body, command.rules);
  } else if (path == "/api/account/payout") {
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

/// P&L by Greek in dollars, to the cent. The parts add up to the total exactly:
/// each is its value rounded down or up, the cents rounding down leaves go to the
/// parts it cut most (largest remainder, the earlier part on a tie).
json attribution_json(const trading::Attribution& a) {
  const std::array<double, 6> parts{a.delta, a.gamma, a.vega, a.theta, a.other, a.costs};
  std::array<double, 6> dollars{};
  const double total = std::round(a.total() * 100);
  if (std::all_of(parts.begin(), parts.end(), [](double x) { return std::isfinite(x); }) && std::isfinite(total)) {
    std::array<double, 6> remainders{};
    double left = total;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      dollars[i] = std::floor(parts[i] * 100);
      remainders[i] = parts[i] * 100 - dollars[i];
      left -= dollars[i];
    }
    std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return remainders[x] > remainders[y]; });
    for (std::size_t k = 0; left >= 1; ++k, --left) ++dollars[order[k % order.size()]];
    for (std::size_t k = 0; left <= -1; ++k, ++left) --dollars[order[order.size() - 1 - k % order.size()]];
    for (auto& part : dollars) part /= 100;
  } else {
    for (std::size_t i = 0; i < parts.size(); ++i) dollars[i] = std::round(parts[i] * 100) / 100;
  }
  return {{"delta", number(dollars[0])}, {"gamma", number(dollars[1])}, {"vega", number(dollars[2])},
          {"theta", number(dollars[3])}, {"other", number(dollars[4])}, {"costs", number(dollars[5])},
          {"total", number(total / 100)}};
}

ApiResponse api_error(int status, std::string code, std::string message, const Decision& evidence) {
  // Some reducer checks identify a single contract. The HTTP contract exposes
  // risk scope as the underlying, while the order itself carries the OSI.
  return {status, json{{"error", {{"code", code}, {"message", message},
      {"actual", evidence.actual ? number(*evidence.actual) : json(nullptr)},
      {"limit", evidence.limit ? number(*evidence.limit) : json(nullptr)},
      {"scope", scope_json(evidence.scope)}}}}.dump()};
}
json trading_status_json(const TradingStatus& status) {
  return {{"enabled", status.enabled}, {"reason", nullable(status.reason)},
          {"account_version", std::to_string(status.account_version)},
          {"kill_latched", status.kill_latched}, {"write", status.write},
          {"fee_per_contract", status.fee_per_contract.str()},
          {"initial_cash", status.initial_cash.str()},
          {"plan", nullable(status.plan)}, {"evaluation", nullable(status.evaluation)}};
}
json accounts_json(const MetricsSource& source, const ApiAccess& access) {
  json list = json::array();
  for (const auto& account : source.status().accounts) {
    if (!sandbox_visible(account, access)) continue;
    const auto view = source.trading_view(account.id);
    list.push_back({{"id", account.id}, {"name", account.name}, {"trading", trading_status_json(account.trading)},
                    {"equity", view && view->snapshot ? json(view->snapshot->equity.str()) : json(nullptr)}});
    if (account.sandbox_idle_seconds) list.back()["sandbox_idle_seconds"] = account.sandbox_idle_seconds;
  }
  return list;
}
json account_ticks_json(const EngineStatus& status) {
  json list = json::array();
  for (const auto& account : status.accounts) {
    list.push_back({{"id", account.id}, {"name", account.name}, {"trading", trading_status_json(account.trading)}});
    if (account.sandbox_idle_seconds) list.back()["sandbox_idle_seconds"] = account.sandbox_idle_seconds;
  }
  return list;
}

std::optional<std::map<std::string, std::string>> query_parameters(std::string_view query, bool plus_is_space) {
  const auto decode = [plus_is_space](std::string_view text) -> std::optional<std::string> {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '%') { decoded += plus_is_space && text[i] == '+' ? ' ' : text[i]; continue; }
      if (i + 2 >= text.size()) return std::nullopt;
      unsigned int byte = 0;
      const auto* digits = text.data() + i + 1;
      const auto [end, error] = std::from_chars(digits, digits + 2, byte, 16);
      if (error != std::errc{} || end != digits + 2) return std::nullopt;
      decoded += static_cast<char>(byte);
      i += 2;
    }
    return decoded;
  };
  std::map<std::string, std::string> pairs;
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto pair = query.substr(0, amp);
    const auto equals = pair.find('=');
    auto key = decode(pair.substr(0, equals));
    auto value = decode(equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1));
    if (!key || !value || !pairs.emplace(std::move(*key), std::move(*value)).second) return std::nullopt;
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return pairs;
}

std::optional<std::string> query_account(std::string_view target) {
  const auto question = target.find('?');
  const auto pairs = query_parameters(question == std::string_view::npos ? std::string_view{} : target.substr(question + 1));
  if (!pairs) return std::nullopt;
  const auto account = pairs->find("account");
  if (account == pairs->end()) return std::string(kMainAccount);
  if (!valid_account(account->second)) return std::nullopt;
  return account->second;
}

/// Whether `account` names an account the source publishes (the main one when empty).
bool known_account(const MetricsSource& source, const std::string& account) {
  if (account.empty() || account == kMainAccount) return true;
  const auto accounts = source.status().accounts;
  return std::any_of(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
}

std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source) {
  const auto question = request.target.find('?');
  const auto path = request.target.substr(0, question);
  if (path != "/api/portfolio" && path != "/api/orders" && path != "/api/fills" && path != "/api/risk" &&
      path != "/api/trades.csv" && path != "/api/fills.csv" && path != "/api/account" && path != "/api/account/equity" &&
      path != "/api/trades" && path != "/api/plans" && path != "/api/accounts") return {};
  const auto query = question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1);
  // Every route takes account=ID; orders take status=open|all; trades take
  // status=open|closed|all and attempt=current|all. Each key at most once.
  const auto pairs = query_parameters(query);
  const bool csv = path == "/api/trades.csv" || path == "/api/fills.csv";
  // CSV exports filter by New York date; the equity history by instant.
  std::string account, status = "all", attempt = csv ? "all" : "current", from, to;
  std::optional<Timestamp> since, until;
  bool valid_query = pairs.has_value();
  for (const auto& [key, value] : pairs.value_or(std::map<std::string, std::string>{})) {
    if ((key == "from" || key == "to") && path == "/api/account/equity") {
      const auto parsed = md::parse_datetime(value, md::Zone::Utc);
      if (!parsed || *parsed < 0) valid_query = false;
      else if (key == "from") since = parsed; else until = parsed;
    } else if (key == "account" && path != "/api/accounts" && valid_account(value)) account = value;
    else if (key == "status" && path == "/api/orders" && (value == "open" || value == "all")) status = value;
    else if (key == "status" && (path == "/api/trades" || path == "/api/trades.csv") && (value == "open" || value == "closed" || value == "all")) status = value;
    else if (key == "attempt" && (path == "/api/trades" || path == "/api/trades.csv") && (value == "current" || value == "all")) attempt = value;
    else if (csv && key == "from" && csv_date(value)) from = value;
    else if (csv && key == "to" && csv_date(value)) to = value;
    else valid_query = false;
  }
  if (!from.empty() && !to.empty() && from > to) valid_query = false;
  if (since && until && *since > *until) valid_query = false;
  if (!valid_query) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  if (path == "/api/plans") return ApiResponse{200, plans_json().dump()};
  if (path == "/api/accounts") return ApiResponse{200, json{{"accounts", accounts_json(source, request.access)}}.dump()};
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  if (path == "/api/account/equity") {
    json samples = json::array();
    for (const auto& sample : view->equity_samples) {
      if ((since && sample.time < *since) || (until && sample.time > *until)) continue;
      samples.push_back({{"time", md::format_timestamp(sample.time)}, {"day", md::format_date(md::trading_date(sample.time))},
          {"attempt", sample.attempt}, {"equity", sample.equity.str()}, {"floor", money(sample.floor)}, {"peak", sample.peak.str()},
          {"target", money(sample.target)}, {"tomorrow_floor", money(sample.tomorrow_floor)},
          {"fill", sample.stock_fill ? json("s" + std::to_string(sample.stock_fill)) : sample.fill ? json(std::to_string(sample.fill)) : json(nullptr)}});
    }
    return ApiResponse{200, json{{"samples", samples}, {"error", nullable(view->equity_error)}}.dump()};
  }
  if (path == "/api/portfolio") return ApiResponse{200, portfolio_json(*view).dump()};
  if (path == "/api/risk") return ApiResponse{200, risk_json(*view).dump()};
  if (path == "/api/account") return ApiResponse{200, account_json(*view).dump()};
  if (path == "/api/trades") return ApiResponse{200, trades_json(*view, status, attempt == "current").dump()};
  if (csv) {
    json rows = json::array();
    const bool fills = path == "/api/fills.csv";
    if (fills) {
      for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) rows.push_back(fill_json(*it, *view));
    } else {
      const auto trades = trades_json(*view, status, attempt == "current");
      for (auto row : trades.at("trades")) { row["kind"] = "option"; rows.push_back(std::move(row)); }
      for (const auto& row : trades.at("share_trades")) rows.push_back(row);
    }
    return ApiResponse{200, paper_csv(rows, fills, account.empty() ? std::string(kMainAccount) : account,
                                    source.status().provider, s.account_version, from, to, view->run),
                       "text/csv; charset=utf-8", fills ? "fills.csv" : "trades.csv"};
  }
  json body{{"account_version", std::to_string(s.account_version)}};
  if (path == "/api/orders") {
    body["orders"] = json::array();
    for (auto it = s.recent_orders.rbegin(); it != s.recent_orders.rend(); ++it)
      if (status != "open" || it->open()) body["orders"].push_back(order_json(*it, *view));
  } else {
    body["fills"] = json::array();
    for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) body["fills"].push_back(fill_json(*it, *view));
  }
  return ApiResponse{200, body.dump()};
}

void handle_api_async(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  if (request.method == "POST" && request.target.substr(0, request.target.find('?')) == "/api/sandboxes") {
    if (source.sandboxes()) source.sandboxes()->create(request, source, std::move(complete));
    else complete(api_error(404, "NOT_FOUND", "Sandboxes are not offered"));
    return;
  }
  if (auto rejection = sandbox_visibility(request, source)) { complete(std::move(*rejection)); return; }
  if (request.method == "GET") { complete(handle_api(request, source)); return; }
  if (request.target.starts_with("/api/notifications/")) {
    if (!source.notifications()) { complete(api_error(503, "NOTIFICATIONS_UNAVAILABLE", "Notifications are unavailable on this source")); return; }
    try {
      const auto body = json::parse(request.body);
      if (request.method == "POST" && request.target == "/api/notifications/test") {
        if (!body.is_object() || body.size() != 1 || !body.at("channel").is_string()) throw std::invalid_argument("request");
        const int status = source.notifications()->test(body.at("channel").get<std::string>());
        if (status == 202) complete({202, json{{"queued", true}}.dump()});
        else complete(api_error(status, "NOTIFICATION_TEST_FAILED", "Channel is unknown, disabled, stopped or its queue is full"));
      } else if (request.method == "PUT" && request.target.starts_with("/api/notifications/channels/") && request.target.find('?') == std::string::npos) {
        if (!source.notifications()->configure(request.target.substr(28), body)) complete(api_error(404, "NOT_FOUND", "Unknown notification channel"));
        else complete({200, source.notifications()->status().dump()});
      } else complete(api_error(404, "NOT_FOUND", "Unknown notification endpoint or method"));
    } catch (...) { complete(api_error(400, "INVALID_REQUEST", "Invalid notification settings or test request")); }
    return;
  }
  if (playbook_write(request, source, complete)) return;
  // Writes take one query parameter, account=ID; the route is the path alone.
  const auto question = request.target.find('?');
  const std::string path = request.target.substr(0, question);
  const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  const bool route = (request.method == "POST" && (path == "/api/orders" || path == "/api/orders/preview" ||
      (path.starts_with("/api/orders/") && path.ends_with("/preview")) ||
      path == "/api/orders/cancel" || path == "/api/positions/close" || path == "/api/accounts" ||
      path == "/api/positions/exercise" || path == "/api/stocks/close" ||
      path == "/api/risk/kill" || path == "/api/settlements" ||
      path == "/api/account/reset" || path == "/api/account/payout")) ||
      (request.method == "PUT" && (path == "/api/risk/limits" || path == "/api/risk/guardrails" || path.starts_with("/api/orders/") ||
                                   ((path.starts_with("/api/trades/") || path.starts_with("/api/days/")) && path.ends_with("/note")))) ||
      (request.method == "DELETE" && path.starts_with("/api/orders/"));
  if (!route) { complete(api_error(404, "NOT_FOUND", "Unknown endpoint or method")); return; }
  std::string account;
  if (!pairs || pairs->size() > 1 || (pairs->size() == 1 && (!pairs->contains("account") || !valid_account(pairs->at("account"))) ) ||
      (pairs->size() == 1 && path == "/api/accounts")) {
    complete(api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter"));
    return;
  }
  if (pairs->size() == 1) account = pairs->at("account");
  // An account whose journal another process holds cannot take writes.
  const auto status = source.status();
  for (const auto& a : status.accounts)
    if ((a.id == (account.empty() ? std::string(kMainAccount) : account)) && a.trading.reason.starts_with("JOURNAL_LOCKED:")) {
      complete(api_error(503, "TRADING_UNAVAILABLE", a.trading.reason));
      return;
    }
  if (status.accounts.empty() && status.trading.reason.starts_with("JOURNAL_LOCKED:")) {
    complete(api_error(503, "TRADING_UNAVAILABLE", status.trading.reason));
    return;
  }
  try {
    auto command = parse_command(request, path);
    command.account = account;
    command.actor = request.actor;
    if (!source.post_trading(command, [command, complete](TradingReply reply) {
          complete(command_response(command, reply));
        })) {
      // Safe to retry: an order resent with the same client_order_id and terms is answered once.
      auto busy = api_error(503, "TRADING_UNAVAILABLE", "Command inbox full or trading unavailable");
      busy.retry_after = kInboxRetrySeconds;
      complete(std::move(busy));
    }
  } catch (const TradingError& error) {
    complete(api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what()));
  } catch (const std::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  }
}
}  // namespace openport::server
