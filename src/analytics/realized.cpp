#include "openport/analytics/realized.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace openport::analytics {
namespace {
bool positive(double value) { return std::isfinite(value) && value > 0; }
double sample_variance(std::span<const double> values) {
  if (values.size() < 2) return kNaN;
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double sum = 0;
  for (double value : values) sum += (value - mean) * (value - mean);
  return sum / static_cast<double>(values.size() - 1);
}
double annual_vol(double variance) {
  return std::isfinite(variance) && variance >= -1e-14 ? 100 * std::sqrt(std::max(0.0, variance) * kSessionsPerYear) : kNaN;
}
/// True when no session is missing between two daily bars. The md calendar knows NYSE
/// holidays from 2022; before that a holiday would look like a missing session, so
/// earlier history (Cboe's goes back decades) accepts a gap of up to a week, enough for
/// any holiday weekend or unscheduled closure.
bool adjacent(md::Date earlier, md::Date later) {
  constexpr int kFirstCalendarYear = 2022;
  if (earlier.year >= kFirstCalendarYear) return md::previous_business_day(later) == earlier;
  const auto gap = md::days_since_epoch(later) - md::days_since_epoch(earlier);
  return gap >= 1 && gap <= 7;
}
bool adjacent(const md::Bar& earlier, const md::Bar& later) {
  return adjacent(md::new_york_time(earlier.start).date, md::new_york_time(later.start).date);
}
bool consecutive(std::span<const md::Bar> bars) {
  for (std::size_t i = 1; i < bars.size(); ++i)
    if (!adjacent(bars[i - 1], bars[i])) return false;
  return true;
}
}  // namespace

RealizedWindow realized_window(std::span<const md::Bar> bars, int sessions) {
  RealizedWindow out;
  out.sessions = sessions;
  auto missing = [&](const std::string& reason) {
    out.close_to_close.reason = out.parkinson.reason = out.garman_klass.reason = out.yang_zhang.reason = reason;
  };
  if (sessions < 2 || bars.size() < static_cast<std::size_t>(sessions)) {
    missing("insufficient_history"); return out;
  }
  const auto window = bars.last(static_cast<std::size_t>(sessions));
  if (!consecutive(window)) { missing("missing_sessions"); return out; }
  const bool has_previous = bars.size() > static_cast<std::size_t>(sessions);
  const auto closes = has_previous ? bars.last(static_cast<std::size_t>(sessions) + 1) : window;
  std::vector<double> returns, overnight, open_close;
  if (!has_previous) out.close_to_close.reason = "insufficient_history";
  else if (!consecutive(closes)) out.close_to_close.reason = "missing_sessions";
  else {
    for (std::size_t i = 1; i < closes.size(); ++i) {
      if (!positive(closes[i].close) || !positive(closes[i - 1].close)) {
        out.close_to_close.reason = "invalid_close";
        break;
      }
      returns.push_back(std::log(closes[i].close / closes[i - 1].close));
    }
    if (out.close_to_close.reason.empty()) out.close_to_close.vol = annual_vol(sample_variance(returns));
  }
  double parkinson = 0, gk = 0, rs = 0;
  bool valid_ohlc = true;
  for (std::size_t i = 0; i < window.size(); ++i) {
    const auto& bar = window[i];
    if (!md::valid_bar(bar)) { valid_ohlc = false; continue; }
    const double hl = std::log(bar.high / bar.low), co = std::log(bar.close / bar.open);
    parkinson += hl * hl / (4 * std::log(2.0));
    gk += 0.5 * hl * hl - (2 * std::log(2.0) - 1) * co * co;
    rs += std::log(bar.high / bar.open) * std::log(bar.high / bar.close)
        + std::log(bar.low / bar.open) * std::log(bar.low / bar.close);
    if (has_previous && out.close_to_close.reason.empty())
      overnight.push_back(std::log(bar.open / closes[i].close));
    open_close.push_back(co);
  }
  if (!valid_ohlc) {
    for (auto* estimate : {&out.parkinson, &out.garman_klass, &out.yang_zhang}) {
      estimate->vol = out.close_to_close.vol;
      estimate->fallback = true;
      estimate->reason = std::isfinite(estimate->vol) ? "invalid_ohlc_used_close_to_close" : "invalid_ohlc_and_missing_close_to_close";
    }
    return out;
  }
  const double n = sessions;
  const double k = 0.34 / (1.34 + (n + 1) / (n - 1));
  out.parkinson.vol = annual_vol(parkinson / n);
  out.garman_klass.vol = annual_vol(gk / n);
  if (out.close_to_close.reason.empty())
    out.yang_zhang.vol = annual_vol(sample_variance(overnight) + k * sample_variance(open_close) + (1 - k) * rs / n);
  else out.yang_zhang.reason = out.close_to_close.reason;
  return out;
}

double percentile(std::span<const double> values, double probability) {
  std::vector<double> sorted;
  for (double value : values) if (std::isfinite(value)) sorted.push_back(value);
  if (sorted.empty() || !std::isfinite(probability) || probability < 0 || probability > 1) return kNaN;
  std::sort(sorted.begin(), sorted.end());
  const double index = probability * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(index));
  const auto upper = static_cast<std::size_t>(std::ceil(index));
  return std::lerp(sorted[lower], sorted[upper], index - static_cast<double>(lower));
}

