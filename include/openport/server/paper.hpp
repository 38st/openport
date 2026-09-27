#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "openport/trading/session.hpp"
#include "openport/analytics/chain_analytics.hpp"
#include "openport/server/equity.hpp"

namespace openport::server {

/// A market-wide trading halt from the S&P 500's circuit breakers (NYSE Rule 7.12,
/// which the options exchanges follow).
struct MarketHalt {
  int level = 0;
  md::Timestamp start = 0;
  md::Timestamp end = 0;   ///< when trading resumes
  double reference = 0.0;  ///< the previous close the fall is measured from
  double price = 0.0;      ///< the print that tripped it
};

/// Published with engine status, independently of the selected paper account.
struct CircuitBreakerStatus {
  struct Close {
    md::Date date;
    double price = 0.0;
    bool operator==(const Close&) const = default;
  };
  std::string symbol;
  md::Date day;
  std::optional<Close> previous_close;
  int level = 0;
  std::vector<MarketHalt> halts;
  md::Timestamp market_time = 0;
  bool active = false;
  std::string error;
};

/// The halt a print of the S&P 500 trips, if any. In the regular session a fall of 7%
/// (level 1) or 13% (level 2) from the previous close halts trading for 15 minutes,
/// each once a day and only until 35 minutes before the close (15:25 ET); a fall of
/// 20% (level 3) halts it for the rest of the day. `tripped` is the highest level
/// already tripped that day.
[[nodiscard]] std::optional<MarketHalt> circuit_breaker(double reference, double price, md::Timestamp time,
                                                        int tripped);

/// Shared session/feed gate for new orders and status/ticks: open while the
/// underlying's options are in any session (regular, overnight or curb; the
/// reducer decides which orders each takes) and no market-wide halt covers the
/// market time. Contract, risk and write-access checks remain separate. Never
/// advances the reducer's data clock.
[[nodiscard]] trading::Decision paper_acceptance(std::string_view underlying,
    md::Timestamp market_time, md::Timestamp wall_time, std::chrono::seconds delay,
    md::Timestamp max_quote_age, const std::vector<MarketHalt>& halts = {});

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
  trading::Contracts contracts;
  trading::Valuations valuations;
  /// Per-underlying data clocks, seeded from persisted quotes on recovery.
  std::map<std::string, md::Timestamp> market_times;
  /// Circuit-breaker halts of the last day.
  std::vector<MarketHalt> halts;
  trading::BreachRisk breach;
  std::vector<EquitySample> equity_samples;
  std::string equity_error;
  std::string playbooks_json;  ///< Immutable catalogue and transient stages for this account.
};

/// The market's implied variance of an underlying's log price from its analytics'
/// market time to that day's regular close: the nearest expiry's at-the-money IV²
/// times its years (as the IV was solved, on calendar time), with today's share of it
/// in regular-session time. Zero once today's session is over; empty without an IV.
[[nodiscard]] std::optional<double> implied_variance_to_close(const analytics::UnderlyingMetrics& metrics);

/// The main account keeps the original journal; others are named alongside it.
inline constexpr std::string_view kMainAccount = "main";

/// One paper account's standing, for the account list and ticks.
struct AccountStatus {
  std::string id;
  std::string name;
  TradingStatus trading;
};

struct TradingCommand {
  enum class Kind { Submit, Cancel, Limits, Trip, Reset, Settle, ResetAccount, Payout, Modify, CancelAll, ClosePositions, CreateAccount, Annotate,
                    Exercise, CloseStock, DayNote, Preview, Guardrails, Playbook };
  Kind kind = Kind::Submit;
  std::string actor = "unknown";
  trading::OrderRequest order;
  trading::OrderId order_id = 0;
  trading::Limits limits;
  trading::Guardrails guardrails;
  double floor_share = 0.5;
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
  std::string account;           ///< The account it acts on; empty for the main account.
  std::string name;              ///< CreateAccount: the new account's display name.
  std::uint64_t trade = 0;        ///< Annotate: the trade, by its opening fill's ID.
  bool shares = false;            ///< Annotate: a share round trip, by its opening stock fill ("s" + ID).
  md::Date day;
  std::string plan;
  std::string review;
  std::string note;               ///< Annotate: the note; empty with no tags clears it.
  std::vector<std::string> tags;  ///< Annotate: the trade's tags.
  /// Exercise: contracts of `symbol`; CloseStock: shares of `symbol` to close, 0 for all.
  trading::Quantity quantity = 0;
};

struct TradingReply {
  trading::Decision decision;
  std::string error_code;  ///< Transport/revision errors outside reducer Reasons.
  std::optional<trading::OrderId> order_id;
  std::shared_ptr<const TradingView> view;
  std::vector<trading::OrderId> cancelled_orders;
  std::vector<trading::OrderId> created_orders;  ///< Orders the command added, in sequence.
  std::string account;  ///< The account the command acted on (a new account's ID for CreateAccount).
  bool replayed = false;  ///< A retried order answered as first submitted.
  std::optional<trading::OrderPreview> preview;
  std::string playbook_result;
};
using TradingCompletion = std::function<void(TradingReply)>;

/// A journal as compact_paper_journals left it.
struct JournalCompaction {
  std::filesystem::path file;
  std::uintmax_t bytes_before = 0;
  std::uintmax_t bytes_after = 0;
  std::filesystem::path backup;  ///< The journal as it was; empty if nothing was rewritten.
  std::string error;             ///< Why the journal was left as it was.
};
/// Rewrite the main paper journal and the account journals in `accounts` whose
/// records carry whole states (schemas 1 and 2) in the compact schema. Each takes the
/// writer's lock, so a running openportd's journals are refused. The rewrite
/// must recover to the same account before it replaces the journal, and the
/// original stays beside it as FILE.bak.
[[nodiscard]] std::vector<JournalCompaction> compact_paper_journals(
    const std::filesystem::path& journal, const std::filesystem::path& accounts);

}  // namespace openport::server
