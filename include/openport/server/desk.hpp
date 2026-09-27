#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "openport/analytics/chain_book.hpp"
#include "openport/md/provider.hpp"
#include "openport/server/candles.hpp"
#include "openport/server/paper.hpp"
#include "openport/trading/dividends.hpp"
#include "openport/analytics/volatility.hpp"

namespace openport::server {

/// Single-owner market-to-account kernel. The caller supplies both clocks and
/// drain boundaries; this class has no threads, queue, HTTP or clock reads.
class Desk {
 public:
  struct Options {
    std::string initial_actor = "system";
    std::string run_input;  ///< Canonical provenance JSON; empty for legacy/live desks.
    bool paper_enabled = true;
    bool replay = false;  ///< Publish the replay market clock and keep halt state isolated.
    std::filesystem::path paper_journal;  ///< The main account. Empty only for explicit in-process simulations.
    /// More named accounts, one journal each (<id>.jsonl, named in <id>.name). Empty for none.
    std::filesystem::path paper_accounts;
    trading::SessionConfig paper;
    /// Dividends every account pays on held shares at the rollover into each ex-date.
    std::vector<trading::Dividend> dividends;
    std::vector<analytics::EventLabel> events;
    std::shared_ptr<trading::Journal> paper_sink;  ///< Optional in-process test/simulation sink.
    std::string write_mode = "open";
    analytics::AnalyticsOptions analytics;
    std::shared_ptr<CandleStore> candles;
    std::function<void(std::string_view, const EquitySample&)> equity_sample;
  };
  struct PendingCommand {
    std::uint64_t sequence;
    TradingCommand command;
    TradingCompletion completion;
  };

  Desk(std::string provider, md::Capabilities capabilities, md::Subscription subscription, Options options);
  void start_trading();
  void halt() { stopping_ = true; }
  void stop() { halt(); accounts_.clear(); }
  void set_dividends(std::vector<trading::Dividend> dividends);
  void observe(const md::Event& event);
  bool refresh_analytics();
  void apply_analytics(std::shared_ptr<const analytics::UnderlyingMetrics> result, md::Timestamp time);
  void update_trading(const std::vector<md::Event>& batch, std::deque<PendingCommand>& commands,
                      md::Timestamp driver_time);
  void apply_command(PendingCommand& pending, md::Timestamp market_time, md::Timestamp driver_time);
  void replay_batch(const std::vector<md::Event>& batch, md::Timestamp driver_time, md::Timestamp boundary_time = 0);
  void command(TradingCommand command, TradingCompletion completion, md::Timestamp time, md::Timestamp driver_time);
  [[nodiscard]] bool active() const;
  [[nodiscard]] md::Timestamp market_time() const { return market_time_; }
  [[nodiscard]] const analytics::ChainBook& book() const { return book_; }
  [[nodiscard]] const auto& publications() const { return metrics_; }
  [[nodiscard]] const auto& views() const { return trading_views_; }
  [[nodiscard]] const auto& accounts() const { return account_statuses_; }
  [[nodiscard]] const auto& trading_status() const { return trading_status_; }
  [[nodiscard]] const auto& breaker() const { return breaker_; }
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const;
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view(std::string_view account = {}) const;

 private:
  /// One paper account: a reducer and its journal. Single-owner state.
  struct PaperAccount {
    std::string id;
    std::string name;
    std::unique_ptr<trading::TradingSession> session;
    std::string failure;  ///< Why it cannot trade; empty while it can.
    std::unique_ptr<EquityStore> equity;
    std::shared_ptr<const trading::TradingSnapshot> sampled_snapshot;
  };
  PaperAccount* find_account(std::string_view id);
  void create_account(const TradingCommand& command, TradingReply& reply);
  void observe_trading(const md::Event& event);
  void publish_trading();
  void record_input(const std::string& input, const std::string& actor = "system");
  void sample_equity(PaperAccount& account);
  /// Stops one account after a journal or integration failure; the others carry on.
  void fail_trading(PaperAccount& account, std::string reason);

  Options options_;
  std::string provider_;
  md::Subscription subscription_;
  md::Capabilities capabilities_;
  analytics::ChainBook book_;
  bool stopping_ = false;
  TradingStatus trading_status_;
  std::vector<AccountStatus> account_statuses_;
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> trading_views_;
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics_;
  // Initialized/recovered by start_trading(), then owned by the caller.
  std::vector<PaperAccount> accounts_;  // the main account first
  std::map<std::string, std::string> settlement_source_;
  /// Each underlying's first print at or after a date's regular close, and its
  /// last one before it, for the last week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> closing_prints_;
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> before_close_;
  /// Official closes by symbol and date (md::UnderlyingClose), for a week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingClose> official_closes_;
  CircuitBreakerStatus breaker_;
  bool breaker_dirty_ = false;
  bool breaker_storage_ = false;  // only while the main journal's writer lock is held
  std::vector<trading::Dividend> dividends_;
  std::uint64_t dividends_version_ = 0;
  void check_circuit_breaker(const md::UnderlyingQuote& spot);
  void advance_circuit_breaker(md::Timestamp time);
  void publish_circuit_breaker();
  void load_circuit_breaker();
  void save_circuit_breaker();
  md::Timestamp market_time_ = 0;
  std::map<std::string, md::InstrumentId> instruments_;
  std::map<std::string, std::uint64_t> observations_;
  /// Each underlying's latest md::SnapshotComplete time, from snapshot providers.
  std::map<std::string, md::Timestamp> snapshots_;

  std::map<std::string, std::uint64_t> analysed_versions_;
  std::uint64_t analysed_dividends_version_ = 0;
  std::map<std::string, std::shared_ptr<const analytics::DiscountCurve>> discount_curves_;
  std::shared_ptr<const analytics::DiscountCurve> discount_curve_;
};

}  // namespace openport::server
