#pragma once

#include "openport/pricing/black.hpp"
#include "openport/trading/session.hpp"

namespace openport::test {

/// Reusable deterministic quote/valuation fixture for core and future engine/API
/// integration. Times are market time; a new sample has an explicit new ID.
struct ScriptedMarket {
  md::OptionContract contract = *md::parse_osi("SPXW261022C05000000");
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 1;
  std::string symbol() const { return contract.osi_symbol(); }

  trading::QuoteObservation quote(std::string_view bid = "4.00", std::string_view ask = "4.20",
                                   trading::Quantity size = 10) const {
    return {symbol(), observation, time, trading::Money::parse(bid), trading::Money::parse(ask), size, size};
  }
  trading::Valuation valuation(double delta = 0.5, double vega = 2.0) const {
    return {symbol(), time, delta, 0.001, vega, -0.1, 5000, 5010, 0.99,
            md::years_between(time, contract.expiry_time()), 0.20, true};
  }
  trading::OrderRequest limit(std::string client, trading::Quantity quantity = 1,
      std::string_view price = "4.20", trading::Side side = trading::Side::Buy,
      trading::TimeInForce tif = trading::TimeInForce::Day) const {
    return {std::move(client), symbol(), side, trading::OrderType::Limit, tif, quantity, trading::Money::parse(price), {}, {}, {}};
  }
  trading::OrderRequest market(std::string client, trading::Quantity quantity = 1,
                              trading::Side side = trading::Side::Buy) const {
    return {std::move(client), symbol(), side, trading::OrderType::Market, trading::TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  }
  void next() { ++observation; time += md::kNanosPerSecond; }
  void seed(trading::TradingSession& session, std::string_view bid = "4.00",
            std::string_view ask = "4.20", trading::Quantity size = 10) const {
    session.define(contract, time);
    session.on_quotes({quote(bid, ask, size)}, {valuation()}, time);
  }
};

}  // namespace openport::test
