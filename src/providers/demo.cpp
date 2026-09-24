#include "openport/providers/demo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/md/recording.hpp"
#include "openport/pricing/black.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {
namespace {

constexpr md::Timestamp kStep = 15 * md::kNanosPerSecond;
constexpr double kRate = 0.04;
constexpr double kDividend = 0.013;
constexpr double kOpen = 6000.0;     // SPX at the open
constexpr double kSpyRatio = 10.02;  // index points per SPY dollar

/// splitmix64 with Box-Muller: the same draws on every standard library, which
/// <random>'s distributions do not promise.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  /// Uniform in (0, 1).
  double uniform() { return (static_cast<double>(next() >> 11) + 0.5) * 0x1.0p-53; }
  double normal() {
    const double radius = std::sqrt(-2 * std::log(uniform()));
    return radius * std::cos(2 * std::numbers::pi * uniform());
  }

 private:
  std::uint64_t state_;
};

/// A draw in (0, 1) that depends only on its arguments, not on the order of calls.
double draw(std::uint64_t seed, std::uint64_t a, std::uint64_t b) {
  Random random(seed ^ (a * 0xD6E8FEB86659FD93ull) ^ (b * 0xA0761D6478BD642Full));
  random.next();
  return random.uniform();
}

double cents(double value) { return std::round(value * 100) / 100; }

md::Date next_business_day(md::Date date) {
  // After 17:00 the trading date is the next business day.
  return md::trading_date(md::new_york_to_utc(date, 18, 0));
}
bool business_day(md::Date date) { return md::trading_date(md::new_york_to_utc(date, 12, 0)) == date; }
md::Date friday_from(md::Date date) {
  auto days = md::days_since_epoch(date);
  while (md::weekday(md::date_from_days(days)) != 5) ++days;
  return md::date_from_days(days);
}
/// A Friday expiry moves to the business day before when the exchange is closed.
md::Date expiry_on(md::Date friday) { return business_day(friday) ? friday : md::previous_business_day(friday); }

struct Series {
  std::string root;
  md::Date expiry;
  double step;    // strike spacing
  double window;  // strikes within this fraction of the open
  bool monthly;
};

std::vector<md::OptionContract> list(const Series& series, double center) {
  std::vector<md::OptionContract> contracts;
  const double low = std::ceil(center * (1 - series.window) / series.step) * series.step;
  const double high = std::floor(center * (1 + series.window) / series.step) * series.step;
  for (double strike = low; strike <= high + 1e-9; strike += series.step) {
    for (const char right : {'C', 'P'}) {
      char osi[32];
      std::snprintf(osi, sizeof osi, "%s%02d%02d%02d%c%08lld", series.root.c_str(), series.expiry.year % 100,
                    series.expiry.month, series.expiry.day, right, std::llround(strike * 1000));
      if (const auto contract = md::parse_osi(osi)) contracts.push_back(*contract);
    }
  }
  return contracts;
}

/// At-the-money implied volatility: a little higher for the shortest expiries and
/// next month, and higher as the index falls (`spot_vol` per unit of return).
double atm_vol(double days, double ratio, double spot_vol, double noise) {
  return 0.132 + 0.018 * std::exp(-days / 1.5) + 0.012 * std::min(days / 30, 1.0) + spot_vol * (ratio - 1) + noise;
}
/// The skew in standardised moneyness: puts richer, calls cheaper, wings up.
double smile(double atm, double forward, double strike, double years) {
  const double z = std::log(strike / forward) / (atm * std::sqrt(std::max(years, 1e-7)));
  return atm * std::clamp(1 - 0.18 * z + 0.03 * z * z, 0.6, 3.0);
}

struct Quote {
  double bid = 0;
  double ask = 0;
};
/// Quotes around the model price on the product's ticks (trading::tick_size):
/// SPY and QQQ in pennies, SPX in nickels below $3 and dimes from $3. Far
/// out-of-the-money series show no bid.
Quote quote(bool penny, double mid) {
  const auto tick = [penny](double price) { return penny ? 0.01 : price < 3 ? 0.05 : 0.10; };
  const double width = penny ? std::max(0.01, 0.01 * mid + 0.01) : std::max(0.05, 0.02 * mid + 0.05);
  const double low = mid - width / 2;
  const double high = mid + width / 2;
  Quote q;
  q.bid = low > 0 ? std::floor(low / tick(low) + 1e-9) * tick(low) : 0;
  q.ask = std::ceil(high / tick(high) - 1e-9) * tick(high);
  q.ask = std::max({q.ask, q.bid + tick(q.bid), tick(0)});
  // The ask's tick follows its own price: 2.95 + a nickel reaches 3.00, a dime tick.
  q.ask = std::ceil(q.ask / tick(q.ask) - 1e-9) * tick(q.ask);
  return {cents(q.bid), cents(q.ask)};
}

