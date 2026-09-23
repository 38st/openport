#pragma once

#include "openport/trading/types.hpp"

namespace openport::trading {

struct Position {
  md::OptionContract contract;
  Quantity quantity = 0;
  Money basis;  ///< Signed total acquisition notional, excludes fees.
  Money realised;
  Money fees;
};
/// Shares of an underlying, from exercise and assignment.
struct StockPosition {
  std::string symbol;   ///< The underlying, e.g. "SPY".
  Quantity shares = 0;  ///< Signed.
  Money basis;          ///< Signed total acquisition cost, excludes fees.
  Money realised;
  Money fees;
};
struct Account {
  Money cash;
  Money realised;  ///< Gross, before fees, including settled/closed positions.
  Money fees;
};

/// Pure accounting. Mutations have the strong exception guarantee. Reductions
/// allocate signed basis proportionally, nearest micro-dollar (ties away); the
/// residual stays in the position and is fully released on the last close.
class Ledger {
 public:
  explicit Ledger(Money initial_cash = {}) : account_{initial_cash, {}, {}} {}
  [[nodiscard]] const Account& account() const { return account_; }
  [[nodiscard]] const std::map<std::string, Position>& positions() const { return positions_; }
  [[nodiscard]] const std::map<std::string, StockPosition>& stocks() const { return stocks_; }
  void fill(const md::OptionContract& contract, Quantity signed_quantity, Money price, Money fee);
  /// Buy (positive) or sell shares at a price per share, with the same basis
  /// allocation as options.
  void trade_stock(const std::string& symbol, Quantity signed_shares, Money price, Money fee);
  /// Settlement is accounting, never subject to order risk limits. No fee.
  void settle(const std::string& symbol, Money intrinsic);
  /// Cash leaves the account (a payout); realised P&L is unchanged.
  void withdraw(Money amount);
  /// Validated journal outcomes only; public for independent outcome consumers.
  static Ledger restore(Account account, std::map<std::string, Position> positions,
                        std::map<std::string, StockPosition> stocks = {});
 private:
  Account account_;
  std::map<std::string, Position> positions_;
  std::map<std::string, StockPosition> stocks_;
};

}  // namespace openport::trading
