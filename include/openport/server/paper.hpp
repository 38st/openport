#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "openport/trading/session.hpp"

namespace openport::server {

/// Shared session/feed gate for new orders and status/ticks. Contract, risk and
/// write-access checks remain separate. Never advances the reducer's data clock.
[[nodiscard]] trading::Decision paper_acceptance(std::string_view underlying,
    md::Timestamp market_time, md::Timestamp wall_time, std::chrono::seconds delay,
    md::Timestamp max_quote_age);

struct TradingStatus {
  bool enabled = false;
  std::string reason = "PAPER_DISABLED";
  std::uint64_t account_version = 0;
  bool kill_latched = false;
  std::string write = "disabled";
  trading::Money fee_per_contract;
  trading::Money initial_cash;
  std::string plan;        ///< Active rules' display name; empty without a plan.
  std::string evaluation;  ///< active/passed/failed with a target or drawdown rule, else empty.
};

/// The reducer snapshot and its pricing inputs are published together so HTTP
/// readers never combine portfolio state with a different risk frame.
struct TradingView {
  std::shared_ptr<const trading::TradingSnapshot> snapshot;
  trading::SessionConfig config;
  std::map<std::string, md::OptionContract> contracts;
  std::map<std::string, trading::Valuation> valuations;
  /// Per-underlying data clocks, seeded from persisted quotes on recovery.
  std::map<std::string, md::Timestamp> market_times;
};

struct TradingCommand {
  enum class Kind { Submit, Cancel, Limits, Trip, Reset, Settle, ResetAccount, Payout, Modify, CancelAll, ClosePositions };
  Kind kind = Kind::Submit;
  trading::OrderRequest order;
  trading::OrderId order_id = 0;
  trading::Limits limits;
  std::uint64_t expected_revision = 0;
  std::string reason;
  std::string symbol;
  trading::Money settlement;
  trading::Money initial_cash;   ///< ResetAccount: the new starting balance.
  trading::AccountRules rules;   ///< ResetAccount: the new attempt's rules.
  /// ResetAccount: a plan name whose evaluation the current attempt must have
  /// passed (funded presets); empty for no requirement.
  std::string required_pass;
  trading::Money amount;         ///< Payout: the withdrawal.
  trading::OrderChange change;   ///< Modify: the order's new terms.
  std::string underlying;        ///< CancelAll and ClosePositions: one underlying, or empty for all.
};

struct TradingReply {
  trading::Decision decision;
  std::string error_code;  ///< Transport/revision errors outside reducer Reasons.
  std::optional<trading::OrderId> order_id;
  std::shared_ptr<const TradingView> view;
  std::vector<trading::OrderId> cancelled_orders;
  std::vector<trading::OrderId> created_orders;  ///< Orders the command added, in sequence.
};
using TradingCompletion = std::function<void(TradingReply)>;

}  // namespace openport::server