double open_interest(const md::OptionContract& c, bool monthly, double center, double days, std::uint64_t seed,
                     md::InstrumentId id) {
  const bool etf = c.root != "SPX" && c.root != "SPXW";
  const double x = std::log(c.strike / center);
  const double width = 0.012 + 0.01 * std::sqrt(days);
  double oi = (etf ? 6000.0 : 2500.0) * std::exp(-0.5 * (x / width) * (x / width)) * (monthly ? 3 : 1);
  if (std::fmod(c.strike, etf ? 5 : 25) == 0) oi *= 1.8;
  if (std::fmod(c.strike, etf ? 10 : 100) == 0) oi *= 1.6;
  if (c.type == pricing::OptionType::Put ? c.strike < center : c.strike > center) oi *= 1.3;
  return std::round(oi * (0.6 + 0.8 * draw(seed, id, 0x0A11)));
}

class Discard final : public md::EventSink {
 public:
  void publish(md::Event) override {}
};

struct Listed {
  md::OptionContract contract;
  md::InstrumentId id = 0;
  Quote last;
  double bid_size = 0;
  double ask_size = 0;
};

/// How a day goes: drift in fractions of its session, each (until, total change),
/// realised volatility annualised over trading time, and implied volatility's shift
/// and response to the index.
struct Script {
  DemoDay day;
  const char* id;
  const char* title;
  const char* description;
  md::Date date;
  std::uint64_t seed;
  std::vector<std::pair<double, double>> drift;
  double volatility;
  double iv_shift;
  double spot_vol;
  bool overnight = false;
};
const std::vector<Script>& scripts() {
  static const std::vector<Script> all{
      {DemoDay::Reversal, "reversal", "Slide and rebound", "SPX slides about 1% into late morning, then rallies into the close.",
       {2026, 9, 16}, 20260916, {{0.1, -0.003}, {0.3, -0.009}, {0.5, 0.0}, {0.8, 0.012}, {1.0, 0.002}}, 0.13, 0.0, -2.0},
      {DemoDay::Trend, "trend", "Trend", "A steady climb of about 1% while implied volatility eases.",
       {2026, 9, 15}, 20260915, {{0.2, 0.003}, {0.5, 0.004}, {0.8, 0.003}, {1.0, 0.002}}, 0.09, -0.015, -1.5},
      {DemoDay::Chop, "chop", "Chop", "A tight range that goes nowhere, with quiet volatility.",
       {2026, 9, 14}, 20260914, {{0.25, 0.003}, {0.5, -0.004}, {0.75, 0.003}, {1.0, -0.002}}, 0.08, -0.01, -1.5},
      {DemoDay::Selloff, "selloff", "Selloff", "SPX falls almost 3% as volatility climbs, and a midday bounce fails.",
       {2026, 9, 17}, 20260917, {{0.15, -0.006}, {0.35, -0.012}, {0.5, 0.004}, {0.8, -0.010}, {1.0, -0.004}}, 0.24, 0.03, -2.5},
      {DemoDay::Overnight, "overnight", "Overnight session",
       "SPX options in Cboe's global trading hours, 8:15 pm to 9:25 am ET: limit orders only, with the index frozen at its close.",
       {2026, 9, 16}, 2026091600, {{0.3, -0.002}, {0.6, -0.004}, {1.0, 0.003}}, 0.06, 0.0, -2.0, true},
  };
  return all;
}
const Script& script_for(DemoDay day) {
  for (const auto& s : scripts())
    if (s.day == day) return s;
  return scripts().front();
}

/// When the day's snapshots run: the regular session to the 16:15 last trade,
/// or the overnight session before the date's open.
struct Window {
  md::Timestamp first;
  md::Timestamp close;  // regular days: the 16:00 index close
  md::Timestamp last;
  md::Timestamp step;
};
Window window_for(const Script& script, md::Date date) {
  if (script.overnight) {
    const auto evening = md::date_from_days(md::days_since_epoch(date) - 1);
    const auto end = md::new_york_to_utc(date, 9, 25);
    return {md::new_york_to_utc(evening, 20, 15), end, end, 60 * md::kNanosPerSecond};
  }
  const int hour = md::regular_close_hour(date);
  return {md::new_york_to_utc(date, 9, 30), md::new_york_to_utc(date, hour, 0), md::new_york_to_utc(date, hour, 15), kStep};
}

