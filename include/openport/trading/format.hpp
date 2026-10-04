#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace openport::trading {

/// Display-only dollar delta: signed whole dollars with thousands separators.
inline std::string format_dollar_delta(double value) {
  if (!std::isfinite(value)) return "unavailable";
  std::array<char, std::numeric_limits<double>::max_exponent10 + 3> text{};
  const auto result = std::to_chars(text.data(), text.data() + text.size(),
      std::round(std::abs(value)), std::chars_format::fixed, 0);
  std::string digits(text.data(), result.ptr);
  for (auto i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast<std::size_t>(i), ",");
  return (value < 0 ? "-$" : "+$") + digits;
}

}  // namespace openport::trading
