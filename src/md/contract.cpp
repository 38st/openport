#include "openport/md/contract.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace openport::md {
namespace {

using pricing::ExerciseStyle;

struct IndexRoot {
  std::string_view root;
  std::string_view underlying;
  Settlement settlement;
};

// Cash-settled, European-style index option roots.
constexpr std::array<IndexRoot, 13> kIndexRoots{{
    {"SPX", "SPX", Settlement::AM},   {"SPXW", "SPX", Settlement::PM},
    {"XSP", "XSP", Settlement::PM},
    {"NDX", "NDX", Settlement::AM},   {"NDXP", "NDX", Settlement::PM},
    {"XND", "XND", Settlement::PM},   {"RUT", "RUT", Settlement::AM},
    {"RUTW", "RUT", Settlement::PM},  {"MRUT", "MRUT", Settlement::PM},
    {"VIX", "VIX", Settlement::AM},   {"VIXW", "VIX", Settlement::AM},
    {"DJX", "DJX", Settlement::AM},   {"XEO", "OEX", Settlement::AM},
}};

}  // namespace

bool is_index_underlying(std::string_view underlying) {
  for (const IndexRoot& index : kIndexRoots) {
    if (index.underlying == underlying) return true;
  }
  return false;
}

std::vector<std::string> option_roots(std::string_view underlying) {
  std::vector<std::string> roots;
  for (const IndexRoot& index : kIndexRoots) {
    if (index.underlying == underlying) roots.emplace_back(index.root);
  }
  if (roots.empty()) roots.emplace_back(underlying);
  return roots;
}

RootConventions conventions_for_root(std::string_view root) {
  for (const IndexRoot& index : kIndexRoots) {
    if (index.root == root) {
      return {std::string(index.underlying), ExerciseStyle::European, index.settlement};
    }
  }
  // Equity and ETF options; adjusted contracts carry a trailing digit ("SPY1").
  std::string_view underlying = root;
  while (underlying.size() > 1 && std::isdigit(static_cast<unsigned char>(underlying.back()))) {
    underlying.remove_suffix(1);
  }
  return {std::string(underlying), ExerciseStyle::American, Settlement::PM};
}

std::string OptionContract::osi_symbol() const {
  char buffer[32];
  const auto strike_thousandths = static_cast<long long>(std::llround(strike * 1000.0));
  std::snprintf(buffer, sizeof buffer, "%-6s%02d%02d%02d%c%08lld", root.c_str(), expiry.year % 100,
                expiry.month, expiry.day, type == pricing::OptionType::Call ? 'C' : 'P',
                strike_thousandths);
  return buffer;
}

Timestamp OptionContract::expiry_time() const noexcept {
  return settlement == Settlement::AM ? new_york_to_utc(expiry, 9, 30)
                                      : new_york_to_utc(expiry, 16, 0);
}

std::optional<OptionContract> parse_osi(std::string_view symbol) {
  // The last 15 characters are always YYMMDD + C/P + 8-digit strike.
  if (symbol.size() < 16) return std::nullopt;
  const std::string_view tail = symbol.substr(symbol.size() - 15);
  std::string_view root = symbol.substr(0, symbol.size() - 15);
  while (!root.empty() && root.back() == ' ') root.remove_suffix(1);
  if (root.empty() || root.size() > 6) return std::nullopt;
  for (char c : root) {
    if (!std::isalnum(static_cast<unsigned char>(c))) return std::nullopt;
  }

  auto digits = [&](std::size_t pos, std::size_t len, long long& out) {
    out = 0;
    for (std::size_t i = pos; i < pos + len; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(tail[i]))) return false;
      out = out * 10 + (tail[i] - '0');
    }
    return true;
  };
  long long yy = 0, mm = 0, dd = 0, strike = 0;
  if (!digits(0, 2, yy) || !digits(2, 2, mm) || !digits(4, 2, dd) || !digits(7, 8, strike)) {
    return std::nullopt;
  }
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31) return std::nullopt;
  const char right = tail[6];
  if (right != 'C' && right != 'P') return std::nullopt;

  OptionContract contract;
  contract.root = std::string(root);
  const RootConventions conventions = conventions_for_root(root);
  contract.underlying = conventions.underlying;
  contract.style = conventions.style;
  contract.settlement = conventions.settlement;
  contract.expiry = Date{2000 + static_cast<int>(yy), static_cast<int>(mm), static_cast<int>(dd)};
  contract.strike = static_cast<double>(strike) / 1000.0;
  contract.type = right == 'C' ? pricing::OptionType::Call : pricing::OptionType::Put;
  return contract;
}

}  // namespace openport::md
