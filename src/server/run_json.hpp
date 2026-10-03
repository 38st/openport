#pragma once

#include "../trading/state.hpp"
#include "openport/server/desk.hpp"

namespace openport::md {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScheduledDay, date, name, closed, close_hour, overnight_until)
}
namespace openport::trading {
inline void to_json(nlohmann::json& j, const OrderChange& c) {
  j = nlohmann::json{{"quantity", c.quantity}, {"limit_price", c.limit_price}, {"trigger_level", c.trigger_level}};
  // Only a change of time in force records it, so other changes keep their bytes.
  if (c.tif) j["tif"] = *c.tif;
}
inline void from_json(const nlohmann::json& j, OrderChange& c) {
  j.at("quantity").get_to(c.quantity); j.at("limit_price").get_to(c.limit_price); j.at("trigger_level").get_to(c.trigger_level);
  added_field(j, "tif", c.tif);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Dividend, symbol, ex_date, per_share)
}
namespace openport::analytics {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AnalyticsOptions, parity_strikes, flip_range, flip_steps,
    fallback_rate, deamericanize, min_days_for_rate, exposure_min_days, max_spot_age_minutes)
}
namespace openport::server {
inline void to_json(nlohmann::json& j, const TradingCommand& c) {
  j = nlohmann::json{{"kind", c.kind}, {"order", c.order}, {"order_id", c.order_id}, {"limits", c.limits},
      {"guardrails", c.guardrails}, {"floor_share", c.floor_share}, {"expected_revision", c.expected_revision},
      {"reason", c.reason}, {"symbol", c.symbol}, {"settlement", c.settlement}, {"initial_cash", c.initial_cash},
      {"rules", c.rules}, {"required_pass", c.required_pass}, {"amount", c.amount}, {"change", c.change},
      {"underlying", c.underlying}, {"account", c.account}, {"name", c.name}, {"trade", c.trade}, {"shares", c.shares},
      {"day", c.day}, {"plan", c.plan}, {"review", c.review}, {"note", c.note}, {"tags", c.tags},
      {"quantity", c.quantity}, {"actor", c.actor}};
  // Later fields are recorded only when set, so earlier commands keep their bytes.
  if (c.stock_price) j["stock_price"] = {{"symbol", c.stock_price->symbol}, {"time", c.stock_price->time}, {"price", c.stock_price->price}};
  if (c.do_not_exercise) j["do_not_exercise"] = true;
  if (!c.trades.empty()) j["trades"] = c.trades;
  if (c.close_pricing.limit) { j["close_limit"] = true; j["limit_ticks"] = c.close_pricing.limit_ticks; }
}
inline void from_json(const nlohmann::json& j, TradingCommand& c) {
  j.at("kind").get_to(c.kind);
  j.at("order").get_to(c.order);
  j.at("order_id").get_to(c.order_id);
  j.at("limits").get_to(c.limits);
  j.at("guardrails").get_to(c.guardrails);
  j.at("floor_share").get_to(c.floor_share);
  j.at("expected_revision").get_to(c.expected_revision);
  j.at("reason").get_to(c.reason);
  j.at("symbol").get_to(c.symbol);
  j.at("settlement").get_to(c.settlement);
  j.at("initial_cash").get_to(c.initial_cash);
  j.at("rules").get_to(c.rules);
  j.at("required_pass").get_to(c.required_pass);
  j.at("amount").get_to(c.amount);
  j.at("change").get_to(c.change);
  j.at("underlying").get_to(c.underlying);
  j.at("account").get_to(c.account);
  j.at("name").get_to(c.name);
  j.at("trade").get_to(c.trade);
  j.at("shares").get_to(c.shares);
  j.at("day").get_to(c.day);
  j.at("plan").get_to(c.plan);
  j.at("review").get_to(c.review);
  j.at("note").get_to(c.note);
  j.at("tags").get_to(c.tags);
  j.at("quantity").get_to(c.quantity);
  if (j.contains("stock_price")) {
    const auto& p = j.at("stock_price");
    c.stock_price = trading::StockPrice{p.at("symbol").get<std::string>(), p.at("time").get<md::Timestamp>(), p.at("price").get<trading::Money>()};
  }
  trading::added_field(j, "actor", c.actor);
  trading::added_field(j, "do_not_exercise", c.do_not_exercise);
  trading::added_field(j, "trades", c.trades);
  trading::added_field(j, "close_limit", c.close_pricing.limit);
  trading::added_field(j, "limit_ticks", c.close_pricing.limit_ticks);
}
}
