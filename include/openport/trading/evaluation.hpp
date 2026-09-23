#pragma once

#include <optional>
#include <string>
#include <vector>

#include "openport/trading/ledger.hpp"

namespace openport::trading {

enum class EvaluationStatus { Active, Passed, Failed };

/// One finished New York trading day of an attempt, recorded at rollover.
struct EvaluationDay {
  md::Date day;
  Money open_equity;   ///< Fully marked equity when the day began (its baseline).
  Money close_equity;  ///< Last fully marked equity observed on that date.
  Money peak;          ///< High-water mark after that day's ratchet.
  Money floor;         ///< Drawdown floor after that day's ratchet; zero without a rule.
  Money realised;      ///< Net realised P&L of the day (after fees).
  bool qualifying = false;  ///< Funded accounts: the day counted toward a payout.
};

/// A funded-account withdrawal. The account pays out `amount`; the trader keeps
/// `trader_share` of it.
struct Payout {
  std::uint64_t number = 0;
  Timestamp time = 0;
  md::Date day;   ///< Trading day in progress when requested; it and later days count toward the next payout.
  Money amount;
  Money trader_share;
  Money balance;  ///< Equity when requested, before the withdrawal.
};

/// Rule progress for the current attempt. Only fully marked equity (every
/// position has a mark, fresh or not) ratchets the peak or decides the outcome.
struct Evaluation {
  std::uint64_t attempt = 1;
  Timestamp started = 0;
  Money starting_balance;
  Money peak;   ///< High-water mark that the trailing floor follows.
  Money floor;  ///< peak - max_drawdown with a drawdown rule, otherwise zero.
  EvaluationStatus status = EvaluationStatus::Active;
  Timestamp decided_at = 0;
  Money decided_equity;
  std::string decision;           ///< Human-readable pass/fail explanation.
  OrderId first_order = 1;        ///< Orders with smaller IDs belong to earlier attempts.
  std::uint64_t first_fill = 1;   ///< Likewise for fills.
  md::Date day;                   ///< Trading day in progress.
  Money day_open_equity;
  Money day_close_equity;         ///< Latest fully marked equity observed on `day`.
  std::vector<EvaluationDay> days;  ///< Finished days of this attempt, oldest first.
  bool floor_locked = false;      ///< The floor reached the lock balance and stopped trailing.
  Money day_open_realised;        ///< Net realised P&L when `day` began.
  std::uint64_t qualifying_days = 0;  ///< Days closed since the last payout (or the start) that qualified.
  Timestamp cycle_started = 0;
  std::vector<Payout> payouts;
};

/// An earlier attempt, summarised when the account is reset.
struct AttemptSummary {
  std::uint64_t attempt = 0;
  std::string plan;
  Timestamp started = 0;
  Timestamp ended = 0;
  Money starting_balance;
  Money final_equity;  ///< Last-mark estimate at reset.
  EvaluationStatus status = EvaluationStatus::Active;
  std::string decision;
  OrderId first_order = 1;
  std::uint64_t first_fill = 1;
};

enum class ClosureKind { Settlement, Reset };

/// A position that left the ledger without a fill, so trade history can close it.
struct Closure {
  std::string symbol;
  Quantity quantity = 0;  ///< Signed position that was closed.
  Money price;            ///< Per unit: settlement intrinsic, or the last mark at reset.
  Timestamp time = 0;
  ClosureKind kind = ClosureKind::Settlement;
  std::uint64_t after_fill = 0;  ///< Fills recorded before the closure, for ordering.
};

/// Cash buying power. Long premium is paid in full. A short option holds its
/// buy-back value plus the naked requirement; working orders reserve their
/// worst-case cash use. Spreads are not netted: each short leg is naked.
struct BuyingPower {
  Money available;          ///< cash - short_requirement - reserved; may be negative.
  Money reserved;
  Money short_requirement;
};

/// Per-contract naked short requirement excluding premium: 100 * max(20% of spot
/// less the out-of-the-money amount, 10% of spot for calls or of strike for puts).
/// Without a valid spot the strike stands in for it.
[[nodiscard]] Money naked_requirement(const md::OptionContract& contract, std::optional<double> spot);

}  // namespace openport::trading
