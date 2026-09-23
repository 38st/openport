#include "openport/server/api.hpp"

#include <charconv>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace openport::server {
namespace {

using analytics::SliceMetrics;
using analytics::UnderlyingMetrics;
using nlohmann::json;

/// Rounds to `digits` significant digits, keeping payloads small without losing
/// anything a screen can show. Non-finite values become JSON null.
json sig(double x, int digits = 6) {
  if (!std::isfinite(x)) return nullptr;
  if (x == 0.0) return 0.0;
  // Scale by an exact power of ten in the direction that keeps it >= 1, so large
  // values come back as clean integers rather than 11665899999.999998.
  const int exponent = digits - 1 - static_cast<int>(std::floor(std::log10(std::abs(x))));
  if (exponent >= 0) {
    const double scale = std::pow(10.0, exponent);
    return std::round(x * scale) / scale;
  }
  const double factor = std::pow(10.0, -exponent);
  return std::round(x / factor) * factor;
}

json price(double x) {
  if (!std::isfinite(x)) return nullptr;
  return std::round(x * 1e4) / 1e4;
}

std::string settlement_of(const SliceMetrics& slice) {
  return slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM";
}

std::string expiry_id(const SliceMetrics& slice) {
  return md::format_date(slice.expiry) + settlement_of(slice) +
         (slice.root.empty() ? "" : "-" + slice.root);
}

json coverage_json(const analytics::Coverage& c) {
  return {{"options", c.options},
          {"quoted", c.quoted},
          {"priced", c.priced},
          {"open_interest", c.open_interest}};
}

json spot_source_json(const UnderlyingMetrics& m) {
  return m.spot_source.empty() ? json(nullptr) : json(m.spot_source);
}

json market_json(md::Timestamp ts) {
  const auto session = md::market_session(ts);
  return {{"open", session.open},
          {"note", session.note},
          {"next_open",
           session.next_open ? json(md::format_timestamp(*session.next_open)) : json(nullptr)}};
}

json expiry_json(const SliceMetrics& slice) {
  const double rate = -std::log(slice.forward.discount) / slice.years;
  return {
      {"id", expiry_id(slice)},
      {"expiry", md::format_date(slice.expiry)},
      {"settlement", settlement_of(slice)},
      {"style", slice.style == pricing::ExerciseStyle::American ? "american" : "european"},
      {"coverage", coverage_json(slice.coverage)},
      {"expiry_time", md::format_timestamp(slice.expiry_time)},
      {"days", sig(slice.years * 365.0, 5)},
      {"forward", price(slice.forward.forward)},
      {"discount", sig(slice.forward.discount, 9)},
      {"rate", sig(rate, 4)},
      {"rate_fitted", slice.forward.fitted_discount},
      {"atm_iv", sig(slice.atm_iv)},
      {"gex", sig(slice.gex)},
      {"vex", sig(slice.vex)},
      {"strikes", slice.strikes.size()},
  };
}

json exposure_summary(const UnderlyingMetrics& m) {
  return {
      {"oi_coverage", sig(m.exposure.oi_coverage)},
      {"gex", sig(m.exposure.gex)},
      {"vex", sig(m.exposure.vex)},
      {"gamma_flip", price(m.exposure.gamma_flip)},
      {"call_wall", price(m.exposure.call_wall)},
      {"put_wall", price(m.exposure.put_wall)},
  };
}

json option_json(const analytics::OptionMetrics& o) {
  if (o.id == analytics::kNoInstrument) return nullptr;
  return {
      {"bid", price(o.bid)},
      {"ask", price(o.ask)},
      {"mid", price(o.mid)},
      {"iv", sig(o.iv)},
      {"bid_iv", sig(o.bid_iv)},
      {"ask_iv", sig(o.ask_iv)},
      {"delta", sig(o.delta)},
      {"gamma", sig(o.gamma)},
      {"vega", sig(o.vega)},
      {"theta", sig(o.theta)},
      {"vanna", sig(o.vanna)},
      {"oi",
       o.has_open_interest && std::isfinite(o.open_interest) ? json(o.open_interest) : json(nullptr)},
      {"vendor_iv", sig(o.vendor_iv)},
  };
}

std::optional<std::map<std::string, std::string>> parse_query(std::string_view query) {
  std::map<std::string, std::string> out;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    const std::size_t eq = pair.find('=');
    const auto [it, inserted] = out.emplace(
        std::string(pair.substr(0, eq)),
        eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1)));
    if (!inserted) return std::nullopt;
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return out;
}

