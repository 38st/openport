#pragma once

#include <algorithm>
#include <string>

#include "openport/analytics/chain_book.hpp"
#include "openport/pricing/binomial.hpp"

namespace openport::test {
// Deterministic American quotes; shared by the regression tests and timing probe.
inline void add_american_expiry(analytics::ChainBook& book, md::Timestamp as_of, md::Date expiry,
                                md::InstrumentId& id, double spot = 100, double low = 80,
                                double spacing = 2.5, int strikes = 17, int steps = 1001,
                                const std::string& symbol = "SPY") {
  book.apply(md::UnderlyingQuote{symbol, as_of, spot, spot, spot});
  for (int i = 0; i < strikes; ++i) {
    for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
      auto c = *md::parse_osi("SPY261218C00100000");
      c.root = c.underlying = symbol;
      c.expiry = expiry;
      c.strike = low + spacing * i;
      c.type = type;
      book.apply(md::ContractDefinition{id, c});
      const double t = md::years_between(as_of, c.expiry_time());
      const double price = pricing::binomial_price({type, spot, c.strike, t, .045, .01, .20},
                                                   pricing::ExerciseStyle::American,
                                                   pricing::TreeMethod::LeisenReimer, steps);
      const double spread = std::min(.001 * spot, price * .01);
      book.apply(md::OptionQuote{id, as_of, price - spread, price + spread, 1, 1});
      book.apply(md::OpenInterest{id, as_of, type == pricing::OptionType::Call ? 100.0 : 120.0});
      ++id;
    }
  }
}
}  // namespace openport::test
