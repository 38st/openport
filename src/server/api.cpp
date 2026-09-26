#include "openport/server/api.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <type_traits>

#include "openport/analytics/svi.hpp"
#include "openport/analytics/ssvi.hpp"
#include "openport/providers/demo.hpp"
#include "paper_json.hpp"

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
  json dividends = json::array();
  for (const auto& d : slice.dividends)
    dividends.push_back({{"ex_date", md::format_date(d.ex_date)}, {"amount", d.amount}});
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
      {"rate_source", slice.rate_source},
      {"rate_curve_symbol",
       slice.rate_curve_symbol.empty() ? json(nullptr) : json(slice.rate_curve_symbol)},
      {"deamericanized", slice.deamericanized},
      {"dividends", std::move(dividends)},
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
      {"symbol", o.contract.osi_symbol()},
      {"bid_size", std::isfinite(o.bid_size) && o.bid_size >= 0 && o.bid_size < 9223372036854775808.0
          ? json(static_cast<std::int64_t>(std::floor(o.bid_size))) : json(nullptr)},
      {"ask_size", std::isfinite(o.ask_size) && o.ask_size >= 0 && o.ask_size < 9223372036854775808.0
          ? json(static_cast<std::int64_t>(std::floor(o.ask_size))) : json(nullptr)},
      {"tradable", trading::eligible(o.contract).ok()},
      {"untradable_reason", trading::eligible(o.contract).ok() ? json(nullptr) : json(trading::to_string(trading::eligible(o.contract).code))},
      {"bid", price(o.bid)},
      {"ask", price(o.ask)},
      {"mid", price(o.mid)},
      {"eep", sig(o.eep)},
      {"iv", sig(o.iv)},
      {"bid_iv", sig(o.bid_iv)},
      {"ask_iv", sig(o.ask_iv)},
      {"delta", sig(o.delta)},
      {"gamma", sig(o.gamma)},
      {"vega", sig(o.vega)},
      {"theta", sig(o.theta)},
      {"vanna", sig(o.vanna)},
      {"oi", o.has_open_interest && std::isfinite(o.open_interest) ? json(o.open_interest)
                                                                   : json(nullptr)},
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

// Strict decimal syntax before strtod: libc++ and libstdc++ streams disagree
// on underflow. ERANGE rejects both overflow and underflow, including subnormals.
bool decimal_number(const std::string& text, double& value) {
  std::size_t pos = 0;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
  auto digits = [&] {
    const auto first = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    return pos != first;
  };
  bool mantissa = digits();
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    mantissa = digits() || mantissa;
  }
  if (!mantissa) return false;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
    if (!digits()) return false;
  }
  if (pos != text.size()) return false;
  errno = 0;
  char* end = nullptr;
  value = std::strtod(text.c_str(), &end);
  return errno != ERANGE && end == text.c_str() + text.size() && std::isfinite(value);
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
    if (!decimal_number(text, value)) return false;
  }
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

ApiResponse ok(const json& body) {
  return {200, body.dump()};
}

ApiResponse error(int status, const std::string& message) {
  return api_error(status, status == 400 ? "INVALID_REQUEST" : status == 404 ? "NOT_FOUND" : "METHOD_NOT_ALLOWED", message);
}

bool in_window(double strike, double spot, double window) {
  // A hair of tolerance so strikes exactly on the edge (4900 of 5000 at 2%) stay in.
  return !(window > 0.0) || !(spot > 0.0) || std::abs(strike / spot - 1.0) <= window + 1e-12;
}

