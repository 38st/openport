#include "strategy_template.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include "openport/trading/types.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument(reason); }
bool finite(const json& value) { return value.is_number() && std::isfinite(value.get<double>()); }
std::string choice(const json& value, std::initializer_list<std::string_view> choices, std::string_view field) {
  if (value.is_string()) for (auto item : choices) if (value == item) return value.get<std::string>();
  invalid("Invalid " + std::string(field));
}
std::string numeric(const json& value) {
  std::string text = value.dump();
  if (text.ends_with(".0")) text.resize(text.size() - 2);
  return text;
}
json nullable(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
/// A number as the terminal writes it: 130, 0.01, 5752.5.
std::string text(double value) {
  std::ostringstream out;
  out.precision(15);
  out << value;
  return out.str();
}
std::string expiry_id(const analytics::SliceMetrics& slice) {
  return md::format_date(slice.expiry) + (slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM") + (slice.root.empty() ? "" : "-" + slice.root);
}
}
void strict_keys(const json& value, std::initializer_list<std::string_view> allowed) {
  if (!value.is_object()) invalid("Expected a JSON object");
  for (const auto& [key, field] : value.items()) {
    (void)field;
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) invalid("Unknown field: " + key);
  }
}
double bounded_number(const json& value, double low, double high, std::string_view field) {
  if (!finite(value) || value.get<double>() < low || value.get<double>() > high)
    invalid(std::string(field) + " is outside its allowed range");
  return value.get<double>();
}
void validate_template(const json& value) {
  const auto kind = choice(value.at("kind"), {"vertical", "condor", "iron-butterfly", "strangle", "straddle", "butterfly", "calendar", "diagonal"}, "template kind");
  if (kind == "vertical") strict_keys(value, {"kind", "type", "direction", "target", "width"});
  else if (kind == "condor") strict_keys(value, {"kind", "target", "width"});
  else if (kind == "iron-butterfly") strict_keys(value, {"kind", "width"});
  else if (kind == "strangle") strict_keys(value, {"kind", "side", "delta"});
  else if (kind == "straddle") strict_keys(value, {"kind", "side"});
  else if (kind == "butterfly") strict_keys(value, {"kind", "type", "target", "width"});
  else strict_keys(value, {"kind", "type", "target", "farExpiry", "offset"});
  if (kind == "vertical" || kind == "butterfly" || kind == "calendar" || kind == "diagonal")
    choice(value.at("type"), {"call", "put"}, "option type");
  if (kind == "vertical") choice(value.at("direction"), {"credit", "debit"}, "premium direction");
  if (kind == "strangle" || kind == "straddle") choice(value.at("side"), {"buy", "sell"}, "side");
  if (kind == "vertical" || kind == "condor" || kind == "iron-butterfly" || kind == "butterfly") {
    if (!finite(value.at("width")) || value.at("width").get<double>() <= 0) invalid("Width must be greater than zero points.");
    bounded_number(value.at("width"), 0.000001, 1000000, "width");
  }
  if (kind == "strangle") bounded_number(value.at("delta"), 0.000001, 99.999999, "delta");
  if (kind == "calendar" || kind == "diagonal") {
    if (!value.at("farExpiry").is_string() || value.at("farExpiry").get<std::string>().size() < 12) invalid("farExpiry must be an expiry ID");
    const auto offset = bounded_number(value.at("offset"), -1000000, 1000000, "offset");
    if (kind == "diagonal" && offset == 0) invalid("Diagonal far-strike offset must be nonzero points.");
  }
  if (kind == "vertical" || kind == "condor" || kind == "butterfly" || kind == "calendar" || kind == "diagonal") {
    const auto& target = value.at("target");
    const auto mode = choice(target.at("mode"), {"atm", "delta", "points", "moves", "strike"}, "target mode");
    if (mode == "atm") strict_keys(target, {"mode"});
    else {
      strict_keys(target, {"mode", "value"});
      if (!finite(target.at("value"))) invalid("Enter a finite strike target.");
      if (mode == "delta" && !(target.at("value").get<double>() > 0 && target.at("value").get<double>() < 100))
        invalid("Target delta must be between 0 and 100, excluding the endpoints.");
      bounded_number(target.at("value"), -1000000, 1000000, "target value");
    }
    if (kind == "condor" && (mode != "delta" && mode != "moves")) invalid("Condor shorts require a delta or expected-move target.");
    if (kind == "condor" && target.at("value").get<double>() <= 0) invalid("Condor target must be greater than zero.");
    if (kind == "butterfly" && mode != "atm" && mode != "strike") invalid("Butterfly centre requires ATM or a strike.");
    if ((kind == "calendar" || kind == "diagonal") && mode != "atm" && mode != "delta") invalid("Calendar and diagonal strikes require ATM or delta.");
  }
}
std::string template_tag(const json& value) {
  const auto kind = value.at("kind").get<std::string>();
  std::string target;
  if (value.contains("target")) {
    const auto& pick = value.at("target");
    const auto mode = pick.at("mode").get<std::string>();
    target = mode == "atm" ? "atm" : numeric(pick.at("value")) +
        (mode == "delta" ? "d" : mode == "points" ? "pt" : mode == "moves" ? "em" : "k");
  }
  std::string tag;
  if (kind == "vertical") tag = value.at("type").get<std::string>() + "-" + value.at("direction").get<std::string>() + "-" + target + "-" + numeric(value.at("width")) + "w";
  else if (kind == "condor") tag = "iron-condor-" + target + "-" + numeric(value.at("width")) + "w";
  else if (kind == "iron-butterfly") tag = "iron-butterfly-atm-" + numeric(value.at("width")) + "w";
  else if (kind == "strangle" || kind == "straddle") tag = std::string(value.at("side") == "sell" ? "short-" : "long-") + kind + "-" + (kind == "straddle" ? "atm" : numeric(value.at("delta")) + "d");
  else if (kind == "butterfly") tag = "long-" + value.at("type").get<std::string>() + "-butterfly-" + target + "-" + numeric(value.at("width")) + "w";
  else tag = value.at("type").get<std::string>() + "-" + kind + "-" + target + (kind == "diagonal" ? "-" + numeric(value.at("offset")) + "pt" : "");
  return tag.substr(0, 32);
}
json template_chain(const analytics::UnderlyingMetrics& metrics, const analytics::SliceMetrics& slice) {
  json rows = json::array();
  const auto option = [](const analytics::OptionMetrics& item) -> json {
    if (item.id == analytics::kNoInstrument) return nullptr;
    const auto decision = trading::eligible(item.contract);
    return {{"symbol", item.contract.osi_symbol()}, {"tradable", decision.ok()}, {"untradable_reason", decision.message},
            {"bid", nullable(item.bid)}, {"ask", nullable(item.ask)}, {"mid", nullable(item.mid)}, {"delta", nullable(item.delta)}, {"iv", nullable(item.iv)}, {"bid_iv", nullable(item.bid_iv)}, {"ask_iv", nullable(item.ask_iv)},
            {"gamma", nullable(item.gamma)}, {"vega", nullable(item.vega)}, {"theta", nullable(item.theta)}, {"vanna", nullable(item.vanna)},
            {"bid_size", nullable(item.bid_size)}, {"ask_size", nullable(item.ask_size)}, {"oi", nullable(item.open_interest)},
            {"volume", nullable(item.volume)}, {"vendor_iv", nullable(item.vendor_iv)}};
  };
  for (const auto& row : slice.strikes) rows.push_back({{"strike", row.strike}, {"call", option(row.call)}, {"put", option(row.put)}});
  return {{"symbol", metrics.symbol}, {"expiry", {{"id", expiry_id(slice)}, {"expiry_time", md::format_timestamp(slice.expiry_time)},
      {"forward", nullable(slice.forward.forward)}, {"atm_iv", nullable(slice.atm_iv)}, {"days", slice.years * 365}}}, {"strikes", rows}};
}
json build_template(const json& value, const json& near, const json& far) {
  validate_template(value);
  const auto listed = [](const json& chain) {
    std::vector<json> rows;
    for (const auto& row : chain.at("strikes")) if (finite(row.at("strike")) && row.at("strike").get<double>() > 0) rows.push_back(row);
    std::sort(rows.begin(), rows.end(), [](const auto& first, const auto& second) { return first.at("strike") < second.at("strike"); });
    return rows;
  };
  const auto nearest = [&](const json& chain, double target, const std::string& purpose) {
    const auto rows = listed(chain);
    if (rows.empty()) invalid(chain.at("expiry").at("id").get<std::string>() + ": no listed strikes for " + purpose + ".");
    const double low = rows.front().at("strike").template get<double>(), high = rows.back().at("strike").template get<double>();
    if (!std::isfinite(target) || target < low || target > high)
      invalid(purpose + " at " + text(target) + ": outside the loaded strike range " + text(low) + "–" + text(high) + ". Widen the chain window.");
    return *std::min_element(rows.begin(), rows.end(), [&](const auto& first, const auto& second) {
      return std::abs(first.at("strike").template get<double>() - target) < std::abs(second.at("strike").template get<double>() - target);
    });
  };
  const auto atm = [&] {
    const auto& forward = near.at("expiry").at("forward");
    if (!finite(forward) || forward.get<double>() <= 0) invalid("The selected expiry has no forward for an ATM strike.");
    return forward.get<double>();
  };
  const auto pick = [&](const std::string& type, const json& target) {
    const auto mode = target.at("mode").get<std::string>();
    if (mode == "atm") return nearest(near, atm(), "ATM strike");
    const auto amount = target.at("value").get<double>();
    if (mode == "delta") {
      auto rows = listed(near);
      std::erase_if(rows, [&](const auto& row) {
        const auto quote = row.at(type);
        return quote.is_null() || !finite(quote.at("delta")) ||
            quote.at("delta").template get<double>() < (type == "put" ? -1 : 0) || quote.at("delta").template get<double>() > (type == "put" ? 0 : 1);
      });
      if (rows.empty()) invalid("No " + type + " deltas in the loaded chain.");
      const double signed_delta = (type == "put" ? -1 : 1) * amount / 100;
      const auto delta = [&](const json& row) { return row.at(type).at("delta").get<double>(); };
      const auto range = std::minmax_element(rows.begin(), rows.end(), [&](const auto& first, const auto& second) { return delta(first) < delta(second); });
      if (signed_delta < delta(*range.first) || signed_delta > delta(*range.second))
        invalid(text(amount) + "Δ " + type + " is outside the loaded delta range. Widen the chain window.");
      return *std::min_element(rows.begin(), rows.end(), [&](const auto& first, const auto& second) { return std::abs(delta(first) - signed_delta) < std::abs(delta(second) - signed_delta); });
    }
    if (mode == "strike") return nearest(near, amount, "Centre strike");
    double offset = amount;
    if (mode == "moves") {
      const auto& expiry = near.at("expiry");
      if (!finite(expiry.at("atm_iv")) || !finite(expiry.at("days")) || expiry.at("atm_iv").get<double>() <= 0 || expiry.at("days").get<double>() <= 0) invalid("Expected move unavailable: the expiry needs a forward, ATM volatility and time remaining.");
      offset *= atm() * expiry.at("atm_iv").get<double>() * std::sqrt(expiry.at("days").get<double>() / 365);
    }
    return nearest(near, atm() + (type == "put" ? -offset : offset), type + " target");
  };
  const auto leg = [](const json& chain, const json& row, const std::string& type, const std::string& side, int ratio = 1) {
    const auto& quote = row.at(type);
    const auto name = chain.at("expiry").at("id").get<std::string>() + " " + text(row.at("strike").get<double>()) + " " + type;
    if (quote.is_null() || !finite(quote.at("bid")) || !finite(quote.at("ask")) || !finite(quote.at("mid")) || quote.at("bid").get<double>() < 0 || quote.at("ask") < quote.at("bid")) invalid(name + ": no valid two-sided quote.");
    if (!quote.value("tradable", false) || quote.value("symbol", "").empty()) {
      const auto reason = quote.contains("untradable_reason") && quote.at("untradable_reason").is_string() ? quote.at("untradable_reason").get<std::string>() : std::string();
      invalid(name + ": " + (reason.empty() ? "unavailable for paper trading" : reason) + ".");
    }
    return json{{"symbol", quote.at("symbol")}, {"underlying", chain.at("symbol")}, {"side", side}, {"ratio", ratio},
                {"type", type}, {"strike", row.at("strike")}, {"expiry", chain.at("expiry").at("id")}, {"quote", quote}};
  };
  const auto wing = [&](const json& center, double offset, const std::string& type) {
    const auto row = nearest(near, center.at("strike").get<double>() + offset, type + " wing");
    if ((row.at("strike").get<double>() - center.at("strike").get<double>()) * offset <= 0)
      invalid(type + " wing snaps to the centre strike " + text(center.at("strike").get<double>()) + "; increase the width.");
    return leg(near, row, type, "buy");
  };
  if (listed(near).empty()) invalid("The selected chain has no listed strikes.");
  const auto kind = value.at("kind").get<std::string>();
  const auto type = value.value("type", "call");
  const double width = value.value("width", 0.0);
  json legs = json::array(), widths = json::array();
  if (kind == "vertical") {
    const auto short_row = pick(type, value.at("target"));
    const auto long_leg = wing(short_row, (type == "call" ? 1 : -1) * (value.at("direction") == "credit" ? 1 : -1) * width, type);
    legs = {leg(near, short_row, type, "sell"), long_leg};
    widths = {std::abs(short_row.at("strike").get<double>() - long_leg.at("strike").get<double>())};
  } else if (kind == "condor" || kind == "iron-butterfly" || kind == "strangle" || kind == "straddle") {
    const json target = kind == "condor" ? value.at("target") : kind == "strangle" ? json{{"mode", "delta"}, {"value", value.at("delta")}} : json{{"mode", "atm"}};
    const auto put = pick("put", target), call = pick("call", target);
    if (kind == "condor" && put.at("strike") >= call.at("strike")) invalid("Condor short put must be below the short call after snapping.");
    if (kind == "strangle" && put.at("strike") >= call.at("strike")) invalid("Strangle put must be below the call after snapping.");
    if (kind == "condor" || kind == "iron-butterfly") {
      const auto low = wing(put, -width, "put"), high = wing(call, width, "call");
      legs = {low, leg(near, put, "put", "sell"), leg(near, call, "call", "sell"), high};
      widths = {put.at("strike").get<double>() - low.at("strike").get<double>(), high.at("strike").get<double>() - call.at("strike").get<double>()};
    } else {
      legs = {leg(near, put, "put", value.at("side")), leg(near, call, "call", value.at("side"))};
      widths = {call.at("strike").get<double>() - put.at("strike").get<double>()};
    }
  } else if (kind == "butterfly") {
    const auto center = pick(type, value.at("target"));
    const auto low = wing(center, -width, type), high = wing(center, width, type);
    const double lower = center.at("strike").get<double>() - low.at("strike").get<double>();
    const double upper = high.at("strike").get<double>() - center.at("strike").get<double>();
    if (std::abs(lower - upper) > 1e-8) invalid("Listed strikes cannot form equal butterfly wings at this centre and width.");
    legs = {low, leg(near, center, type, "sell", 2), high};
    widths = {lower, upper};
  } else {
    if (far.is_null() || far.at("expiry").at("id") != value.at("farExpiry"))
      invalid("Load the far chain (" + value.at("farExpiry").get<std::string>() + ") before building this template.");
    if (far.at("symbol") != near.at("symbol")) invalid("Both expiries must have the same underlying.");
    if (far.at("expiry").at("expiry_time") <= near.at("expiry").at("expiry_time")) invalid("Far expiry must settle after the selected expiry.");
    const auto short_row = pick(type, value.at("target"));
    const double strike = short_row.at("strike").get<double>();
    json long_row;
    if (kind == "calendar") {
      for (const auto& row : listed(far)) if (row.at("strike").get<double>() == strike) long_row = row;
      if (long_row.is_null()) invalid("Far expiry has no listed " + text(strike) + " strike for a calendar.");
    } else long_row = nearest(far, strike + value.at("offset").get<double>(), "Far strike");
    if (kind == "diagonal" && long_row.at("strike") == short_row.at("strike")) invalid("Diagonal offset snaps to the near strike; increase its size.");
    legs = {leg(near, short_row, type, "sell"), leg(far, long_row, type, "buy")};
    widths = {std::abs(long_row.at("strike").get<double>() - strike)};
  }
  std::set<std::string> symbols;
  for (const auto& item : legs) if (!symbols.insert(item.at("symbol").get<std::string>()).second) invalid("The selected contracts overlap after snapping.");
  return {{"legs", legs}, {"tag", template_tag(value)}, {"widths", widths}};
}
}  // namespace openport::server
