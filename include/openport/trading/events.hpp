#pragma once

#include <istream>
#include "openport/trading/types.hpp"

namespace openport::trading {
void validate_event_rules(const AccountRules& rules);
/// Canonical sort/dedup and default earnings session. Capture the business-day
/// calendar only at the start of an attempt, never while recovering one.
void normalize_event_rules(AccountRules& rules, bool capture_calendar = false);
[[nodiscard]] std::vector<PlanEvent> parse_event_calendar(std::istream& input);
[[nodiscard]] Timestamp event_time(const PlanEvent& event);
struct EventWindow {
  std::string key;
  std::string kind;
  std::string symbol;
  std::string label;
  Timestamp start = 0;
  Timestamp end = 0;
};
/// Windows intersecting [from,to], ordered by start, then stable identity.
[[nodiscard]] std::vector<EventWindow> event_windows(const AccountRules& rules, Timestamp from, Timestamp to);
[[nodiscard]] Decision event_entry_check(const AccountRules& rules, std::string_view underlying, Timestamp time);
[[nodiscard]] Decision event_decision(const AccountRules& rules, const EventWindow& window, Timestamp time);
}  // namespace openport::trading
