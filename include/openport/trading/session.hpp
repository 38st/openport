#pragma once

#include <memory>

#include "openport/trading/evaluation.hpp"
#include "openport/trading/journal.hpp"
#include "openport/trading/risk.hpp"

namespace openport::trading {

struct MarkedPosition {
  Position position;
  std::optional<Money> mark;  ///< Never substituted with zero when missing.
  Timestamp mark_time = 0;
  Timestamp mark_age = 0;
  std::optional<Money> market_value;
  std::optional<Money> unrealised;
  bool fresh = false;
  bool awaiting_settlement = false;
};
struct TradingSnapshot {
  std::uint64_t account_version = 0;
  Timestamp time = 0;
  Account account;
  Money equity;  ///< Last-mark estimate; consult valuation_complete.
  Money start_of_day_equity;
  Money unrealised;
  bool valuation_complete = true;
  bool journal_failed = false;
  std::vector<MarkedPosition> positions;
  std::vector<Order> open_orders;
  std::vector<Order> recent_orders;  ///< All v1 orders, in acceptance sequence.
  std::vector<Fill> recent_fills;    ///< All v1 fills, in execution sequence.
  RiskSnapshot risk;
  ScenarioGrid scenarios;
  std::vector<Reason> quality_flags;
  Evaluation evaluation;             ///< Current attempt's rule progress.
  BuyingPower buying_power;
  std::vector<Closure> closures;     ///< Settlements and resets, in sequence.
  std::vector<AttemptSummary> attempts;  ///< Earlier attempts, oldest first.
};
struct CommandResult {
  Decision decision;
  std::optional<OrderId> order_id;
  std::uint64_t account_version = 0;
};

/// Single-threaded, deterministic reducer. Every timestamp is caller market
/// time (UTC nanoseconds), monotone and nonnegative. No networking or clocks.
/// Business failures return Decisions; invalid commands/arithmetic/journal
/// failures throw TradingError. A journal failure latches a stop on all commands.
/// Snapshots are immutable owned values, valid after subsequent commands.
class TradingSession {
 public:
  TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal = {});
  ~TradingSession();
  TradingSession(TradingSession&&) noexcept;
  TradingSession& operator=(TradingSession&&) noexcept;
  TradingSession(const TradingSession&) = delete;
  TradingSession& operator=(const TradingSession&) = delete;

  /// Caller must resolve from a known definition. Conflicting OSI terms reject.
  CommandResult define(const md::OptionContract& contract, Timestamp time);
  /// The integration may reject a resolved but unsupported definition before
  /// registration; it still consumes the client ID and journals a rejected order.
  CommandResult submit(OrderRequest request, Timestamp time, Decision rejection = {});
  CommandResult cancel(OrderId id, Timestamp time);
  /// Apply a whole batch before risk/matching. Unknown symbols and future data
  /// reject the whole batch; duplicate/older observations are ignored. Supply
  /// at most one quote and one valuation per OSI per batch. An empty batch
  /// advances expiry, DAY cancellation, freshness and daily-loss monitoring.
  CommandResult on_quotes(const std::vector<QuoteObservation>& quotes,
                          const std::vector<Valuation>& valuations, Timestamp time);
  CommandResult set_limits(Limits limits, Timestamp time);
  CommandResult trip_kill(std::string reason, Timestamp time);
  CommandResult reset_kill(std::string reason, Timestamp time);
  /// Explicit authoritative reference, including AM imports. Only after expiry.
  CommandResult settle(const std::string& symbol, Money settlement, Timestamp time);
  /// Explicit baseline reset, once per later New York date; requires full marks.
  /// Kill latch persists across rollover.
  CommandResult roll_day(Timestamp time);
  /// Start a new attempt: cancel working orders, close positions at their last
  /// mark as Reset closures (no fills, no fees), restore cash, clear the kill
  /// latch and apply the given rules. Order and fill history is kept.
  CommandResult reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time);
  [[nodiscard]] std::shared_ptr<const TradingSnapshot> snapshot() const;

  /// Read-only integration context, owned by the reducer. The engine copies it
  /// into its publication; recovery therefore needs no parallel journal schema.
  [[nodiscard]] const SessionConfig& config() const;
  [[nodiscard]] const std::map<std::string, md::OptionContract>& contracts() const;
  [[nodiscard]] const std::map<std::string, Valuation>& valuations() const;
  [[nodiscard]] std::optional<QuoteObservation> quote(const std::string& symbol) const;
  [[nodiscard]] md::Date trading_day() const;

  /// Rebuild recorded outcomes without matching/repricing. Optional resumed
  /// sink must have exactly the verified recovered head and sequence.
  static TradingSession recover(const JournalRecovery& recovery,
                                std::shared_ptr<Journal> journal = {});
  /// Stable diagnostic representation, also convenient for exact replay checks.
  [[nodiscard]] std::string snapshot_json() const;
 private:
  struct Impl;
  explicit TradingSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::trading
