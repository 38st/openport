#pragma once

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include "openport/server/event_calendar.hpp"
#include "openport/trading/events.hpp"

namespace openport::server {
inline nlohmann::json calendar_event_json(const trading::PlanEvent& e) {
  // Keep journal normalization unchanged; HTTP removes only insignificant zeros.
  auto time = e.time;
  if (e.kind == "news" && time.find('.') != std::string::npos && time.ends_with('Z')) {
    time.pop_back();
    while (time.ends_with('0')) time.pop_back();
    if (time.ends_with('.')) time.pop_back();
    time += 'Z';
  }
  nlohmann::json j{{"kind", e.kind}, {"time", time}};
  if (!e.symbol.empty()) j["symbol"] = e.symbol;
  if (!e.session.empty()) j["session"] = e.session;
  if (!e.label.empty()) j["label"] = e.label;
  return j;
}
inline nlohmann::json calendar_events_json(const std::vector<trading::PlanEvent>& events) {
  auto j = nlohmann::json::array();
  for (const auto& e : events) j.push_back(calendar_event_json(e));
  return j;
}
inline std::vector<trading::PlanEvent> parse_calendar_events(const nlohmann::json& j) {
  if (!j.is_array() || j.size() > 256) throw std::invalid_argument("events must be an array of at most 256 entries");
  trading::AccountRules r;
  for (const auto& value : j) {
    if (!value.is_object()) throw std::invalid_argument("each event must be an object");
    for (const auto& [key, field] : value.items()) {
      if (key != "kind" && key != "time" && key != "symbol" && key != "session" && key != "label")
        throw std::invalid_argument("Unknown event field: " + key);
      if (!field.is_string()) throw std::invalid_argument("event " + key + " must be a string");
    }
    if (!value.contains("kind") || !value.contains("time")) throw std::invalid_argument("each event requires kind and time");
    if (value.contains("session") && value.at("kind") != "earnings")
      throw std::invalid_argument("event session is allowed only for earnings");
    if (value.contains("session") && value.at("session") != "before_open" && value.at("session") != "after_close")
      throw std::invalid_argument("earnings session must be before_open or after_close");
    r.events.push_back({value.at("kind").get<std::string>(), value.at("time").get<std::string>(),
        value.value("symbol", ""), value.value("session", ""), value.value("label", "")});
  }
  trading::normalize_event_rules(r);
  return r.events;
}
inline nlohmann::json event_window_json(const trading::EventWindow& w, md::Timestamp time) {
  return {{"kind", w.kind}, {"symbol", w.symbol.empty() ? nlohmann::json(nullptr) : nlohmann::json(w.symbol)},
      {"label", w.label.empty() ? nlohmann::json(nullptr) : nlohmann::json(w.label)},
      {"start", md::format_timestamp(w.start)}, {"end", md::format_timestamp(w.end)}, {"active", w.start <= time && time < w.end}};
}
inline nlohmann::json restricting_events_json(const trading::AccountRules& r, md::Timestamp time, bool active_only) {
  auto result = nlohmann::json::array();
  if (time <= 0 || (r.events.empty() && r.hold_restrictions.empty())) return result;
  auto until = time;
  if (!active_only) {
    constexpr auto room = 8 * md::kNanosPerDay;
    const auto extend = [](md::Timestamp t) { return t > std::numeric_limits<md::Timestamp>::max() - room
        ? std::numeric_limits<md::Timestamp>::max() : t + room; };
    until = extend(time);
    for (const auto& e : r.events) until = std::max(until, extend(trading::event_time(e)));
  }
  for (const auto& w : trading::event_windows(r, time, until)) result.push_back(event_window_json(w, time));
  return result;
}
}  // namespace openport::server
