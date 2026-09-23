// Reproducible timing/convergence probe; see docs/american-analytics.md.
#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

#include "../tests/support/american_chain.hpp"
#include "openport/analytics/chain_analytics.hpp"

using namespace openport;

void accuracy() {
  for (int n : {15, 21, 31, 51, 71, 101}) {
    double largest = 0, otm = 0;
    for (double t : {.25, 1.0})
      for (double k = 80; k <= 120; k += 2.5)
        for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
          const pricing::BsmInputs in{type, 100, k, t, .045, .01, .2};
          const double error = std::abs(pricing::binomial_early_exercise_premium(in, n) -
                                        pricing::binomial_early_exercise_premium(in, 2001));
          largest = std::max(largest, error);
          if ((type == pricing::OptionType::Call) == (k >= 100 * std::exp(.035 * t)))
            otm = std::max(otm, error);
        }
    std::printf("steps=%d max_EEP_error=%.8f max_OTM_EEP_error=%.8f\n", n, largest, otm);
  }
  for (double t : {.25, 1.0}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    const md::Date expiry{2027, 9, 22};
    const auto as_of =
        md::new_york_to_utc(expiry, 16, 0) - static_cast<md::Timestamp>(t * md::kNanosPerYear);
    test::add_american_expiry(book, as_of, expiry, id);
    analytics::AnalyticsOptions options;
    options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
    for (bool corrected : {false, true}) {
      options.deamericanize = corrected;
      const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
      const auto& s = m.slices[0];
      double vol_error = 0;
      for (const auto& row : s.strikes) vol_error = std::max(vol_error, std::abs(row.iv - .2));
      std::printf("T=%.2f corrected=%d forward_error_pct=%.6f max_OTM_IV_error_vp=%.6f\n", t,
                  corrected, 100 * (s.forward.forward / (100 * std::exp(.035 * t)) - 1),
                  100 * vol_error);
    }
  }
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--accuracy") {
    accuracy();
    return 0;
  }
  analytics::ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  md::InstrumentId id = 0;
  for (int i = 0; i < 31; ++i) {
    const auto date = md::date_from_days(md::days_since_epoch({2026, 9, 22}) + 1 + i * i);
    test::add_american_expiry(book, as_of, date, id, 600, 120, 5, 194, 101);
  }
  analytics::AnalyticsOptions options;
  options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
  options.deamericanize = !(argc > 1 && std::string_view(argv[1]) == "--no-deamericanize");
  std::vector<double> timings;
  int priced = 0;
  for (int i = 0; i < 55; ++i) {
    const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    priced = m.options_priced;
    if (i >= 5) timings.push_back(m.compute_ms);
  }
  std::sort(timings.begin(), timings.end());
  std::printf("options=%u expiries=31 priced=%d corrected=%d steps=%d median_ms=%.3f p90_ms=%.3f\n",
              id, priced, options.deamericanize, analytics::kDeamericanizationSteps, timings[25],
              timings[45]);
}