template <typename T>
bool bounded_number(const std::map<std::string, std::string>& query, const std::string& key,
                    T minimum, T maximum, T& value) {
  const auto it = query.find(key);
  if (it == query.end()) return true;
  const auto& text = it->second;
  if constexpr (std::is_integral_v<T>) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return false;
  } else {
    // Apple's pinned libc++ lacks floating-point from_chars. A classic-locale,
    // non-skipping stream still requires the entire value to be one number.
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    if (!(input >> std::noskipws >> value) || !input.eof()) return false;
  }
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

ApiResponse ok(const json& body) {
  return {200, body.dump()};
}

ApiResponse error(int status, const std::string& message) {
  return {status, json{{"error", message}}.dump()};
}

bool in_window(double strike, double spot, double window) {
  // A hair of tolerance so strikes exactly on the edge (4900 of 5000 at 2%) stay in.
  return !(window > 0.0) || !(spot > 0.0) || std::abs(strike / spot - 1.0) <= window + 1e-12;
}

json underlyings_json(const MetricsSource& source, const EngineStatus& status, bool details) {
  json underlyings = json::array();
  std::set<std::string> symbols;
  for (const auto& symbol : source.symbols()) symbols.insert(symbol);
  for (const auto& [symbol, health] : status.underlyings) symbols.insert(symbol);
  for (const std::string& symbol : symbols) {
    json item{{"symbol", symbol}, {"spot", nullptr}, {"as_of", nullptr}, {"version", 0}};
    if (details) {
      item["expiries"] = 0;
      item["options"] = 0;
    }
    if (const auto health = status.underlyings.find(symbol); health != status.underlyings.end()) {
      const auto& h = health->second;
      item["state"] = md::to_string(h.state);
      item["message"] = h.message;
      item["last_success"] =
          h.last_success > 0 ? json(md::format_timestamp(h.last_success)) : json(nullptr);
      item["last_error"] = h.last_error.empty() ? json(nullptr) : json(h.last_error);
      item["last_error_time"] =
          h.last_error_time > 0 ? json(md::format_timestamp(h.last_error_time)) : json(nullptr);
    }
    const auto m = source.metrics(symbol);
    if (m) {
      item["spot"] = price(m->spot);
      item["as_of"] = md::format_timestamp(m->as_of);
      item["version"] = m->version;
      if (details) {
        item["expiries"] = m->slices.size();
        item["options"] = m->options_priced;
      }
    }
    underlyings.push_back(std::move(item));
  }
  return underlyings;
}

json status_json(const MetricsSource& source) {
  const EngineStatus s = source.status();
  return {
      {"market", market_json(md::now())},
      {"provider",
       {{"name", s.provider},
        {"realtime", s.capabilities.realtime},
        {"realtime_plan_dependent", s.capabilities.realtime_plan_dependent},
        {"poll_interval_seconds", s.capabilities.poll_interval.count()},
        {"delay_seconds", s.capabilities.delay.count()},
        {"trades", s.capabilities.trades},
        {"open_interest", s.capabilities.open_interest},
        {"vendor_greeks", s.capabilities.vendor_greeks}}},
      {"feed",
       {{"state", md::to_string(s.feed_state)},
        {"message", s.feed_message},
        {"updated",
         s.feed_updated > 0 ? json(md::format_timestamp(s.feed_updated)) : json(nullptr)}}},
      {"underlyings", underlyings_json(source, s, true)},
      {"engine",
       {{"events", s.events},
        {"events_per_second", sig(s.events_per_second, 4)},
        {"analytics_ms", sig(s.analytics_ms, 4)},
        {"queue_depth", s.queue_depth},
        {"coalesced_events", s.coalesced_events},
        {"dropped_events", s.dropped_events},
        {"overloaded", s.overloaded},
        {"contracts", s.contracts},
        {"nonstandard_contracts", s.nonstandard_contracts},
        {"uptime_seconds", s.started > 0 ? (md::now() - s.started) / md::kNanosPerSecond : 0}}},
  };
}

