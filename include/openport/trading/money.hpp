#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace openport::trading {

/// Signed micro-dollars. All arithmetic is checked; invalid decimals throw
/// TradingError(INVALID_MONEY), overflow throws TradingError(ARITHMETIC_OVERFLOW).
class Money {
 public:
  constexpr Money() = default;
  static constexpr Money from_micros(std::int64_t value) { return Money(value); }
  /// Exact decimal input, optional sign, 0-6 fractional digits, no exponent/space.
  static Money parse(std::string_view text);
  /// Adapter boundary only: nearest micro-dollar, ties away from zero.
  static Money from_double(double dollars);
  [[nodiscard]] constexpr std::int64_t micros() const { return value_; }
  [[nodiscard]] double dollars() const;
  /// At least two fractional digits, with further significant digits preserved.
  [[nodiscard]] std::string str() const;
  friend constexpr auto operator<=>(Money, Money) = default;
  friend Money operator+(Money, Money);
  friend Money operator-(Money, Money);
  friend Money operator-(Money);
  friend Money operator*(Money, std::int64_t);
  /// Exact wide intermediate; rounds nearest, ties away, preserving close residue.
  [[nodiscard]] Money prorate(std::int64_t numerator, std::int64_t denominator) const;

 private:
  explicit constexpr Money(std::int64_t value) : value_(value) {}
  std::int64_t value_ = 0;
};

}  // namespace openport::trading
