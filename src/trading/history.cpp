#include "openport/trading/history.hpp"

#include <algorithm>
#include <cmath>

namespace openport::trading {

Money naked_requirement(const md::OptionContract& c, std::optional<double> spot) {
  const double s = spot && std::isfinite(*spot) && *spot > 0 ? *spot : c.strike;
  const bool call = c.type == pricing::OptionType::Call;
  const double otm = std::max(0.0, call ? c.strike - s : s - c.strike);
  const double per_unit = std::max(0.20 * s - otm, 0.10 * (call ? s : c.strike));
  return Money::from_double(per_unit * c.multiplier);
}

namespace {
struct Open {
  std::size_t index = 0;
  Ledger ledger;
};
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
}  // namespace

std::vector<Lifecycle> lifecycles(const std::vector<Fill>& fills, const std::vector<Closure>& closures,
                                  const std::map<std::string, md::OptionContract>& contracts) {
  std::vector<Lifecycle> out;
  std::map<std::string, Open> open;
  auto finish = [&](const std::string& symbol, Timestamp time) {
    const auto it = open.find(symbol);
    auto& life = out[it->second.index];
    life.closed = time;
    life.quantity = 0;
    open.erase(it);
  };
  auto start = [&](const Fill& fill, const md::OptionContract& contract, Quantity signed_quantity) {
    Lifecycle life;
    life.symbol = fill.symbol;
    life.contract = contract;
    life.direction = signed_quantity > 0 ? 1 : -1;
    life.opened = fill.time;
    life.first_fill = fill.id;
    out.push_back(std::move(life));
    open[fill.symbol] = Open{out.size() - 1, Ledger{}};
  };
  auto apply_fill = [&](const Fill& fill) {
    const auto contract = contracts.find(fill.symbol);
    if (contract == contracts.end() || fill.quantity <= 0) return;
    const Quantity signed_quantity = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
    auto it = open.find(fill.symbol);
    if (it == open.end()) {
      start(fill, contract->second, signed_quantity);
      it = open.find(fill.symbol);
    }
    auto* life = &out[it->second.index];
    const auto held = life->quantity;
    if (held == 0 || (held > 0) == (signed_quantity > 0)) {
      it->second.ledger.fill(contract->second, signed_quantity, fill.price, fill.fee);
      life->quantity = held + signed_quantity;
      life->opened_contracts += fill.quantity;
      life->open_notional = life->open_notional + fill.price * fill.quantity;
    } else {
      const auto closing = std::min(magnitude(held), fill.quantity);
      const auto remainder = fill.quantity - closing;
      const Money closing_fee = fill.fee.prorate(closing, fill.quantity);
      it->second.ledger.fill(contract->second, signed_quantity > 0 ? closing : -closing, fill.price, closing_fee);
      life->quantity = held + (signed_quantity > 0 ? closing : -closing);
      life->closed_contracts += closing;
      life->close_notional = life->close_notional + fill.price * closing;
      if (remainder > 0) {
        life->fills.push_back(fill.id);
        life->gross = it->second.ledger.account().realised;
        life->fees = it->second.ledger.account().fees;
        finish(fill.symbol, fill.time);
        start(fill, contract->second, signed_quantity > 0 ? remainder : -remainder);
        it = open.find(fill.symbol);
        life = &out[it->second.index];
        it->second.ledger.fill(contract->second, signed_quantity > 0 ? remainder : -remainder,
                               fill.price, fill.fee - closing_fee);
        life->quantity = signed_quantity > 0 ? remainder : -remainder;
        life->opened_contracts += remainder;
        life->open_notional = life->open_notional + fill.price * remainder;
      }
    }
    life->max_quantity = std::max(life->max_quantity, magnitude(life->quantity));
    life->fills.push_back(fill.id);
    life->gross = it->second.ledger.account().realised;
    life->fees = it->second.ledger.account().fees;
    if (life->quantity == 0) finish(fill.symbol, fill.time);
  };
  auto apply_closure = [&](const Closure& closure) {
    const auto it = open.find(closure.symbol);
    const auto contract = contracts.find(closure.symbol);
    if (it == open.end() || contract == contracts.end()) return;
    auto& life = out[it->second.index];
    const auto held = life.quantity;
    if (held == 0) return;
    // An early exercise can take part of the position; the rest stays open.
    const auto closing = closure.kind == ClosureKind::Exercise
        ? std::min(magnitude(closure.quantity), magnitude(held)) : magnitude(held);
    if (closure.kind == ClosureKind::Settlement) {
      it->second.ledger.settle(closure.symbol, closure.price);
    } else {
      it->second.ledger.fill(contract->second, held > 0 ? -closing : closing, closure.price, Money{});
    }
    life.quantity = held > 0 ? held - closing : held + closing;
    life.closed_contracts += closing;
    life.close_notional = life.close_notional + closure.price * closing;
    life.gross = it->second.ledger.account().realised;
    life.fees = it->second.ledger.account().fees;
    if (life.quantity != 0) return;
    life.closure = closure.kind;
    finish(closure.symbol, closure.time);
  };
  std::size_t next_closure = 0;
  auto closures_until = [&](std::uint64_t executed) {
    while (next_closure < closures.size() && closures[next_closure].after_fill <= executed)
      apply_closure(closures[next_closure++]);
  };
  closures_until(0);
  for (std::size_t i = 0; i < fills.size(); ++i) {
    apply_fill(fills[i]);
    closures_until(i + 1);
  }
  while (next_closure < closures.size()) apply_closure(closures[next_closure++]);
  return out;
}

std::vector<ShareLifecycle> share_lifecycles(const std::vector<StockFill>& fills) {
  std::vector<ShareLifecycle> out;
  std::map<std::string, Open> open;
  auto start = [&](const StockFill& fill, Quantity signed_shares) {
    ShareLifecycle life;
    life.symbol = fill.symbol;
    life.direction = signed_shares > 0 ? 1 : -1;
    life.opened = fill.time;
    out.push_back(std::move(life));
    open[fill.symbol] = Open{out.size() - 1, Ledger{}};
  };
  // Each fill opens, adds, reduces or reverses; the ledger keeps the basis.
  auto trade = [&](Open& o, const StockFill& fill, Quantity signed_shares) {
    auto& life = out[o.index];
    const auto held = life.shares;
    o.ledger.trade_stock(fill.symbol, signed_shares, fill.price, Money{});
    life.shares = held + signed_shares;
    if (held == 0 || (held > 0) == (signed_shares > 0)) {
      life.opened_shares += magnitude(signed_shares);
      life.open_notional = life.open_notional + fill.price * magnitude(signed_shares);
    } else {
      life.closed_shares += magnitude(signed_shares);
      life.close_notional = life.close_notional + fill.price * magnitude(signed_shares);
    }
    life.max_shares = std::max(life.max_shares, magnitude(life.shares));
    life.gross = o.ledger.account().realised;
  };
  for (const auto& fill : fills) {
    if (fill.shares == 0) continue;
    if (!open.contains(fill.symbol)) start(fill, fill.shares);
    auto* o = &open.at(fill.symbol);
    const auto held = out[o->index].shares;
    const bool reverses = held != 0 && (held > 0) != (fill.shares > 0) && magnitude(fill.shares) > magnitude(held);
    trade(*o, fill, reverses ? -held : fill.shares);
    out[o->index].fills.push_back(fill.id);
    if (out[o->index].shares == 0) {
      out[o->index].closed = fill.time;
      open.erase(fill.symbol);
      if (!reverses) continue;
      const auto remainder = fill.shares + held;
      start(fill, remainder);
      o = &open.at(fill.symbol);
      trade(*o, fill, remainder);
      out[o->index].fills.push_back(fill.id);
    }
  }
  return out;
}

}  // namespace openport::trading
