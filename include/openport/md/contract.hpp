#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "openport/md/time.hpp"
#include "openport/pricing/binomial.hpp"
#include "openport/pricing/option.hpp"

namespace openport::md {

/// When a contract's settlement value is fixed: on the opening print of expiry day
/// (AM, e.g. monthly SPX) or on the close (PM, e.g. SPXW and equity options).
enum class Settlement : std::uint8_t { AM, PM };

/// A listed option, identified the way the Options Clearing Corporation does.
struct OptionContract {
  std::string root;        ///< OCC root, e.g. "SPXW"
  std::string underlying;  ///< e.g. "SPX"
  Date expiry;
  double strike = 0.0;
  pricing::OptionType type = pricing::OptionType::Call;
  pricing::ExerciseStyle style = pricing::ExerciseStyle::American;
  Settlement settlement = Settlement::PM;
  double multiplier = 100.0;

  /// 21-character OSI symbol: root padded to six, YYMMDD, C or P, then the strike
  /// times 1000 in eight digits. For example "SPXW  261005P07405000".
  [[nodiscard]] std::string osi_symbol() const;

  /// The instant time value runs out: 09:30 New York for AM-settled contracts,
  /// 16:00 for PM-settled ones.
  [[nodiscard]] Timestamp expiry_time() const noexcept;
};

/// Exercise and settlement conventions implied by an OCC root.
struct RootConventions {
  std::string underlying;
  pricing::ExerciseStyle style = pricing::ExerciseStyle::American;
  Settlement settlement = Settlement::PM;
};

/// Index roots (SPX, SPXW, NDX, RUT, VIX, ...) are European and cash-settled, with
/// their own AM/PM settlement. Every other root is treated as an American equity
/// option on the root without any adjustment digits ("SPY1" -> "SPY").
[[nodiscard]] RootConventions conventions_for_root(std::string_view root);

/// Parses an OSI symbol, padded ("SPXW  261005P07405000") or compact
/// ("SPXW261005P07405000"). Returns nullopt for anything malformed.
[[nodiscard]] std::optional<OptionContract> parse_osi(std::string_view symbol);

}  // namespace openport::md
