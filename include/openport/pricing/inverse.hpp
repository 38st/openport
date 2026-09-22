#pragma once

#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"

/// Coin-settled ("inverse") options, as listed on Deribit.
///
/// A BTC option pays max(S - K, 0) / S *bitcoin* at expiry: the dollar payoff of a
/// vanilla call, converted to coin at the settlement price. Its premium is quoted
/// in coin, and Deribit marks it as the Black-76 value on the forward (zero rates
/// on both legs) divided by that forward. The dollar value is therefore an
/// ordinary vanilla, and all of the usual pricing machinery applies unchanged.
namespace openport::pricing::inverse {

/// Premium in coin.
[[nodiscard]] double coin_price(OptionType type, double forward, double strike, double expiry,
                                double vol) noexcept;

/// Implied volatility from a premium quoted in coin.
[[nodiscard]] IvResult implied_vol(double coin_premium, OptionType type, double forward,
                                   double strike, double expiry,
                                   const IvOptions& options = {}) noexcept;

/// Delta of the coin-denominated position, measured in coin.
///
/// Buying the option costs coin, and that coin had delta one, so the premium
/// comes off the Black delta: delta_coin = delta - premium. FX desks call this
/// the premium-adjusted delta. Deribit's per-option Greeks report plain Black
/// delta; the adjustment matters for an account that keeps its P&L in coin.
[[nodiscard]] double premium_adjusted_delta(OptionType type, double forward, double strike,
                                            double expiry, double vol) noexcept;

}  // namespace openport::pricing::inverse