constexpr double kQqqOpen = 480.0;
constexpr double kQqqBeta = 1.25;  // QQQ's moves against SPX's

}  // namespace

bool simulated_provider(std::string_view name) noexcept {
  return name == kDemoProvider || name == "replay (demo)";
}

const std::vector<DemoInfo>& demo_days() {
  static const std::vector<DemoInfo> days = [] {
    std::vector<DemoInfo> out;
    for (const auto& s : scripts()) {
      std::vector<std::string> symbols{"SPX"};
      if (!s.overnight) symbols = {"SPX", "SPY", "QQQ"};
      out.push_back({s.day, s.id, s.title, s.description, s.date, symbols, window_for(s, s.date).first});
    }
    return out;
  }();
  return days;
}

const DemoInfo* find_demo_day(std::string_view id) {
  for (const auto& d : demo_days())
    if (d.id == id) return &d;
  return nullptr;
}

void write_demo_recording(const std::filesystem::path& path, const DemoOptions& options) {
  const auto& script = script_for(options.day);
  const auto date = options.date.value_or(script.date);
  const auto seed = options.seed != 0 ? options.seed : script.seed;
  if (!md::valid_date(date) || !business_day(date)) throw std::invalid_argument("The demo day must be a trading day");
  const auto w = window_for(script, date);

  // Five expiries a chain: today, the next two business days, the Friday after,
  // and next month's third Friday (AM-settled for SPX).
  const auto d1 = next_business_day(date);
  const auto d2 = next_business_day(d1);
  const auto weekly = expiry_on(friday_from(md::date_from_days(md::days_since_epoch(d2) + 1)));
  const md::Date month{date.month == 12 ? date.year + 1 : date.year, date.month == 12 ? 1 : date.month + 1, 1};
  const auto monthly = expiry_on(md::date_from_days(md::days_since_epoch(friday_from(month)) + 14));
  const double spy_open = kOpen / kSpyRatio;
  std::vector<std::pair<Series, double>> series;
  const auto add = [&](const std::string& root, md::Date expiry, double step, double window, bool is_monthly) {
    const bool index = root == "SPX" || root == "SPXW";
    if (script.overnight && !index) return;  // only SPX options trade overnight
    series.push_back({{root, expiry, step, window, is_monthly}, index ? kOpen : root == "SPY" ? spy_open : kQqqOpen});
  };
  for (const auto& expiry : {date, d1, d2}) {
    add("SPXW", expiry, 5, 0.03, false);
    add("SPY", expiry, 1, 0.03, false);
    add("QQQ", expiry, 1, 0.03, false);
  }
  add("SPXW", weekly, 5, 0.04, false);
  add("SPY", weekly, 1, 0.04, false);
  add("QQQ", weekly, 1, 0.04, false);
  add("SPX", monthly, 10, 0.06, true);
  add("SPY", monthly, 1, 0.06, true);
  add("QQQ", monthly, 1, 0.06, true);

  md::RecordingHeader header;
  header.provider = std::string(kDemoProvider);
  header.capabilities.poll_interval = std::chrono::seconds(w.step / md::kNanosPerSecond);
  header.capabilities.open_interest = true;
  header.subscription.underlyings = script.overnight ? std::vector<std::string>{"SPX"} : std::vector<std::string>{"SPX", "SPY", "QQQ"};
  header.started = w.first;
  md::Timestamp now = w.first;
  md::RecordingSink::Options sink_options;
  sink_options.clock = [&now] { return now; };
  Discard discard;
  md::RecordingSink sink(path, header, discard, sink_options);

  SnapshotPublisher publisher;
  std::map<std::string, std::vector<Listed>> chains;
  for (const auto& [s, center] : series) {
    for (auto& contract : list(s, center)) {
      Listed listed;
      listed.id = publisher.define(contract.osi_symbol(), contract, sink);
      listed.contract = std::move(contract);
      const double days = md::years_between(w.first, listed.contract.expiry_time()) * 365;
      publisher.open_interest(listed.id, w.first, open_interest(listed.contract, s.monthly, center, days, seed, listed.id), sink);
      chains[listed.contract.underlying].push_back(std::move(listed));
    }
  }
  // Overnight the index keeps its last close, printed once; its options follow futures.
  if (script.overnight)
    sink.publish(md::UnderlyingQuote{"SPX", md::new_york_to_utc(md::previous_business_day(date), 16, 0), 0, 0, kOpen});

  const double session = static_cast<double>(w.close - w.first);
  const double hours = script.overnight ? 13.0 : 6.5;
  const double step_vol = script.volatility * std::sqrt(static_cast<double>(w.step) / (hours * 3600 * md::kNanosPerSecond) / 252);
  Random random(seed);
  // The index wanders around its script and back (about a 35-minute half-life), so
  // every day keeps its shape whatever the draws.
  const double revert = std::exp(-static_cast<double>(w.step) / (50.0 * 60 * md::kNanosPerSecond));
  double deviation = 0;
  double log_level = std::log(kOpen);
  double idio = 0;   // QQQ's own wander
  double noise = 0;  // implied volatility's own wander
  double close_level = kOpen;
  for (std::int64_t step = 0; w.first + step * w.step <= w.last; ++step) {
    now = w.first + step * w.step;
    const bool after_close = !script.overnight && now > w.close;
    const double progress = std::min(1.0, static_cast<double>(now - w.first) / session);
    if (step > 0) {
      // Busier at the open and into the close; after the close only SPY and QQQ trade, quietly.
      const double pace = script.overnight ? 1.0
          : after_close ? 0.3 : 0.8 + 0.8 * std::exp(-progress / 0.06) + 0.5 * std::exp(-(1 - progress) / 0.08);
      deviation = revert * deviation + step_vol * pace * random.normal();
      idio = 0.995 * idio + 0.3 * step_vol * random.normal();
      noise = 0.98 * noise + 0.0008 * random.normal();
    }
    // The script: each segment's change spread evenly over its part of the session.
    double scripted = 0;
    double start = 0;
    for (const auto& [until, change] : script.drift) {
      scripted += change * std::clamp((progress - start) / (until - start), 0.0, 1.0);
      start = until;
    }
    log_level = std::log(kOpen) + scripted + deviation;
    const double level = std::exp(log_level);
    if (!after_close) close_level = level;
    const double ratio = level / kOpen;
    const double spy = level / kSpyRatio;
    const double qqq = kQqqOpen * std::exp(kQqqBeta * (log_level - std::log(kOpen)) + idio);
    if (!script.overnight) {
      // The index prints until the close; SPY and QQQ trade on after it.
      if (!after_close) sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, cents(level)});
      for (const auto& [symbol, price] : {std::pair<const char*, double>{"SPY", spy}, {"QQQ", qqq}}) {
        const double bid = std::floor(price * 100) / 100;
        sink.publish(md::UnderlyingQuote{symbol, now, bid, cents(bid + 0.01), cents(price)});
      }
    }
    for (auto& [underlying, chain] : chains) {
      const bool index = underlying == "SPX";
      // SPX options price off the index, which stops at the close (overnight they follow the latent level).
      const double price = index ? (script.overnight ? level : close_level) : underlying == "SPY" ? spy : qqq;
      const double moved = index ? (script.overnight ? ratio : close_level / kOpen) : underlying == "SPY" ? ratio : qqq / kQqqOpen;
      const double extra = underlying == "QQQ" ? 0.03 : underlying == "SPY" ? 0.005 : 0.0;
      std::set<md::InstrumentId> seen;
      for (auto& listed : chain) {
        const auto& c = listed.contract;
        if (now >= c.expiry_time()) continue;
        const double years = md::years_between(now, c.expiry_time());
        const double forward = price * std::exp((kRate - kDividend) * years);
        const double atm = atm_vol(years * 365, moved, script.spot_vol, noise) + script.iv_shift + extra;
        const double mid = pricing::black_price(c.type, forward, c.strike, years, smile(atm, forward, c.strike, years),
                                                std::exp(-kRate * years));
        const auto q = quote(!index, mid);
        if (q.bid != listed.last.bid || q.ask != listed.last.ask || listed.ask_size == 0) {
          listed.last = q;
          const double scale = index ? 120 : 800;
          listed.bid_size = q.bid > 0 ? std::floor((index ? 5 : 20) + scale * draw(seed, listed.id, 2 * step)) : 0;
          listed.ask_size = std::floor((index ? 5 : 20) + scale * draw(seed, listed.id, 2 * step + 1));
        }
        publisher.quote(listed.id, now, q.bid, q.ask, listed.bid_size, listed.ask_size, sink);
        seen.insert(listed.id);
      }
      publisher.finish(underlying, seen, now, sink);
      sink.publish(md::ProviderStatus{now, md::FeedState::Live,
                                      "demo " + underlying + ": " + std::to_string(seen.size()) + " simulated options", underlying});
    }
  }
  sink.close();
  if (const auto error = sink.error(); !error.empty()) throw std::runtime_error("Demo recording failed: " + error);
}

}  // namespace openport::providers