VolCone vol_cone(std::span<const md::Bar> bars, int sessions) {
  VolCone out;
  out.sessions = sessions;
  if (sessions < 2) return out;
  std::vector<double> values;
  std::vector<double> returns(bars.size(), kNaN);
  std::vector<std::size_t> invalid(bars.size() + 1, 0);
  std::vector<bool> used(bars.size(), false);
  // Calendar validation is independent of the rolling window. Do it once per
  // return, then use prefix counts to reject any window spanning a missing day.
  for (std::size_t i = 0; i < bars.size(); ++i) {
    if (i > 0 && positive(bars[i].close) && positive(bars[i - 1].close) && adjacent(bars[i - 1], bars[i]))
      returns[i] = std::log(bars[i].close / bars[i - 1].close);
    invalid[i + 1] = invalid[i] + (std::isfinite(returns[i]) ? 0u : 1u);
  }
  const auto count = static_cast<std::size_t>(sessions);
  for (std::size_t i = count; i < bars.size(); ++i) {
    const auto begin = i - count + 1;
    if (invalid[i + 1] != invalid[begin]) continue;
    const double value = annual_vol(sample_variance(std::span<const double>(returns).subspan(begin, count)));
    if (std::isfinite(value)) {
      values.push_back(value);
      for (auto day = i - count; day <= i; ++day) used[day] = true;
      if (i + 1 == bars.size()) out.current = value;
    }
  }
  out.days_used = static_cast<std::size_t>(std::count(used.begin(), used.end(), true));
  out.observations = values.size();
  out.min = percentile(values, 0); out.p10 = percentile(values, 0.1);
  out.p25 = percentile(values, 0.25); out.p50 = percentile(values, 0.5);
  out.p75 = percentile(values, 0.75); out.p90 = percentile(values, 0.9); out.max = percentile(values, 1);
  if (!values.empty() && std::isfinite(out.current)) {
    double rank = 0;
    for (double value : values) rank += value < out.current ? 1 : value == out.current ? 0.5 : 0;
    out.current_percentile = 100 * rank / static_cast<double>(values.size());
  }
  return out;
}

std::vector<IntradayRealized> intraday_realized(std::span<const md::Bar> minutes, md::Timestamp as_of) {
  std::map<md::Date, std::map<md::Timestamp, md::Bar>> sessions;
  for (const auto& bar : minutes) {
    if (!md::valid_bar(bar) || bar.start + md::kNanosPerMinute > as_of) continue;
    if (!md::stock_session(bar.start).open) continue;
    sessions[md::new_york_time(bar.start).date][bar.start] = bar;
  }
  std::vector<IntradayRealized> out;
  for (const auto& [date, bars] : sessions) {
    IntradayRealized value;
    value.date = date;
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    value.session_minutes = static_cast<int>((close - open) / md::kNanosPerMinute);
    double squares = 0, previous_close = kNaN;
    md::Timestamp previous_end = 0;
    for (auto start = open; start + 5 * md::kNanosPerMinute <= std::min(as_of, close); start += 5 * md::kNanosPerMinute) {
      bool complete = true;
      for (int i = 0; i < 5; ++i)
        if (!bars.contains(start + i * md::kNanosPerMinute)) complete = false;
      if (!complete) continue;
      const auto& first = bars.at(start);
      const auto& last = bars.at(start + 4 * md::kNanosPerMinute);
      // After a gap, use the observed block's open; never bridge missing prices.
      const double base = previous_end == start ? previous_close : first.open;
      const double ret = std::log(last.close / base);
      squares += ret * ret;
      ++value.returns;
      previous_close = last.close;
      previous_end = start + 5 * md::kNanosPerMinute;
    }
    value.observed_minutes = value.returns * 5;
    value.partial = value.observed_minutes < value.session_minutes;
    if (value.returns > 0)
      value.vol = annual_vol(squares * value.session_minutes / value.observed_minutes);
    else value.reason = "no_complete_five_minute_blocks";
    out.push_back(value);
  }
  if (out.size() > 10) out.erase(out.begin(), out.end() - 10);
  return out;
}

RealizedMetrics realized_metrics(std::span<const md::Bar> days, std::span<const md::Bar> minutes,
                                 md::Timestamp as_of) {
  std::map<md::Timestamp, md::Bar> complete;
  for (const auto& bar : days) {
    const auto date = md::new_york_time(bar.start).date;
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (close <= as_of && md::market_session(open).open) complete[open] = bar;
  }
  std::vector<md::Bar> history;
  for (const auto& [time, bar] : complete) history.push_back(bar);
  RealizedMetrics out;
  if (!history.empty()) out.daily_as_of = md::format_date(md::new_york_time(history.back().start).date);
  for (const int window : kRealizedWindows) {
    out.windows.push_back(realized_window(history, window));
    out.cones.push_back(vol_cone(history, window));
  }
  out.intraday = intraday_realized(minutes, as_of);
  for (const auto& value : out.intraday)
    if (value.date == md::new_york_time(as_of).date) out.today = value.vol;
  return out;
}
}  // namespace openport::analytics
