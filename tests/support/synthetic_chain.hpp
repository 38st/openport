#pragma once

#include <cmath>
#include <cstdio>

#include "openport/analytics/chain_book.hpp"
#include "openport/pricing/black.hpp"

namespace openport::test {

/// A complete SPXW expiry priced from known inputs (spot 5000, r 4.5%, q 1.3%, a
/// skewed smile), quoted a few cents either side of fair value, with call open
/// interest above spot and put open interest below it.
struct SyntheticChain {
  static constexpr double kSpot = 5000.0;
  static constexpr double kRate = 0.045;
  static constexpr double kDividend = 0.013;

  static double smile(double strike, double forward) {
    const double k = std::log(strike / forward);
    return 0.18 - 0.25 * k + 0.9 * k * k;
  }

  analytics::ChainBook book;
  md::Timestamp as_of = md::new_york_to_utc(md::Date{2026, 9, 22}, 15, 0);
  double years = 0.0;
  double forward = 0.0;
  double discount = 0.0;

  SyntheticChain() {
    const md::OptionContract probe = *md::parse_osi("SPXW261022C05000000");
    years = md::years_between(as_of, probe.expiry_time());
    discount = std::exp(-kRate * years);
    forward = kSpot * std::exp((kRate - kDividend) * years);

    book.apply(md::UnderlyingQuote{"SPX", as_of, kSpot - 0.5, kSpot + 0.5, kSpot});
    md::InstrumentId id = 0;
    for (double k = 4500.0; k <= 5500.0; k += 25.0) {
      for (pricing::OptionType type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
        char symbol[32];
        std::snprintf(symbol, sizeof symbol, "SPXW261022%c%08lld",
                      type == pricing::OptionType::Call ? 'C' : 'P', static_cast<long long>(k * 1000));
        book.apply(md::ContractDefinition{id, *md::parse_osi(symbol)});
        const double fair = pricing::black_price(type, forward, k, years, smile(k, forward), discount);
        book.apply(md::OptionQuote{id, as_of, fair - 0.05, fair + 0.05, 10, 10});
        const bool above = k > kSpot;
        const double oi = (type == pricing::OptionType::Call) == above ? 1000.0 : 100.0;
        book.apply(md::OpenInterest{id, as_of, oi});
        ++id;
      }
    }
  }
};

}  // namespace openport::test
