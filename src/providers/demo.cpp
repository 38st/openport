#include "openport/providers/demo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
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
constexpr double kVolatility = 0.13;  // realised, annualised over trading time

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
/// next month, and higher as the index falls.
double atm_vol(double days, double level, double noise) {
  return 0.132 + 0.018 * std::exp(-days / 1.5) + 0.012 * std::min(days / 30, 1.0) - 2.0 * (level / kOpen - 1) + noise;
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
/// SPY in pennies, SPX in nickels below $3 and dimes from $3. Far out-of-the-money
/// series show no bid.
Quote quote(bool spy, double mid) {
  const auto tick = [spy](double price) { return spy ? 0.01 : price < 3 ? 0.05 : 0.10; };
  const double width = spy ? std::max(0.01, 0.01 * mid + 0.01) : std::max(0.05, 0.02 * mid + 0.05);
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
  const bool spy = c.root == "SPY";
  const double x = std::log(c.strike / center);
  const double width = 0.012 + 0.01 * std::sqrt(days);
  double oi = (spy ? 6000.0 : 2500.0) * std::exp(-0.5 * (x / width) * (x / width)) * (monthly ? 3 : 1);
  if (std::fmod(c.strike, spy ? 5 : 25) == 0) oi *= 1.8;
  if (std::fmod(c.strike, spy ? 10 : 100) == 0) oi *= 1.6;
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

}  // namespace

bool simulated_provider(std::string_view name) noexcept {
  return name == kDemoProvider || name == "replay (demo)";
}

void write_demo_recording(const std::filesystem::path& path, const DemoOptions& options) {
  const auto date = options.date;
  if (!md::valid_date(date) || !business_day(date)) throw std::invalid_argument("The demo day must be a trading day");
  const int close_hour = md::regular_close_hour(date);
  const auto opening = md::new_york_to_utc(date, 9, 30);
  const auto closing = md::new_york_to_utc(date, close_hour, 0);
  const auto last_trade = md::new_york_to_utc(date, close_hour, 15);

  // Five expiries a chain: today, the next two business days, the Friday after,
  // and next month's third Friday (AM-settled for SPX).
  const auto d1 = next_business_day(date);
  const auto d2 = next_business_day(d1);
  const auto weekly = expiry_on(friday_from(md::date_from_days(md::days_since_epoch(d2) + 1)));
  const md::Date month{date.month == 12 ? date.year + 1 : date.year, date.month == 12 ? 1 : date.month + 1, 1};
  const auto monthly = expiry_on(md::date_from_days(md::days_since_epoch(friday_from(month)) + 14));
  const double spy_open = kOpen / kSpyRatio;
  std::vector<std::pair<Series, double>> series;
  for (const auto& expiry : {date, d1, d2}) {
    series.push_back({{"SPXW", expiry, 5, 0.03, false}, kOpen});
    series.push_back({{"SPY", expiry, 1, 0.03, false}, spy_open});
  }
  series.push_back({{"SPXW", weekly, 5, 0.04, false}, kOpen});
  series.push_back({{"SPY", weekly, 1, 0.04, false}, spy_open});
  series.push_back({{"SPX", monthly, 10, 0.06, true}, kOpen});
  series.push_back({{"SPY", monthly, 1, 0.06, true}, spy_open});

  md::RecordingHeader header;
  header.provider = std::string(kDemoProvider);
  header.capabilities.poll_interval = std::chrono::seconds(15);
  header.capabilities.open_interest = true;
  header.subscription.underlyings = {"SPX", "SPY"};
  header.started = opening;
  md::Timestamp now = opening;
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
      const double days = md::years_between(opening, listed.contract.expiry_time()) * 365;
      publisher.open_interest(listed.id, opening,
                              open_interest(listed.contract, s.monthly, center, days, options.seed, listed.id), sink);
      chains[listed.contract.underlying].push_back(std::move(listed));
    }
  }

  // The index follows a scripted day, fractions of the session: a soft open, a
  // morning slide, a quiet midday, an afternoon rally and a flat close.
  constexpr std::array<std::pair<double, double>, 5> kDrift{{{0.1, -0.003}, {0.3, -0.009}, {0.5, 0.0}, {0.8, 0.012}, {1.0, 0.002}}};
  const double session = static_cast<double>(closing - opening);
  const double step_vol = kVolatility * std::sqrt(static_cast<double>(kStep) / (6.5 * 3600 * md::kNanosPerSecond) / 252);
  Random random(options.seed);
  double log_level = std::log(kOpen);
  double close_level = kOpen;
  double noise = 0;  // implied volatility's own wander
  for (std::int64_t step = 0; opening + step * kStep <= last_trade; ++step) {
    now = opening + step * kStep;
    const double progress = static_cast<double>(now - opening) / session;
    if (step > 0) {
      double drift = 0;
      double start = 0;
      for (const auto& [until, change] : kDrift) {
        if (progress <= until) {
          drift = change * static_cast<double>(kStep) / ((until - start) * session);
          break;
        }
        start = until;
      }
      // Busier at the open and into the close; after the close only SPY trades, quietly.
      const double pace = now > closing ? 0.3 : 0.8 + 0.8 * std::exp(-progress / 0.06) + 0.5 * std::exp(-(1 - progress) / 0.08);
      log_level += (now > closing ? 0 : drift) + step_vol * pace * random.normal();
      noise = 0.98 * noise + 0.0008 * random.normal();
    }
    const double level = std::exp(log_level);
    if (now <= closing) close_level = level;
    const double spy = level / kSpyRatio;
    // The index prints until the close; SPY trades on after it.
    if (now <= closing) sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, cents(level)});
    const double spy_bid = std::floor(spy * 100) / 100;
    sink.publish(md::UnderlyingQuote{"SPY", now, spy_bid, cents(spy_bid + 0.01), cents(spy)});
    for (auto& [underlying, chain] : chains) {
      const bool is_spy = underlying == "SPY";
      // SPX options price off the index, which stops at the close.
      const double price = is_spy ? spy : close_level;
      const double ratio = is_spy ? level / kOpen : close_level / kOpen;
      std::set<md::InstrumentId> seen;
      for (auto& listed : chain) {
        const auto& c = listed.contract;
        if (now >= c.expiry_time()) continue;
        const double years = md::years_between(now, c.expiry_time());
        const double forward = price * std::exp((kRate - kDividend) * years);
        const double atm = atm_vol(years * 365, kOpen * ratio, noise);
        const double mid = pricing::black_price(c.type, forward, c.strike, years, smile(atm, forward, c.strike, years),
                                                std::exp(-kRate * years));
        const auto q = quote(is_spy, mid);
        if (q.bid != listed.last.bid || q.ask != listed.last.ask || listed.ask_size == 0) {
          listed.last = q;
          const double scale = is_spy ? 800 : 120;
          listed.bid_size = q.bid > 0 ? std::floor((is_spy ? 20 : 5) + scale * draw(options.seed, listed.id, 2 * step)) : 0;
          listed.ask_size = std::floor((is_spy ? 20 : 5) + scale * draw(options.seed, listed.id, 2 * step + 1));
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
