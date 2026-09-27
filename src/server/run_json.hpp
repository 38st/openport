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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradingCommand, kind, order, order_id, limits, guardrails,
    floor_share, expected_revision, reason, symbol, settlement, initial_cash, rules, required_pass,
    amount, change, underlying, account, name, trade, shares, day, plan, review, note, tags, quantity)
}