json summary_json(const UnderlyingMetrics& m) {
  json expiries = json::array();
  for (const SliceMetrics& slice : m.slices) expiries.push_back(expiry_json(slice));
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"compute_ms", sig(m.compute_ms, 4)},
          {"american_approximation", m.american_approximation},
          {"coverage", coverage_json(m.coverage)},
          {"exposure", exposure_summary(m)},
          {"expiries", expiries}};
}

json chain_json(const UnderlyingMetrics& m, const SliceMetrics& slice, double window) {
  json strikes = json::array();
  for (const auto& row : slice.strikes) {
    if (!in_window(row.strike, m.spot, window)) continue;
    strikes.push_back({{"strike", row.strike},
                       {"iv", sig(row.iv)},
                       {"gex", sig(row.gex)},
                       {"vex", sig(row.vex)},
                       {"call", option_json(row.call)},
                       {"put", option_json(row.put)}});
  }
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"expiry", expiry_json(slice)},
          {"strikes", strikes}};
}

json exposure_json(const UnderlyingMetrics& m, std::size_t max_expiries, double window) {
  const std::size_t count = std::min(max_expiries, m.slices.size());
  std::set<double> strike_set;
  for (std::size_t i = 0; i < count; ++i) {
    for (const auto& row : m.slices[i].strikes) {
      if (in_window(row.strike, m.spot, window)) strike_set.insert(row.strike);
    }
  }
  const std::vector<double> strikes(strike_set.begin(), strike_set.end());
  std::map<double, std::size_t> column;
  for (std::size_t i = 0; i < strikes.size(); ++i) column[strikes[i]] = i;

  std::vector<double> total(strikes.size(), 0.0);
  json expiries = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    const SliceMetrics& slice = m.slices[i];
    std::vector<double> gex(strikes.size(), 0.0);
    std::vector<double> vex(strikes.size(), 0.0);
    for (const auto& row : slice.strikes) {
      const auto it = column.find(row.strike);
      if (it == column.end()) continue;
      gex[it->second] = row.gex;
      vex[it->second] = row.vex;
      total[it->second] += row.gex;
    }
    json gex_json = json::array();
    json vex_json = json::array();
    for (std::size_t j = 0; j < strikes.size(); ++j) {
      gex_json.push_back(sig(gex[j], 4));
      vex_json.push_back(sig(vex[j], 4));
    }
    expiries.push_back({{"id", expiry_id(slice)},
                        {"expiry", md::format_date(slice.expiry)},
                        {"days", sig(slice.years * 365.0, 4)},
                        {"gex", gex_json},
                        {"vex", vex_json}});
  }
  json total_json = json::array();
  for (double x : total) total_json.push_back(sig(x, 4));
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"strikes", strikes},
          {"expiries", expiries},
          {"total_gex", total_json},
          {"exposure", exposure_summary(m)}};
}

