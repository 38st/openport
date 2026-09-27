#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/time.hpp"

namespace openport::providers::broker {
using Json = nlohmann::json;
inline constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline Json parse(std::string_view text) {
  // Parser diagnostics can quote input, including credentials in an auth response.
  auto value = Json::parse(text, nullptr, false);
  if (value.is_discarded() || !value.is_object()) throw std::runtime_error("invalid broker JSON");
  return value;
}

inline const Json& field(const Json& object, std::string_view key) {
  static const Json empty;
  if (!object.is_object()) return empty;
  const auto it = object.find(std::string(key));
  return it == object.end() ? empty : *it;
}

inline std::string str(const Json& value) {
  return value.is_string() ? value.get<std::string>() : std::string{};
}

inline double number(const Json& value, double fallback = missing) {
  double result = fallback;
  if (value.is_number()) result = value.get<double>();
  else if (value.is_string()) {
    const auto text = value.get<std::string>();
    // Decimal strings occur in tastytrade instruments. Reject suffixes and infinities.
    try {
      std::size_t used = 0;
      result = std::stod(text, &used);
      if (used != text.size()) return fallback;
    } catch (...) { return fallback; }
  }
  return std::isfinite(result) ? result : fallback;
}

inline std::vector<Json> list(const Json& value) {
  if (value.is_null()) return {};
  if (value.is_array()) return value.get<std::vector<Json>>();
  return {value};
}

inline md::Date date(const Json& value) {
  const auto text = str(value);
  const auto ts = text.size() == 10 ? md::parse_datetime(text + "T00:00:00Z", md::Zone::Utc)
                                  : std::nullopt;
  if (!ts) throw std::runtime_error("invalid broker expiration date");
  return md::date_from_days(*ts / md::kNanosPerDay);
}

inline md::Timestamp milliseconds(const Json& value) {
  // Never guess that a ten-digit value means seconds; conflicting examples exist.
  const double n = number(value);
  constexpr auto max = std::numeric_limits<md::Timestamp>::max() / 1'000'000;
  if (!std::isfinite(n) || n < 1'000'000'000'000.0 || n > static_cast<double>(max) ||
      n != std::floor(n)) return 0;
  return static_cast<md::Timestamp>(n) * 1'000'000;
}

inline std::string symbol_path(std::string_view symbol) {
  if (symbol.empty() || symbol.size() > 32) throw std::runtime_error("invalid broker symbol");
  std::string out;
  constexpr char hex[] = "0123456789ABCDEF";
  for (const char character : symbol) {
    const auto c = static_cast<unsigned char>(character);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
}  // namespace openport::providers::broker
