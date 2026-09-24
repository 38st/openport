#pragma once

#include <istream>
#include <string>
#include <vector>

#include "openport/md/time.hpp"
#include "openport/trading/money.hpp"

namespace openport::trading {

/// A cash dividend on an underlying's shares, paid on its ex-date to the shares
/// held into it; short shares pay it.
struct Dividend {
  std::string symbol;  ///< The underlying, e.g. "SPY".
  md::Date ex_date;
  Money per_share;
};

/// Reads "SYMBOL,YYYY-MM-DD,AMOUNT" lines, the amount in dollars a share. Blank
/// lines, "#" comments and a header starting "symbol," are skipped. Throws
/// std::invalid_argument naming the line for anything else, and for a symbol
/// listed twice on one date.
[[nodiscard]] std::vector<Dividend> parse_dividends(std::istream& in);

/// The dividends that go ex after `after` and on or before `through`: those a day
/// rollover from `after` to `through` pays, including any the server was down for.
[[nodiscard]] std::vector<Dividend> dividends_due(const std::vector<Dividend>& all, md::Date after, md::Date through);

}  // namespace openport::trading
