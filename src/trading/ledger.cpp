#include "openport/trading/ledger.hpp"

#include <algorithm>
#include <limits>

namespace openport::trading {
namespace {
Quantity magnitude(Quantity q) {
  if (q == std::numeric_limits<Quantity>::min()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Quantity magnitude overflow");
  return q < 0 ? -q : q;
}
Quantity add_quantity(Quantity a, Quantity b) {
  Quantity result = 0;
  if (__builtin_add_overflow(a, b, &result)) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Position quantity overflow");
  return result;
}
}  // namespace
void Ledger::fill(const md::OptionContract& contract, Quantity signed_quantity, Money price, Money fee) {
  const auto decision = eligible(contract);
  if (!decision.ok()) throw TradingError(decision.code, decision.message);
  if (signed_quantity == 0 || price < Money{} || fee < Money{})
    throw TradingError(Reason::INVALID_ORDER, "Fill requires nonzero quantity and nonnegative price/fee");
  const auto symbol = contract.osi_symbol();
  const auto it = positions_.find(symbol);
  Position next = it == positions_.end() ? Position{contract, 0, {}, {}, {}} : it->second;
  const auto old_q = next.quantity;
  const auto total_q = add_quantity(old_q, signed_quantity);
  (void)magnitude(total_q);
  const auto size = magnitude(signed_quantity);
  const auto old_size = magnitude(old_q);
  const Money notional = (price * 100) * signed_quantity;
  const Money cash = account_.cash - notional - fee;
  Money realised;
  if (old_q == 0 || (old_q > 0) == (signed_quantity > 0)) {
    next.basis = next.basis + notional;
  } else {
    const auto closed = std::min(old_size, size);
    const Money allocated = next.basis.prorate(closed, old_size);
    realised = ((price * 100) * (old_q > 0 ? closed : -closed)) - allocated;
    next.basis = next.basis - allocated;
    if (size > closed) next.basis = (price * 100) * total_q;
  }
  next.quantity = total_q;
  next.realised = next.realised + realised;
  next.fees = next.fees + fee;
  const Account account{cash, account_.realised + realised, account_.fees + fee};
  if (total_q == 0) positions_.erase(symbol);
  else positions_.insert_or_assign(symbol, std::move(next));
  account_ = account;
}
void Ledger::settle(const std::string& symbol, Money intrinsic) {
  const auto it = positions_.find(symbol);
  if (it == positions_.end() || intrinsic < Money{})
    throw TradingError(Reason::INVALID_SETTLEMENT, "Settlement requires an open position and nonnegative intrinsic");
  const Money proceeds = (intrinsic * 100) * it->second.quantity;
  const Account next{account_.cash + proceeds, account_.realised + proceeds - it->second.basis, account_.fees};
  positions_.erase(it);
  account_ = next;
}
void Ledger::withdraw(Money amount) {
  if (amount <= Money{}) throw TradingError(Reason::INVALID_PAYOUT, "Withdrawal must be positive");
  account_.cash = account_.cash - amount;
}
Ledger Ledger::restore(Account account, std::map<std::string, Position> positions) {
  for (const auto& [symbol, p] : positions) {
    if (!eligible(p.contract).ok() || p.contract.osi_symbol() != symbol || p.quantity == 0 ||
        p.quantity == std::numeric_limits<Quantity>::min() || p.fees < Money{} ||
        (p.quantity > 0 ? p.basis < Money{} : p.basis > Money{}))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Invalid recorded ledger position");
  }
  if (account.fees < Money{}) throw TradingError(Reason::JOURNAL_CORRUPT, "Negative recorded fees");
  Ledger result;
  result.account_ = account;
  result.positions_ = std::move(positions);
  return result;
}
}  // namespace openport::trading
