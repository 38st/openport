#pragma once

#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace openport::providers {

inline int parse_integer(std::string_view text, std::string_view name, int minimum = 0,
                         int maximum = std::numeric_limits<int>::max()) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < minimum ||
      value > maximum)
    throw std::invalid_argument(std::string(name) + " must be an integer in [" +
                                std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
  return value;
}

inline double parse_fraction(std::string_view text, std::string_view name) {
  double value = 0.0;
  std::istringstream input{std::string(text)};
  input.imbue(std::locale::classic());
  input >> std::noskipws >> value;
  if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value) ||
      value < 0.0 || value > 1.0)
    throw std::invalid_argument(std::string(name) + " must be a finite number in [0, 1]");
  return value;
}

}  // namespace openport::providers
