#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "openport/trading/evaluation.hpp"

namespace openport::trading {

/// One round trip on one contract, from flat to flat, or still open.
struct Lifecycle {
  std::string symbol;
  md::OptionContract contract;
  int direction = 1;                ///< +1 opened long, -1 opened short.
  Timestamp opened = 0;
  std::optional<Timestamp> closed;  ///< Absent while any quantity remains.
  Quantity quantity = 0;            ///< Current signed quantity; zero once closed.
  Quantity max_quantity = 0;        ///< Largest absolute quantity held.
  Quantity opened_contracts = 0;    ///< Contracts that opened or added.
  Quantity closed_contracts = 0;    ///< Contracts that reduced, including closures.
  Money open_notional;              ///< Sum of opening price * contracts, per unit.
  Money close_notional;             ///< Sum of closing price * contracts, per unit.
  Money gross;                      ///< Realised gross P&L so far, multiplier applied.
  Money fees;                       ///< Fees allocated to this lifecycle.
  std::vector<std::uint64_t> fills;
  std::optional<ClosureKind> closure;  ///< A settlement or reset ended the position.
  std::uint64_t first_fill = 0;     ///< ID of the opening fill, for attempt filtering.
};

/// One round trip in an underlying's shares, from flat to flat, or still open.
struct ShareLifecycle {
  std::string symbol;
  int direction = 1;                ///< +1 opened long, -1 opened short.
  Timestamp opened = 0;
  std::optional<Timestamp> closed;  ///< Absent while any shares remain.
  Quantity shares = 0;              ///< Current signed shares; zero once closed.
  Quantity max_shares = 0;          ///< Largest absolute holding.
  Quantity opened_shares = 0;       ///< Shares that opened or added.
  Quantity closed_shares = 0;       ///< Shares that reduced.
  Money open_notional;              ///< Sum of opening price * shares.
  Money close_notional;             ///< Sum of closing price * shares.
  Money gross;                      ///< Realised P&L so far; share trades carry no fees.
  Money dividends;                  ///< Dividends received (negative: paid) while held.
  std::vector<std::uint64_t> fills;  ///< StockFill IDs, the first one opening it.
};

/// Rebuilds lifecycles in execution order, in the order each opened. Each one
/// replays its own fills through a fresh Ledger, so realised P&L uses the same
/// basis allocation and rounding as the account. A reversing fill closes one
/// lifecycle and opens the next at the same price, splitting its fee pro rata.
/// Closures apply after the fills they follow; unknown contracts are skipped.
[[nodiscard]] std::vector<Lifecycle> lifecycles(const std::vector<Fill>& fills,
    const std::vector<Closure>& closures, const std::map<std::string, md::OptionContract>& contracts);

/// The same for shares, from the account's stock fills: a fill that reverses the
/// holding closes one round trip and opens the next at its price. Each dividend
/// belongs to the round trip holding its shares when it was paid.
[[nodiscard]] std::vector<ShareLifecycle> share_lifecycles(const std::vector<StockFill>& fills,
                                                          const std::vector<DividendPayment>& dividends = {});

}  // namespace openport::trading
