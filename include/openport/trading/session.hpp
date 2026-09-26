#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "openport/trading/dividends.hpp"
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
struct MarkedStock {
  StockPosition position;
  std::optional<Money> mark;  ///< The underlying's latest price.
  Timestamp mark_time = 0;
  std::optional<Money> market_value;
  std::optional<Money> unrealised;
  bool fresh = false;
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
  std::vector<MarkedStock> stocks;  ///< Shares from exercise and assignment.
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
  std::vector<StockFill> stock_fills;    ///< Every change in shares held, oldest first.
  std::vector<DividendPayment> dividends;  ///< Dividends paid on held shares, oldest first.
  /// Notes and tags by trade, named by its opening fill's ID.
  std::map<std::string, Annotation> annotations;
  /// Closing prints recorded for PM settlement, by "UNDERLYING YYYY-MM-DD".
  std::map<std::string, ClosingPrint> closing_prints;
  /// Today's P&L by Greek for the account, and for each contract held or traded
  /// today. Positions held from before an upgrade join at the next fill or rollover.
  Attribution attribution;
  std::map<std::string, Attribution> attributions;
};
/// A funded account's standing for its next payout: an active, flat account
/// with the required qualifying days since the last payout. `blocked` is the
/// first unmet requirement; when it is NONE, any whole-cent amount from
/// `minimum` to `maximum` is accepted.
struct PayoutQuote {
  Decision blocked;
  std::uint64_t number = 1;      ///< The next payout's number.
  bool funded = false;           ///< Funded phase with payout rules.
  bool active = false;
  bool flat = false;             ///< No positions and no working or armed orders.
  std::uint64_t qualifying_days = 0;
  std::int64_t required_days = 0;
  Money profit;                  ///< Equity less the starting balance.
  Money withdrawable;            ///< withdrawal_percent of positive profit, whole cents.
  std::optional<Money> cap;      ///< This payout number's cap.
  Money maximum;                 ///< min(withdrawable, cap), leaving equity above a locked floor.
  Money minimum;
  Money trader_share;            ///< split_percent of the maximum.
};
[[nodiscard]] PayoutQuote payout_quote(const TradingSnapshot& snapshot, const AccountRules& rules);

struct CommandResult {
  Decision decision;
  std::optional<OrderId> order_id;
  std::uint64_t account_version = 0;
  /// A retried order, its client_order_id and terms already submitted: the first
  /// answer, with nothing recorded.
  bool replayed = false;
};

