#include "openport/server/api.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace openport::server {
namespace {

using nlohmann::json;
using analytics::SliceMetrics;
using analytics::UnderlyingMetrics;

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
  return md::format_date(slice.expiry) + settlement_of(slice);
}

json expiry_json(const SliceMetrics& slice) {
  const double rate = -std::log(slice.forward.discount) / slice.years;
  return {
      {"id", expiry_id(slice)},
      {"expiry", md::format_date(slice.expiry)},
      {"settlement", settlement_of(slice)},
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
      {"bid", price(o.bid)},       {"ask", price(o.ask)},       {"mid", price(o.mid)},
      {"iv", sig(o.iv)},           {"bid_iv", sig(o.bid_iv)},   {"ask_iv", sig(o.ask_iv)},
      {"delta", sig(o.delta)},     {"gamma", sig(o.gamma)},     {"vega", sig(o.vega)},
      {"theta", sig(o.theta)},     {"vanna", sig(o.vanna)},     {"oi", o.open_interest},
      {"vendor_iv", sig(o.vendor_iv)},
  };
}

std::map<std::string, std::string> parse_query(std::string_view query) {
  std::map<std::string, std::string> out;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    const std::size_t eq = pair.find('=');
    out[std::string(pair.substr(0, eq))] =
        eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1));
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return out;
}

double number_or(const std::map<std::string, std::string>& query, const std::string& key,
                 double fallback) {
  const auto it = query.find(key);
  if (it == query.end()) return fallback;
  try {
    return std::stod(it->second);
  } catch (const std::exception&) {
    return fallback;
  }
}

ApiResponse ok(const json& body) { return {200, body.dump()}; }

ApiResponse error(int status, const std::string& message) {
  return {status, json{{"error", message}}.dump()};
}

bool in_window(double strike, double spot, double window) {
  // A hair of tolerance so strikes exactly on the edge (4900 of 5000 at 2%) stay in.
  return !(window > 0.0) || !(spot > 0.0) || std::abs(strike / spot - 1.0) <= window + 1e-12;
}

json status_json(const MetricsSource& source) {
  const EngineStatus s = source.status();
  json underlyings = json::array();
  for (const std::string& symbol : source.symbols()) {
    const auto m = source.metrics(symbol);
    if (!m) continue;
    underlyings.push_back({{"symbol", symbol},
                           {"spot", price(m->spot)},
                           {"as_of", md::format_timestamp(m->as_of)},
                           {"version", m->version},
                           {"expiries", m->slices.size()},
                           {"options", m->options_priced}});
  }
  return {
      {"provider",
       {{"name", s.provider},
        {"realtime", s.capabilities.realtime},
        {"delay_seconds", s.capabilities.delay.count()},
        {"trades", s.capabilities.trades},
        {"open_interest", s.capabilities.open_interest},
        {"vendor_greeks", s.capabilities.vendor_greeks}}},
      {"feed",
       {{"state", md::to_string(s.feed_state)},
        {"message", s.feed_message},
        {"updated", s.feed_updated > 0 ? json(md::format_timestamp(s.feed_updated)) : json(nullptr)}}},
      {"underlyings", underlyings},
      {"engine",
       {{"events", s.events},
        {"events_per_second", sig(s.events_per_second, 4)},
        {"analytics_ms", sig(s.analytics_ms, 4)},
        {"contracts", s.contracts},
        {"uptime_seconds", s.started > 0 ? (md::now() - s.started) / md::kNanosPerSecond : 0}}},
  };
}

json summary_json(const UnderlyingMetrics& m) {
  json expiries = json::array();
  for (const SliceMetrics& slice : m.slices) expiries.push_back(expiry_json(slice));
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"compute_ms", sig(m.compute_ms, 4)},
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
  return {{"symbol", m.symbol},       {"spot", price(m.spot)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},     {"strikes", strikes},
          {"expiries", expiries},     {"total_gex", total_json},
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
  const auto query = parse_query(question == std::string_view::npos ? std::string_view{}
                                                                    : target.substr(question + 1));

  if (path == "/api/status") return ok(status_json(source));

  constexpr std::string_view prefix = "/api/underlyings/";
  if (!path.starts_with(prefix)) return error(404, "unknown endpoint");
  std::string_view rest = path.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  const std::string symbol(rest.substr(0, slash));
  const std::string_view view = slash == std::string_view::npos ? "summary" : rest.substr(slash + 1);

  const auto metrics = source.metrics(symbol);
  if (!metrics) return error(404, "no data for " + symbol + " yet");
  const auto expiries = static_cast<std::size_t>(std::max(1.0, number_or(query, "expiries", 8)));

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
    return ok(chain_json(*metrics, *slice, number_or(query, "window", 0.0)));
  }
  if (view == "exposure") return ok(exposure_json(*metrics, expiries, number_or(query, "window", 0.08)));
  if (view == "surface") return ok(surface_json(*metrics, expiries, number_or(query, "window", 0.2)));
  return error(404, "unknown view " + std::string(view));
}

std::string tick_message(const MetricsSource& source) {
  const EngineStatus s = source.status();
  json underlyings = json::array();
  for (const std::string& symbol : source.symbols()) {
    const auto m = source.metrics(symbol);
    if (!m) continue;
    underlyings.push_back({{"symbol", symbol},
                           {"spot", price(m->spot)},
                           {"as_of", md::format_timestamp(m->as_of)},
                           {"version", m->version}});
  }
  return json{{"type", "tick"},
              {"feed", {{"state", md::to_string(s.feed_state)}, {"message", s.feed_message}}},
              {"underlyings", underlyings},
              {"engine",
               {{"events_per_second", sig(s.events_per_second, 4)},
                {"analytics_ms", sig(s.analytics_ms, 4)},
                {"contracts", s.contracts}}}}
      .dump();
}

}  // namespace openport::server
