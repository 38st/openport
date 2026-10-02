#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
  Money basis;                    ///< Signed basis of the remaining position.
  std::optional<FillContext> entry_context;
  std::optional<FillContext> exit_context;
  OrderId exit_order = 0;           ///< The last reducing fill's order; none after a closure.
  std::uint64_t first_fill = 0;     ///< ID of the opening fill, for attempt filtering.
  OrderId entry_order = 0;          ///< The opening fill's order.
  Quantity entry_contracts = 0;     ///< Contracts that order opened in this round trip.
  Money entry_notional;             ///< Their opening price * contracts, per unit.
  /// The first round trip its entry order opened (its own ID for that one): the
  /// trade it is in, unless the account moved it to another (see trade_group).
  std::uint64_t root = 0;
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

/// The state machine lifecycles() runs, fed one fill or closure at a time in
/// execution order. `open` holds each contract's lifecycle in progress; one that
/// finishes moves to `closed`. `started` numbers them in the order they opened.
class LifecycleBuilder {
 public:
  struct Open {
    std::uint64_t started = 0;
    Lifecycle life;
    Ledger ledger;
  };
  /// What a fill did to a round trip: opened it, or reduced or added to it.
  struct Effect {
    OrderId order = 0;
    Timestamp time = 0;
    std::uint64_t trade = 0;  ///< The round trip's ID.
    OrderId entry = 0;        ///< Its entry order.
    bool opened = false;
  };
  void fill(const Fill& fill, const Contracts& contracts);
  void closure(const Closure& closure, const Contracts& contracts);
  std::map<std::string, Open> open;
  std::vector<std::pair<std::uint64_t, Lifecycle>> closed;
  /// The effects of each fill after `record_from`, in order, while `record` is
  /// set; the caller clears them.
  bool record = false;
  std::uint64_t record_from = 0;
  std::vector<Effect> effects;
  /// Orders that added to a round trip another order opened, so the contracts
  /// they share cannot be attributed to their strategy alone.
  std::set<OrderId> shared;
  /// Each order's first round trip, the root of the others it opens.
  std::map<OrderId, std::uint64_t> roots;

 private:
  void start(const Fill& fill, const md::OptionContract& contract, Quantity signed_quantity);
  void finish(const std::string& symbol, Timestamp time);
  std::uint64_t started_ = 0;
};

/// Rebuilds lifecycles in execution order, in the order each opened. Each one
/// replays its own fills through a fresh Ledger, so realised P&L uses the same
/// basis allocation and rounding as the account. A reversing fill closes one
/// lifecycle and opens the next at the same price, splitting its fee pro rata.
/// Closures apply after the fills they follow; unknown contracts are skipped.
[[nodiscard]] std::vector<Lifecycle> lifecycles(const SharedVector<Fill>& fills,
    const SharedVector<Closure>& closures, const Contracts& contracts, std::set<OrderId>* shared = nullptr);

/// The trade a round trip is in, by trade ID: its entry in `groups` (a roll or an
/// adjustment joined to the trade it continues, or the account's own grouping),
/// else its root's. A multi-leg order's legs share their first leg's ID.
[[nodiscard]] std::string trade_group(const Lifecycle& life, const SharedMap<std::string, std::string>& groups);

/// The same for shares, from the account's stock fills: a fill that reverses the
/// holding closes one round trip and opens the next at its price. Each dividend
/// belongs to the round trip holding its shares when it was paid.
[[nodiscard]] std::vector<ShareLifecycle> share_lifecycles(const SharedVector<StockFill>& fills,
                                                          const SharedVector<DividendPayment>& dividends = {});

}  // namespace openport::trading
