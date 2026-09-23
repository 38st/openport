#include "openport/analytics/chain_analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"
#include "openport/pricing/normal.hpp"

namespace openport::analytics {
namespace {

using pricing::OptionType;

double implied(double price, OptionType type, double forward, double strike, double years,
               double discount) {
  if (!(price > 0.0)) return kNaN;
  const pricing::IvResult iv =
      pricing::implied_vol_black(price, type, forward, strike, years, discount);
  return iv.ok() ? iv.vol : kNaN;
}

/// Quotes and implied volatilities for one side; Greeks are filled in later, once
/// the strike's smile volatility is known.
OptionMetrics quote_metrics(const OptionState& s, md::InstrumentId id, double forward,
                            double discount, double years) {
  OptionMetrics m;
  m.id = id;
  m.bid = s.bid;
  m.ask = s.ask;
  m.open_interest = s.open_interest;
  if (s.vendor.iv > 0.0) m.vendor_iv = s.vendor.iv;
  const OptionType type = s.contract.type;
  const double strike = s.contract.strike;
  if (s.two_sided()) {
    m.mid = s.mid();
    m.iv = implied(m.mid, type, forward, strike, years, discount);
  }
  if (s.bid > 0.0) m.bid_iv = implied(s.bid, type, forward, strike, years, discount);
  if (s.ask > 0.0) m.ask_iv = implied(s.ask, type, forward, strike, years, discount);
  return m;
}

struct GreeksContext {
  double forward;
  double discount;
  double years;
  double spot;
  double carry;  ///< dF/dS = F / S
};

void fill_greeks(OptionMetrics& m, OptionType type, double strike, double vol,
                 const GreeksContext& ctx) {
  if (!(vol > 0.0)) return;
  const pricing::Greeks g =
      pricing::black_greeks(type, ctx.forward, strike, ctx.years, vol, ctx.discount);
  m.delta = g.delta * ctx.carry;
  m.gamma = g.gamma * ctx.carry * ctx.carry;
  m.vega = g.vega / 100.0;
  m.theta = g.theta / 365.0;
  m.vanna = g.vanna * ctx.carry / 100.0;
}

/// Linear interpolation of the smile at the forward.
double atm_vol(const std::vector<StrikeMetrics>& strikes, double forward) {
  const StrikeMetrics* below = nullptr;
  const StrikeMetrics* above = nullptr;
  for (const StrikeMetrics& s : strikes) {
    if (!std::isfinite(s.iv)) continue;
    if (s.strike <= forward) below = &s;
    if (s.strike >= forward && above == nullptr) above = &s;
  }
  if (below && above) {
    if (above->strike == below->strike) return below->iv;
    const double w = (forward - below->strike) / (above->strike - below->strike);
    return below->iv + w * (above->iv - below->iv);
  }
  return below ? below->iv : (above ? above->iv : kNaN);
}

/// Dollar gamma per 1% move of one position term, with gamma at a hypothetical spot.
struct GammaTerm {
  double weight;  ///< sign * open interest * multiplier
  double strike;
  double forward_per_spot;
  double years;
  double vol;
  double discount;
};

double total_gex_at(const std::vector<GammaTerm>& terms, double spot) {
  double total = 0.0;
  for (const GammaTerm& t : terms) {
    const double forward = t.forward_per_spot * spot;
    const double sd = t.vol * std::sqrt(t.years);
    const double d1 = std::log(forward / t.strike) / sd + 0.5 * sd;
    const double gamma_forward = t.discount * pricing::norm_pdf(d1) / (forward * sd);
    const double gamma_spot = gamma_forward * t.forward_per_spot * t.forward_per_spot;
    total += t.weight * gamma_spot * spot * spot * 0.01;
  }
  return total;
}

}  // namespace

UnderlyingMetrics analyze(const UnderlyingBook& book, const ChainBook& chain, md::Timestamp as_of,
                          const AnalyticsOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  UnderlyingMetrics out;
  out.symbol = book.symbol;
  out.as_of = as_of;
  out.version = book.version;
  out.spot = book.spot;

  // Pass 1: fit each live expiry's forward (and discount factor) from put-call parity.
  struct Fitted {
    const ExpirySlice* slice;
    double years;
    std::vector<ParityPoint> points;
    ForwardEstimate forward;
  };
  std::vector<Fitted> fitted;
  std::vector<double> long_rates;
  for (const auto& [key, slice] : book.expiries) {
    const double years = md::years_between(as_of, slice.expiry_time);
    if (!(years > 0.0)) continue;

    std::vector<ParityPoint> points;
    for (const auto& [strike, pair] : slice.strikes) {
      const OptionState* call = chain.option(pair.call);
      const OptionState* put = chain.option(pair.put);
      if (!call || !put || !call->two_sided() || !put->two_sided()) continue;
      const double spread = std::max(call->ask - call->bid + put->ask - put->bid, 1e-4);
      points.push_back({strike, call->mid(), put->mid(), 1.0 / (spread * spread)});
    }
    double reference = book.spot;
    if (!(reference > 0.0) && !points.empty()) {
      // No spot from the feed: the strike where calls and puts cost the same is near the money.
      reference = std::min_element(points.begin(), points.end(), [](const auto& a, const auto& b) {
                    return std::abs(a.call_mid - a.put_mid) < std::abs(b.call_mid - b.put_mid);
                  })->strike;
    }
    std::sort(points.begin(), points.end(), [reference](const auto& a, const auto& b) {
      return std::abs(a.strike - reference) < std::abs(b.strike - reference);
    });
    if (points.size() > static_cast<std::size_t>(options.parity_strikes)) {
      points.resize(static_cast<std::size_t>(options.parity_strikes));
    }
    ForwardEstimate forward = implied_forward(points, years, options.fallback_rate);
    if (forward.ok && forward.fitted_discount && years * 365.0 >= options.min_days_for_rate) {
      long_rates.push_back(-std::log(forward.discount) / years);
    }
    fitted.push_back({&slice, years, std::move(points), forward});
  }

  // Short expiries borrow the rate term structure's level from the long ones.
  double term_rate = options.fallback_rate;
  if (!long_rates.empty()) {
    std::nth_element(long_rates.begin(), long_rates.begin() + long_rates.size() / 2, long_rates.end());
    term_rate = long_rates[long_rates.size() / 2];
  }

  std::map<double, double> gex_by_strike;
  std::vector<GammaTerm> gamma_terms;

  // Pass 2: price every option against its expiry's forward.
  for (Fitted& f : fitted) {
    const ExpirySlice& slice = *f.slice;
    const double years = f.years;
    if (years * 365.0 < options.min_days_for_rate || !f.forward.fitted_discount) {
      f.forward = implied_forward_given_discount(f.points, std::exp(-term_rate * years));
    }

    SliceMetrics sm;
    sm.expiry = slice.expiry;
    sm.expiry_time = slice.expiry_time;
    sm.root = slice.root;
    sm.years = years;
    sm.forward = f.forward;
    if (!sm.forward.ok) {
      if (!(book.spot > 0.0)) continue;  // nothing to anchor this expiry to
      sm.forward.discount = std::exp(-term_rate * years);
      sm.forward.forward = book.spot / sm.forward.discount;
    }
    const double forward = sm.forward.forward;
    const double discount = sm.forward.discount;
    // Without a spot price from the feed, take spot as the discounted forward.
    const double spot = book.spot > 0.0 ? book.spot : forward * discount;
    if (!(out.spot > 0.0)) out.spot = spot;
    const GreeksContext ctx{forward, discount, years, spot, forward / spot};
    const double exposure_years = std::max(years, options.exposure_min_days / 365.0);
    const GreeksContext exposure_ctx{forward, discount, exposure_years, spot, forward / spot};

    sm.strikes.reserve(slice.strikes.size());
    for (const auto& [strike, pair] : slice.strikes) {
      StrikeMetrics row;
      row.strike = strike;
      const OptionState* call = chain.option(pair.call);
      const OptionState* put = chain.option(pair.put);
      if (call) row.call = quote_metrics(*call, pair.call, forward, discount, years);
      if (put) row.put = quote_metrics(*put, pair.put, forward, discount, years);

      const double otm = strike >= forward ? row.call.iv : row.put.iv;
      const double itm = strike >= forward ? row.put.iv : row.call.iv;
      row.iv = std::isfinite(otm) ? otm : itm;
      if (call) fill_greeks(row.call, OptionType::Call, strike, row.iv, ctx);
      if (put) fill_greeks(row.put, OptionType::Put, strike, row.iv, ctx);
      out.options_priced += (call ? 1 : 0) + (put ? 1 : 0);

      if (std::isfinite(row.iv)) {
        // Calls and puts share the smile vol, so they share gamma and vanna too.
        OptionMetrics exposure;
        fill_greeks(exposure, OptionType::Call, strike, row.iv, exposure_ctx);
        const double multiplier =
            call ? call->contract.multiplier : (put ? put->contract.multiplier : 100.0);
        const double net_oi = (row.call.open_interest - row.put.open_interest) * multiplier;
        row.gex = exposure.gamma * net_oi * spot * spot * 0.01;
        row.vex = exposure.vanna * net_oi * spot;
        if (net_oi != 0.0 && std::abs(strike / spot - 1.0) < 0.3) {
          gamma_terms.push_back({net_oi, strike, forward / spot, exposure_years, row.iv, discount});
        }
      }
      sm.gex += row.gex;
      sm.vex += row.vex;
      gex_by_strike[strike] += row.gex;
      sm.strikes.push_back(std::move(row));
    }
    sm.atm_iv = atm_vol(sm.strikes, forward);
    out.exposure.gex += sm.gex;
    out.exposure.vex += sm.vex;
    out.slices.push_back(std::move(sm));
  }

  // Walls: the strikes carrying the most positive and most negative GEX.
  double best_call = 0.0;
  double best_put = 0.0;
  for (const auto& [strike, gex] : gex_by_strike) {
    if (gex > best_call) {
      best_call = gex;
      out.exposure.call_wall = strike;
    }
    if (gex < best_put) {
      best_put = gex;
      out.exposure.put_wall = strike;
    }
  }

  // Gamma flip: re-evaluate total GEX at hypothetical spots (sticky-strike vols)
  // and find the sign change nearest the current spot.
  if (out.spot > 0.0 && !gamma_terms.empty() && options.flip_steps > 1) {
    double previous_spot = 0.0;
    double previous_gex = 0.0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < options.flip_steps; ++i) {
      const double x = -options.flip_range + 2.0 * options.flip_range * i / (options.flip_steps - 1);
      const double s = out.spot * (1.0 + x);
      const double gex = total_gex_at(gamma_terms, s);
      if (i > 0 && (previous_gex < 0.0) != (gex < 0.0)) {
        const double crossing =
            previous_spot + (s - previous_spot) * previous_gex / (previous_gex - gex);
        if (std::abs(crossing - out.spot) < best_distance) {
          best_distance = std::abs(crossing - out.spot);
          out.exposure.gamma_flip = crossing;
        }
      }
      previous_spot = s;
      previous_gex = gex;
    }
  }

  out.compute_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return out;
}

}  // namespace openport::analytics
