#include "openport/trading/dividends.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "openport/trading/types.hpp"

namespace openport::trading {
namespace {
std::string trim(std::string text) {
  const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
  text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), space));
  text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(), text.end());
  return text;
}
}  // namespace

std::vector<Dividend> parse_dividends(std::istream& in) {
  std::vector<Dividend> out;
  std::set<std::pair<std::string, md::Date>> seen;
  std::string line;
  for (int number = 1; std::getline(in, line); ++number) {
    line = trim(line);
    if (line.empty() || line.front() == '#') continue;
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.starts_with("symbol,")) continue;
    const auto fail = [&](const std::string& why) {
      return std::invalid_argument("dividends line " + std::to_string(number) + ": " + why);
    };
    const auto first = line.find(',');
    const auto second = first == std::string::npos ? std::string::npos : line.find(',', first + 1);
    if (second == std::string::npos || line.find(',', second + 1) != std::string::npos)
      throw fail("expected SYMBOL,YYYY-MM-DD,AMOUNT");
    Dividend d;
    d.symbol = trim(line.substr(0, first));
    const auto date = trim(line.substr(first + 1, second - first - 1));
    const auto amount = trim(line.substr(second + 1));
    if (d.symbol.empty() || d.symbol.size() > 6 ||
        !std::all_of(d.symbol.begin(), d.symbol.end(), [](unsigned char c) { return std::isupper(c) || std::isdigit(c); }))
      throw fail("the symbol must be 1 to 6 capital letters or digits");
    const auto parsed = md::parse_datetime(date + " 00:00:00", md::Zone::Utc);
    if (date.size() != 10 || !parsed) throw fail("the ex-date must be YYYY-MM-DD");
    d.ex_date = md::date_from_days(*parsed / md::kNanosPerDay);
    try {
      d.per_share = Money::parse(amount);
    } catch (const TradingError&) {
      throw fail("the amount must be a decimal number of dollars a share");
    }
    if (d.per_share <= Money{}) throw fail("the amount must be positive");
    if (!seen.emplace(d.symbol, d.ex_date).second) throw fail(d.symbol + " is listed twice on " + date);
    out.push_back(std::move(d));
  }
  std::sort(out.begin(), out.end(), [](const Dividend& a, const Dividend& b) {
    return std::tie(a.ex_date, a.symbol) < std::tie(b.ex_date, b.symbol);
  });
  return out;
}

std::vector<Dividend> dividends_due(const std::vector<Dividend>& all, md::Date after, md::Date through) {
  std::vector<Dividend> out;
  for (const auto& d : all)
    if (d.ex_date > after && d.ex_date <= through) out.push_back(d);
  return out;
}

}  // namespace openport::trading