json underlyings_json(const MetricsSource& source, const EngineStatus& status, bool details,
                      md::Timestamp now) {
  json underlyings = json::array();
  const auto view = source.trading_view();
  const auto max_quote_age = view ? view->config.limits.max_quote_age : trading::Limits{}.max_quote_age;
  std::set<std::string> symbols;
  for (const auto& symbol : source.symbols()) symbols.insert(symbol);
  for (const auto& [symbol, health] : status.underlyings) symbols.insert(symbol);
  for (const std::string& symbol : symbols) {
    const auto session = md::trading_session(symbol, now);
    json item{
        {"symbol", symbol},
        {"spot", nullptr},
        {"as_of", nullptr},
        {"version", 0},
        {"has_tradable_contracts", false},
        {"session", {{"name", session.name}, {"open", session.open}, {"note", session.note}}}};
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
    auto market_time = m ? m->as_of : 0;
    if (view) {
      const auto time = view->market_times.find(symbol);
      market_time = time == view->market_times.end() ? 0 : time->second;
    }
    const auto paper = paper_acceptance(symbol, market_time, now, status.capabilities.delay, max_quote_age,
                                        view ? view->halts : std::vector<MarketHalt>{});
    // The session new orders enter: the market-data clock's, which a delayed
    // feed keeps behind the wall clock's `session`.
    item["paper"] = {{"accepting", paper.ok()},
                     {"reason", paper.ok() ? json(nullptr) : json(trading::to_string(paper.code))},
                     {"message", paper.ok() ? json(nullptr) : json(paper.message)},
                     {"session", market_time > 0 ? json(md::trading_session(symbol, market_time).name) : json(nullptr)}};
    if (m) {
      item["spot"] = price(m->spot);
      item["as_of"] = md::format_timestamp(m->as_of);
      item["version"] = m->version;
      item["has_tradable_contracts"] = std::any_of(m->slices.begin(), m->slices.end(), [](const auto& slice) {
        return slice.years > 0 && std::any_of(slice.strikes.begin(), slice.strikes.end(), [](const auto& row) {
          return (row.call.id != analytics::kNoInstrument && trading::eligible(row.call.contract).ok()) ||
                 (row.put.id != analytics::kNoInstrument && trading::eligible(row.put.contract).ok());
        });
      });
      if (details) {
        item["expiries"] = m->slices.size();
        item["options"] = m->options_priced;
      }
    }
    underlyings.push_back(std::move(item));
  }
  return underlyings;
}

json circuit_breaker_json(const CircuitBreakerStatus& breaker) {
  json halts = json::array();
  for (const auto& halt : breaker.halts)
    halts.push_back({{"level", halt.level}, {"start", md::format_timestamp(halt.start)},
                     {"end", md::format_timestamp(halt.end)}, {"reference", halt.reference}, {"price", halt.price},
                     {"active", halt.start <= breaker.market_time && breaker.market_time < halt.end}});
  const auto& close = breaker.previous_close;
  return {{"symbol", breaker.symbol}, {"day", breaker.market_time > 0 ? json(md::format_date(breaker.day)) : json(nullptr)},
          {"previous_close", close ? json{{"date", md::format_date(close->date)}, {"price", close->price}} : json(nullptr)},
          {"level", breaker.level}, {"halts", halts}, {"active", breaker.active},
          {"market_time", breaker.market_time > 0 ? json(md::format_timestamp(breaker.market_time)) : json(nullptr)},
          {"error", breaker.error.empty() ? json(nullptr) : json(breaker.error)}};
}

