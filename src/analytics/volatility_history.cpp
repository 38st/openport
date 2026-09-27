#include "openport/analytics/volatility_history.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace openport::analytics {
namespace {
bool completed(md::Date date, md::Timestamp as_of) {
  const auto open = md::new_york_to_utc(date, 9, 30);
  return md::market_session(open).open && md::new_york_to_utc(date, md::regular_close_hour(date), 0) <= as_of;
}
bool usable(double value) { return std::isfinite(value) && value >= 0; }
}
std::string iv_proxy(std::string_view symbol) {
  if (symbol == "SPX" || symbol == "SPY") return "VIX";
  if (symbol == "QQQ") return "VXN";
  if (symbol == "IWM") return "RVX";
  if (symbol == "DIA") return "VXD";
  return {};
}
std::vector<HistoricalIv> historical_iv(std::span<const VolatilityDay> own,
    std::span<const md::Bar> proxy, std::string_view proxy_name, md::Timestamp as_of, bool atm_fallback) {
  std::map<md::Date, HistoricalIv> values;
  auto first = md::Date{9999, 1, 1};
  for (const auto& row : own) {
    first = std::min(first, row.date);
    if (!completed(row.date, as_of)) continue;
    const bool mfiv = usable(row.mfiv);
    const double value = mfiv ? row.mfiv : atm_fallback ? row.atm : kNaN;
    if (usable(value)) values[row.date] = {row.date, value, mfiv ? "own_mfiv" : "own_atm"};
  }
  for (const auto& bar : proxy) {
    const auto date = md::new_york_time(bar.start).date;
    if (!proxy_name.empty() && date < first && completed(date, as_of) && bar.close > 0 && std::isfinite(bar.close))
      values[date] = {date, bar.close, std::string(proxy_name)};
  }
  std::vector<HistoricalIv> out;
  for (const auto& [date, value] : values) { (void)date; out.push_back(value); }
  return out;
}
IvRank iv_rank(std::span<const HistoricalIv> history, double current, md::Timestamp as_of) {
  IvRank out;
  auto last = md::new_york_time(as_of).date;
  if (!completed(last, as_of)) last = md::previous_business_day(last);
  auto first = last;
  for (int i = 1; i < 252; ++i) first = md::previous_business_day(first);
  std::vector<double> values;
  std::size_t below = 0;
  for (const auto& row : history) {
    if (row.date < first || row.date > last || !usable(row.vol)) continue;
    values.push_back(row.vol);
    if (row.vol < current) ++below;
    if (row.source.starts_with("own_")) ++out.own_sessions;
    else {
      ++out.proxy_sessions;
      if (out.proxy_start.empty()) out.proxy_start = md::format_date(row.date);
    }
  }
  out.sessions = values.size();
  if (values.empty() || !usable(current)) return out;
  const auto [low, high] = std::minmax_element(values.begin(), values.end());
  if (*high > *low) out.rank = (current - *low) / (*high - *low);
  out.percentile = static_cast<double>(below) / static_cast<double>(values.size());
  return out;
}
ExPostVrp ex_post_vrp(std::span<const HistoricalIv> history, std::span<const md::Bar> days, md::Timestamp as_of) {
  ExPostVrp out;
  std::map<md::Date, std::size_t> index;
  for (std::size_t i = 0; i < days.size(); ++i) index[md::new_york_time(days[i].start).date] = i;
  for (const auto& iv : history) {
    const auto found = index.find(iv.date);
    if (found == index.end() || found->second + 21 >= days.size() || !usable(iv.vol)) continue;
    const auto end = md::new_york_time(days[found->second + 21].start).date;
    if (!completed(end, as_of)) continue;
    const auto rv = realized_window(days.subspan(found->second, 22), 21).close_to_close.vol;
    if (!usable(rv)) continue;
    out.points.push_back({iv.date, end, iv.vol, rv, (iv.vol * iv.vol - rv * rv) / 10000,
                          iv.vol - rv, iv.source});
  }
  const auto today = md::new_york_time(as_of).date;
  for (int years : {1, 3, 10}) {
    ExPostSummary summary;
    summary.years = years;
    auto from = today;
    from.year -= years;
    if (!md::valid_date(from)) from.day = 28;
    std::vector<double> variance, vol;
    std::size_t positive = 0;
    for (const auto& point : out.points) {
      if (point.date < from) continue;
      variance.push_back(point.variance); vol.push_back(point.vol_points);
      if (point.variance > 0) ++positive;
      if (!point.source.starts_with("own_")) ++summary.proxy_observations;
      if (summary.from.empty()) summary.from = md::format_date(point.date);
      summary.to = md::format_date(point.date);
    }
    summary.observations = variance.size();
    if (!variance.empty()) {
      const auto n = static_cast<double>(variance.size());
      summary.mean_variance = std::accumulate(variance.begin(), variance.end(), 0.0) / n;
      summary.median_variance = percentile(variance, 0.5);
      summary.mean_vol_points = std::accumulate(vol.begin(), vol.end(), 0.0) / n;
      summary.median_vol_points = percentile(vol, 0.5);
      summary.positive_share = static_cast<double>(positive) / n;
    }
    out.summaries.push_back(summary);
  }
  return out;
}
}  // namespace openport::analytics
