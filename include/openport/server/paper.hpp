#pragma once

#include <functional>
#include <memory>
#include <string>

#include "openport/trading/session.hpp"

namespace openport::server {

struct TradingStatus {
  bool enabled = false;
  std::string reason = "PAPER_DISABLED";
  std::uint64_t account_version = 0;
  bool kill_latched = false;
  std::string write = "disabled";
};

/// The reducer snapshot and its pricing inputs are published together so HTTP
/// readers never combine portfolio state with a different risk frame.
struct TradingView {
  std::shared_ptr<const trading::TradingSnapshot> snapshot;
  trading::SessionConfig config;
  std::map<std::string, md::OptionContract> contracts;
  std::map<std::string, trading::Valuation> valuations;
};

struct TradingCommand {
  enum class Kind { Submit, Cancel, Limits, Trip, Reset, Settle };
  Kind kind = Kind::Submit;
  trading::OrderRequest order;
  trading::OrderId order_id = 0;
  trading::Limits limits;
  std::uint64_t expected_revision = 0;
  std::string reason;
  std::string symbol;
  trading::Money settlement;
};

struct TradingReply {
  trading::Decision decision;
  std::string error_code;  ///< Transport/revision errors outside reducer Reasons.
  std::optional<trading::OrderId> order_id;
  std::shared_ptr<const TradingView> view;
  std::vector<trading::OrderId> cancelled_orders;
};
using TradingCompletion = std::function<void(TradingReply)>;

}  // namespace openport::server