json surface_json(const UnderlyingMetrics& m, std::size_t max_expiries, double window) {
  json expiries = json::array();
  for (std::size_t i = 0; i < std::min(max_expiries, m.slices.size()); ++i) {
    const SliceMetrics& slice = m.slices[i];
    json points = json::array();
    for (const auto& row : slice.strikes) {
      if (!std::isfinite(row.iv) || !in_window(row.strike, m.spot, window)) continue;
      // Smiles are read off the out-of-the-money side. Quotes whose spread is more than
      // half their price (0.05 bid, 0.15 offer far in the wings) imply almost any vol,
      // so they are left out of the picture rather than drawn as spikes.
      const auto& otm = row.strike >= slice.forward.forward ? row.call : row.put;
      if (!(otm.bid > 0.0) || !(otm.ask - otm.bid <= 0.5 * otm.mid)) continue;
      points.push_back({{"strike", row.strike},
                        {"k", sig(std::log(row.strike / slice.forward.forward), 6)},
                        {"iv", sig(row.iv)},
                        {"bid_iv", sig(otm.bid_iv)},
                        {"ask_iv", sig(otm.ask_iv)}});
    }
    expiries.push_back({{"id", expiry_id(slice)},
                        {"expiry", md::format_date(slice.expiry)},
                        {"days", sig(slice.years * 365.0, 4)},
                        {"forward", price(slice.forward.forward)},
                        {"atm_iv", sig(slice.atm_iv)},
                        {"points", points}});
  }
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"expiries", expiries}};
}

}  // namespace

ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source) {
  if (request.method != "GET") return error(405, "only GET is supported");
  const std::string_view target = request.target;
  const std::size_t question = target.find('?');
  const std::string_view path = target.substr(0, question);
  const auto parsed = parse_query(question == std::string_view::npos ? std::string_view{}
                                                                     : target.substr(question + 1));
  if (!parsed) return error(400, "duplicate query parameter");
  const auto& query = *parsed;
  int expiries = 8;
  double window = 0.0;
  if (!bounded_number(query, "expiries", 1, 500, expiries)) {
    return error(400, "expiries must be an integer in [1, 500]");
  }
  if (!bounded_number(query, "window", 0.0, 1.0, window)) {
    return error(400, "window must be a finite number in [0, 1]");
  }

  if (path == "/api/status") return ok(status_json(source));

  constexpr std::string_view prefix = "/api/underlyings/";
  if (!path.starts_with(prefix)) return error(404, "unknown endpoint");
  std::string_view rest = path.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  const std::string symbol(rest.substr(0, slash));
  const std::string_view view =
      slash == std::string_view::npos ? "summary" : rest.substr(slash + 1);

  const auto metrics = source.metrics(symbol);
  if (!metrics) return error(404, "no data for " + symbol + " yet");

  if (view == "summary") return ok(summary_json(*metrics));
  if (view == "chain") {
    const auto it = query.find("expiry");
    const SliceMetrics* slice = nullptr;
    for (const SliceMetrics& s : metrics->slices) {
      if (it == query.end() || expiry_id(s) == it->second) {
        slice = &s;
        break;
      }
    }
    if (!slice) return error(404, "no expiry " + (it == query.end() ? "" : it->second));
    return ok(chain_json(*metrics, *slice, window));
  }
  if (view == "exposure")
    return ok(exposure_json(*metrics, expiries, query.contains("window") ? window : 0.08));
  if (view == "surface")
    return ok(surface_json(*metrics, expiries, query.contains("window") ? window : 0.2));
  return error(404, "unknown view " + std::string(view));
}

std::string tick_message(const MetricsSource& source) {
  const EngineStatus s = source.status();
  return json{{"type", "tick"},
              {"market", market_json(md::now())},
              {"feed", {{"state", md::to_string(s.feed_state)}, {"message", s.feed_message}}},
              {"underlyings", underlyings_json(source, s, false)},
              {"engine",
               {{"events_per_second", sig(s.events_per_second, 4)},
                {"analytics_ms", sig(s.analytics_ms, 4)},
                {"queue_depth", s.queue_depth},
                {"coalesced_events", s.coalesced_events},
                {"dropped_events", s.dropped_events},
                {"overloaded", s.overloaded},
                {"contracts", s.contracts},
                {"nonstandard_contracts", s.nonstandard_contracts}}}}
      .dump();
}

}  // namespace openport::server
