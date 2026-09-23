#include "paper_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace openport::server {
namespace {
using nlohmann::json;
using namespace trading;

json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
json nullable(const std::string& value) { return value.empty() ? json(nullptr) : json(value); }
json money(const std::optional<Money>& value) { return value ? json(value->str()) : json(nullptr); }
std::string underlying(const TradingView& view, const std::string& symbol) {
  const auto it = view.contracts.find(symbol);
  if (it != view.contracts.end()) return it->second.underlying;
  const auto parsed = md::parse_osi(symbol);
  return parsed ? parsed->underlying : "";
}
json order_json(const Order& o, const TradingView& view) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected"};
  return {{"id", std::to_string(o.id)}, {"client_order_id", o.request.client_order_id},
          {"symbol", o.request.symbol}, {"underlying", underlying(view, o.request.symbol)},
          {"side", o.request.side == Side::Buy ? "buy" : "sell"},
          {"type", o.request.type == OrderType::Limit ? "limit" : "market"},
          {"time_in_force", o.request.tif == TimeInForce::Day ? "day" : "ioc"},
          {"quantity", o.request.quantity}, {"filled_quantity", o.filled_quantity},
          {"remaining_quantity", o.remaining()}, {"limit_price", money(o.request.limit_price)},
          {"average_fill_price", o.filled_quantity > 0
              ? json(o.filled_notional.prorate(1, o.filled_quantity).str()) : json(nullptr)},
          {"status", statuses[static_cast<int>(o.status)]},
          {"reason", o.reason.ok() ? json(nullptr) : json{{"code", to_string(o.reason.code)}, {"message", o.reason.message}}},
          {"accepted_at", md::format_timestamp(o.accepted_at)},
          {"day_end", o.day_end > 0 ? json(md::format_timestamp(o.day_end)) : json(nullptr)}};
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
          {"valuation_complete", s.valuation_complete}, {"quality_flags", flags}, {"positions", positions}};
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
    fields(body, {"client_order_id", "symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price"});
    auto& order = command.order;
    order.client_order_id = string_field(body, "client_order_id");
    order.symbol = symbol_field(body);
    const auto side = string_field(body, "side"), type = string_field(body, "type"), tif = string_field(body, "time_in_force");
    if ((side != "buy" && side != "sell") || (type != "limit" && type != "market") || (tif != "day" && tif != "ioc"))
      throw std::invalid_argument("Invalid side, type or time_in_force");
    order.side = side == "buy" ? Side::Buy : Side::Sell;
    order.type = type == "limit" ? OrderType::Limit : OrderType::Market;
    order.tif = tif == "day" ? TimeInForce::Day : TimeInForce::Ioc;
    order.quantity = integer_field(body, "quantity");
    if ((order.type == OrderType::Limit) != body.contains("limit_price"))
      throw std::invalid_argument("limit_price is required for limit orders and forbidden for market orders");
    if (body.contains("limit_price")) order.limit_price = decimal_field(body, "limit_price");
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
          {"initial_cash", status.initial_cash.str()}};
}
std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source) {
  const auto question = request.target.find('?');
  const auto path = request.target.substr(0, question);
  if (path != "/api/portfolio" && path != "/api/orders" && path != "/api/fills" && path != "/api/risk") return {};
  const auto query = question == std::string::npos ? "" : request.target.substr(question + 1);
  if (!query.empty() && (path != "/api/orders" || (query != "status=open" && query != "status=all")))
    return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  const auto view = source.trading_view();
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  if (path == "/api/portfolio") return ApiResponse{200, portfolio_json(*view).dump()};
  if (path == "/api/risk") return ApiResponse{200, risk_json(*view).dump()};
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
      request.target == "/api/risk/kill" || request.target == "/api/settlements")) ||
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
