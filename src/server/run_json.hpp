#pragma once

#include "../trading/state.hpp"
#include "openport/server/desk.hpp"

namespace openport::md {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScheduledDay, date, name, closed, close_hour, overnight_until)
}
namespace openport::trading {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OrderChange, quantity, limit_price, trigger_level)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Dividend, symbol, ex_date, per_share)
}
namespace openport::analytics {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AnalyticsOptions, parity_strikes, flip_range, flip_steps,
    fallback_rate, deamericanize, min_days_for_rate, exposure_min_days, max_spot_age_minutes)
}
namespace openport::server {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(TradingCommand, kind, order, order_id, limits, guardrails,
    floor_share, expected_revision, reason, symbol, settlement, initial_cash, rules, required_pass,
    amount, change, underlying, account, name, trade, shares, day, plan, review, note, tags, quantity, actor)
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
  trading::added_field(j, "actor", c.actor);
}
}