json status_json(const MetricsSource& source) {
  const EngineStatus s = source.status();
  const auto now = source.wall_time();
  return {
      {"trading", trading_status_json(s.trading)},
      {"accounts", account_ticks_json(s)},
      {"circuit_breaker", circuit_breaker_json(s.circuit_breaker)},
      {"market", market_json(now)},
      {"provider",
       {{"name", s.provider},
        {"simulated", providers::simulated_provider(s.provider)},
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
      {"underlyings", underlyings_json(source, s, true, now)},
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

// Weak ownership ties the cache to immutable snapshots, including source identity:
// separate engines with the same symbol/version cannot reuse each other's fits.
// A per-snapshot lock coalesces simultaneous HTTP requests, never the engine pass.
struct SurfaceFits {
  std::mutex mutex;
  std::vector<analytics::SviFit> fits;
  std::optional<analytics::SsviFit> ssvi;
};

std::shared_ptr<SurfaceFits> surface_cache(const std::shared_ptr<const UnderlyingMetrics>& m) {
  using Key = std::weak_ptr<const UnderlyingMetrics>;
  static std::mutex mutex;
  static std::map<Key, std::shared_ptr<SurfaceFits>, std::owner_less<Key>> cache;
  const std::lock_guard lock(mutex);
  std::erase_if(cache, [](const auto& entry) { return entry.first.expired(); });
  auto& entry = cache[Key(m)];
  if (!entry) entry = std::make_shared<SurfaceFits>();
  return entry;
}

json svi_json(const analytics::SviFit& fit) {
  if (fit.status != analytics::SviStatus::Ok) return nullptr;
  const auto& p = fit.parameters;
  // Full precision: rounding parameters independently can spoil a narrow smile.
  return {{"a", p.a}, {"b", p.b}, {"rho", p.rho}, {"m", p.m}, {"sigma", p.sigma},
          {"rmse_vol_points", sig(fit.rmse_vol_points)}, {"points", fit.points},
          {"status", analytics::to_string(fit.status)}, {"reason", nullptr},
          {"fit_ms", fit.fit_ms}, {"butterfly_min_g", sig(fit.butterfly.min_g)},
          {"butterfly_k", sig(fit.butterfly.k)}, {"butterfly_ok", fit.butterfly.ok}};
}

json surface_json(const std::shared_ptr<const UnderlyingMetrics>& metrics,
                  std::size_t max_expiries, double window) {
  const auto& m = *metrics;
  const auto count = std::min(max_expiries, m.slices.size());
  const auto cache = surface_cache(metrics);
  std::vector<analytics::SviFit> fits;
  analytics::SsviFit ssvi;
  {
    const std::lock_guard lock(cache->mutex);
    // Only newly requested expiries are fitted; display windows never change fits.
    while (cache->fits.size() < count)
      cache->fits.push_back(analytics::fit_svi(m.slices[cache->fits.size()]));
    if (!cache->ssvi) cache->ssvi = analytics::fit_ssvi(m.slices);
    ssvi = *cache->ssvi;
    fits.assign(cache->fits.begin(), cache->fits.begin() + static_cast<std::ptrdiff_t>(count));
  }
  json expiries = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    const SliceMetrics& slice = m.slices[i];
    const auto& fit = fits[i];
    const auto& surface_fit = ssvi.expiries[i];
    json points = json::array();
    for (const auto& row : slice.strikes) {
      if (!std::isfinite(row.iv) || !in_window(row.strike, m.spot, window)) continue;
      // Preserve the market display filter; calibration uses every usable two-sided
      // OTM point, with wide IV spreads downweighted rather than removed.
      const auto& otm = row.strike >= slice.forward.forward ? row.call : row.put;
      if (!(otm.bid > 0.0) || !(otm.ask - otm.bid <= 0.5 * otm.mid)) continue;
      const double k = std::log(row.strike / slice.forward.forward);
      points.push_back({{"strike", row.strike}, {"k", sig(k, 6)},
                        {"iv", sig(row.iv)}, {"bid_iv", sig(otm.bid_iv)},
                        {"ask_iv", sig(otm.ask_iv)},
                        {"svi_iv", fit.status == analytics::SviStatus::Ok
                            ? sig(analytics::svi_iv(fit.parameters, k, slice.years)) : json(nullptr)},
                        {"ssvi_iv", sig(analytics::ssvi_iv(ssvi.parameters, k, surface_fit.theta, slice.years))}});
    }
    expiries.push_back({{"id", expiry_id(slice)}, {"expiry", md::format_date(slice.expiry)},
                        {"days", sig(slice.years * 365.0, 4)},
                        {"forward", price(slice.forward.forward)},
                        {"atm_iv", sig(slice.atm_iv)}, {"points", points}, {"svi", svi_json(fit)},
                        {"ssvi_theta", sig(surface_fit.theta, 17)},
                        {"ssvi_rmse_vol_points", sig(surface_fit.rmse_vol_points)},
                        {"ssvi_reason", surface_fit.reason.empty() ? json(nullptr) : json(surface_fit.reason)},
                        {"ssvi_min_k", sig(surface_fit.min_k, 15)}, {"ssvi_max_k", sig(surface_fit.max_k, 15)},
                        {"svi_status", analytics::to_string(fit.status)},
                        {"svi_reason", fit.reason.empty() ? json(nullptr) : json(fit.reason)},
                        {"svi_points", fit.points}, {"svi_fit_ms", fit.fit_ms},
                        {"svi_years", sig(slice.years, 15)},
                        {"svi_min_k", sig(fit.min_k, 15)}, {"svi_max_k", sig(fit.max_k, 15)}});
  }
  json violations = json::array();
  for (const auto& pair : analytics::svi_calendar(fits))
    violations.push_back({{"earlier", expiry_id(m.slices[pair.earlier])},
                          {"later", expiry_id(m.slices[pair.later])}, {"k", sig(pair.k)},
                          {"vol_points", sig(pair.vol_points)},
                          {"tolerance_vol_points", sig(pair.tolerance_vol_points)}});
  return {{"symbol", m.symbol}, {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)}, {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version}, {"expiries", expiries}, {"calendar_violations", violations},
          {"ssvi", {{"rho", sig(ssvi.parameters.rho, 17)}, {"eta", sig(ssvi.parameters.eta, 17)},
                    {"gamma", sig(ssvi.parameters.gamma, 17)}, {"rmse_vol_points", sig(ssvi.rmse_vol_points)},
                    {"status", analytics::to_string(ssvi.status)},
                    {"reason", ssvi.reason.empty() ? json(nullptr) : json(ssvi.reason)},
                    {"monotone_adjusted", ssvi.monotone_adjusted}, {"fit_ms", ssvi.fit_ms}}}};
}

ApiResponse candles_response(const MetricsSource& source, const std::string& symbol,
                             const std::map<std::string, std::string>& query) {
  const auto symbols = source.symbols();
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
    return error(404, "no data for " + symbol + " yet");
  const auto requested = query.find("interval");
  const auto interval = parse_bar_interval(requested == query.end() ? "5m" : requested->second);
  if (!interval) return error(400, "interval must be 1m, 5m, 15m, 30m, 1h or 1d");
  int limit = 500;
  if (!bounded_number(query, "limit", 1, 5000, limit))
    return error(400, "limit must be an integer in [1, 5000]");
  json bars = json::array();
  if (const auto* store = source.candles()) {
    for (const md::Bar& bar : store->bars(symbol, *interval, static_cast<std::size_t>(limit)))
      bars.push_back({{"t", bar.start / md::kNanosPerSecond}, {"o", price(bar.open)},
                      {"h", price(bar.high)}, {"l", price(bar.low)}, {"c", price(bar.close)}});
  }
  return ok({{"symbol", symbol}, {"interval", std::string(to_string(*interval))},
             {"bars", std::move(bars)}});
}

}  // namespace

ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source) {
  if (request.method != "GET") return error(405, "only GET is supported");
  if (auto response = paper_read(request, source)) return *response;
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

  // History can arrive before the first analysis.
  if (view == "candles") return candles_response(source, symbol, query);
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
    return ok(surface_json(metrics, expiries, query.contains("window") ? window : 0.2));
  return error(404, "unknown view " + std::string(view));
}

std::string tick_message(const MetricsSource& source) {
  const EngineStatus s = source.status();
  const auto now = source.wall_time();
  return json{{"type", "tick"},
              {"trading", trading_status_json(s.trading)},
              {"accounts", account_ticks_json(s)},
              {"circuit_breaker", circuit_breaker_json(s.circuit_breaker)},
              {"market", market_json(now)},
              {"feed", {{"state", md::to_string(s.feed_state)}, {"message", s.feed_message}}},
              {"underlyings", underlyings_json(source, s, false, now)},
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