/// New terms for an open order; each field left empty keeps its value.
struct OrderChange {
  std::optional<Quantity> quantity;    ///< The total, filled contracts (or units) included.
  std::optional<Money> limit_price;    ///< Limit orders only; a multi-leg order's net per unit.
  std::optional<Money> trigger_level;  ///< Armed orders with a trigger only.
  [[nodiscard]] bool empty() const { return !quantity && !limit_price && !trigger_level; }
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
  /// Change a resting order in place: a DAY limit order, an armed order or a
  /// bracket exit. Its ID, fills and place among equal prices stay. The new
  /// terms pass every check a new order takes, its own reservation released
  /// first; a failure leaves the order unchanged. A limit that becomes
  /// marketable trades at once, and an armed order whose new level is already
  /// reached activates in the regular session. The quantity must stay above
  /// the filled quantity. Bracket exits change only their level or price:
  /// their size follows the position. `rejection` is the integration's
  /// acceptance gate, as for submit.
  CommandResult modify(OrderId id, OrderChange change, Timestamp time, Decision rejection = {});
  /// Cancel every open order, or every open order on one underlying.
  CommandResult cancel_all(std::optional<std::string> underlying, Timestamp time);
  /// Flatten the account, or one underlying: cancel the open orders in scope,
  /// then close each unexpired position in scope with a market IOC at the
  /// displayed quotes with the account's slippage, short positions first so a
  /// spread never leaves a naked short. Expired positions wait for their settlement.
  /// Every closing order takes the checks any order does; one that cannot
  /// trade is recorded as rejected (with an underlying's entry in `rejections`
  /// when the integration refuses it) and the others still go. The decision
  /// is always success: outcomes are on the orders.
  CommandResult close_positions(std::optional<std::string> underlying, Timestamp time,
                                const std::map<std::string, Decision>& rejections = {});
  /// Apply a whole batch before risk/matching. Unknown symbols and future data
  /// reject the whole batch; duplicate/older observations are ignored. Supply
  /// at most one quote and one valuation per OSI per batch. An empty batch
  /// advances expiry, DAY cancellation, freshness and daily-loss monitoring.
  /// On an idle account (flat, no open orders, attempt started) an empty batch
  /// changes nothing but the clock, so it is not a transaction and nothing is
  /// journaled; the snapshot keeps its time until the next transaction.
  /// `stocks` prices the underlyings: the shares that exercise and assignment
  /// deliver are marked and traded at them.
  CommandResult on_quotes(const std::vector<QuoteObservation>& quotes,
                          const std::vector<Valuation>& valuations, Timestamp time,
                          const std::vector<StockPrice>& stocks = {});
  CommandResult set_limits(Limits limits, Timestamp time);
  CommandResult trip_kill(std::string reason, Timestamp time);
  CommandResult reset_kill(std::string reason, Timestamp time);
  /// Explicit authoritative reference, including AM imports. Only after expiry.
  CommandResult settle(const std::string& symbol, Money settlement, Timestamp time);
  /// Keeps an underlying's closing print for a date in the journal, so PM
  /// settlement uses it after a restart. A different price for the date replaces
  /// it, as an official close replaces a provisional print; positions already
  /// settled keep what they settled on.
  CommandResult record_close(const std::string& underlying, md::Date date, Money price, Timestamp print_time, Timestamp time);
  [[nodiscard]] std::optional<ClosingPrint> closing_print(const std::string& underlying, md::Date date) const;
  /// Explicit baseline reset, once per later New York date; requires full marks.
  /// Kill latch persists across rollover.
  /// `dividends` are those going ex on the new trading date (and any skipped while
  /// the server was down): each pays the shares held into it, once.
  CommandResult roll_day(Timestamp time, const std::vector<Dividend>& dividends = {});
  /// Start a new attempt: cancel working orders, close positions at their last
  /// mark as Reset closures (no fills, no fees), restore cash, clear the kill
  /// latch and apply the given rules. Order and fill history is kept.
  CommandResult reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time);
  /// Withdraw a whole-cent amount from a funded account under its payout rules
  /// (see payout_quote). The withdrawal is not a loss: the day's baseline and
  /// an unlocked trailing peak move down with it. Resets the qualifying days.
  CommandResult request_payout(Money amount, Timestamp time);
  /// Note and tag a trade, named by its opening fill's ID (the `id` of the
  /// trades view): a note of at most 2,000 bytes and up to eight tags of 1 to
  /// 32 bytes without commas, lowercased and each kept once. Invalid text
  /// throws INVALID_NOTE; an unknown trade returns UNKNOWN_TRADE. An empty note
  /// without tags clears it. Allowed whatever the account's state or session.
  CommandResult annotate(std::uint64_t trade, std::string note, std::vector<std::string> tags, Timestamp time);
  /// The same for a share round trip, named by the stock fill that opened it; its
  /// note is kept under "s" and that fill's ID.
  CommandResult annotate_shares(std::uint64_t first_fill, std::string note, std::vector<std::string> tags, Timestamp time);
  /// Exercise `contracts` of a long, in-the-money American equity or ETF option
  /// before expiry: they close at intrinsic value against the underlying's fresh
  /// price, and 100 shares each are bought (calls) or sold (puts) at that price,
  /// which together cost the strike. Held into expiry, such options are exercised
  /// or assigned at settlement when a cent or more in the money.
  CommandResult exercise(const std::string& symbol, Quantity contracts, Timestamp time);
  /// Reduce or close a stock position at the underlying's fresh price in the
  /// regular session, without a fee; shares come only from exercise and assignment.
  CommandResult trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time);
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
  /// Rewrite a journal that recovers into `out`, an empty journal, in the
  /// current schema: the same transactions, times, types, events and
  /// decisions, each state recorded as a change from the one before, and no
  /// snapshots. A final schema 1 record stays as it is, since recovery
  /// completes that journal's evaluation from it. Every rewritten record is
  /// read back before it is written; returns the snapshot_json that `out`
  /// recovers to.
  static std::string compact(const JournalRecovery& recovery, Journal& out);
  /// The reverse: every transaction with its whole state and the snapshot
  /// derived from it (schema 2), for audit tools and builds from before schema 3.
  static void expand(const JournalRecovery& recovery, Journal& out);
  /// Stable diagnostic representation, also convenient for exact replay checks.
  [[nodiscard]] std::string snapshot_json() const;
 private:
  struct Impl;
  explicit TradingSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::trading
