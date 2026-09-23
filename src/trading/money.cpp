#include "openport/trading/money.hpp"

#include <cmath>
#include <limits>

#include "openport/trading/types.hpp"

namespace openport::trading {
namespace {
__extension__ using Wide = __int128;
Money checked(Wide value) {
  if (value < std::numeric_limits<std::int64_t>::min() ||
      value > std::numeric_limits<std::int64_t>::max())
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Micro-dollar arithmetic overflow");
  return Money::from_micros(static_cast<std::int64_t>(value));
}
}  // namespace

Money Money::parse(std::string_view text) {
  auto invalid = [] { throw TradingError(Reason::INVALID_MONEY, "Expected an exact decimal with at most six fractional digits"); };
  if (text.empty()) invalid();
  bool negative = false;
  if (text.front() == '-' || text.front() == '+') {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty()) invalid();
  Wide units = 0;
  std::size_t i = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    units = units * 10 + (text[i] - '0');
    if (units > static_cast<Wide>(std::numeric_limits<std::int64_t>::max()) / 1'000'000 + 1)
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Decimal exceeds micro-dollar range");
  }
  if (i == 0) invalid();
  int digits = 0;
  Wide fraction = 0;
  if (i < text.size()) {
    if (text[i++] != '.' || i == text.size()) invalid();
    for (; i < text.size(); ++i) {
      if (text[i] < '0' || text[i] > '9' || ++digits > 6) invalid();
      fraction = fraction * 10 + (text[i] - '0');
    }
  }
  while (digits++ < 6) fraction *= 10;
  const Wide value = units * 1'000'000 + fraction;
  return checked(negative ? -value : value);
}
Money Money::from_double(double dollars) {
  if (!std::isfinite(dollars)) throw TradingError(Reason::INVALID_MONEY, "Nonfinite price");
  const long double scaled = std::round(static_cast<long double>(dollars) * 1'000'000.0L);
  // The upper bound is exclusive even when long double aliases double.
  if (scaled < -9223372036854775808.0L || scaled >= 9223372036854775808.0L)
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Floating price exceeds micro-dollar range");
  return from_micros(static_cast<std::int64_t>(scaled));
}
double Money::dollars() const { return static_cast<double>(value_) / 1'000'000.0; }
std::string Money::str() const {
  const auto magnitude = value_ < 0 ? static_cast<std::uint64_t>(-(value_ + 1)) + 1
                                    : static_cast<std::uint64_t>(value_);
  std::string fractional = std::to_string(1'000'000 + magnitude % 1'000'000).substr(1);
  while (fractional.size() > 2 && fractional.back() == '0') fractional.pop_back();
  return (value_ < 0 ? "-" : "") + std::to_string(magnitude / 1'000'000) + "." + fractional;
}
Money operator+(Money a, Money b) { return checked(static_cast<Wide>(a.micros()) + b.micros()); }
Money operator-(Money a, Money b) { return checked(static_cast<Wide>(a.micros()) - b.micros()); }
Money operator-(Money a) { return checked(-static_cast<Wide>(a.micros())); }
Money operator*(Money a, std::int64_t b) { return checked(static_cast<Wide>(a.micros()) * b); }
Money Money::prorate(std::int64_t numerator, std::int64_t denominator) const {
  if (denominator <= 0) throw TradingError(Reason::INVALID_MONEY, "Denominator must be positive");
  Wide value = static_cast<Wide>(value_) * numerator;
  const bool negative = value < 0;
  if (negative) value = -value;
  const Wide result = (value + denominator / 2) / denominator;
  return checked(negative ? -result : result);
}
}  // namespace openport::trading
