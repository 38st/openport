#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include "openport/server/paper.hpp"

namespace openport::server {
/// The reason an evaluation outside a playbook's entry window gives.
inline constexpr std::string_view kOutsideEntryWindow = "Outside entry window";
struct PlaybookInputs {
  double spot = analytics::kNaN, prior_close = analytics::kNaN, day_open = analytics::kNaN;
  double iv_rank = analytics::kNaN, vrp = analytics::kNaN, term_ratio = analytics::kNaN;
};
void validate_playbook(const nlohmann::json& definition);
bool playbook_window(const nlohmann::json& definition, md::Timestamp time);
bool playbook_conditions(const nlohmann::json& definition, const PlaybookInputs& inputs, double dte);
std::string playbook_tag(const nlohmann::json& definition);
/// Exits for an entry at `entry` (signed: negative is a credit). The take-profit sits on
/// `tick`, the legs' smallest (a nickel for SPX); see playbook_tick.
trading::Bracket playbook_bracket(const nlohmann::json& management, trading::Money entry,
                                  trading::Money tick = trading::Money::from_micros(10'000));
/// The smallest tick among an order's legs, on which a combo's net prices must sit.
trading::Money playbook_tick(const trading::OrderRequest& order);
md::Timestamp playbook_deadline(const nlohmann::json& definition, md::Timestamp opened);
nlohmann::json playbook_report(const nlohmann::json& catalogue, const TradingView& view);

/// Definitions and transient staging are Desk-owned, outside the reducer. The
/// callbacks always use its normal preview and submission paths.
class Playbooks {
 public:
  using Json = nlohmann::json;
  using Preview = std::function<trading::OrderPreview(const trading::OrderRequest&, double)>;
  using Cancel = std::function<void(trading::OrderId)>;
  using Send = std::function<TradingReply(const trading::OrderRequest&)>;
  explicit Playbooks(std::filesystem::path file = {}, Json initial = nullptr);
  [[nodiscard]] bool enabled(std::string_view account) const;
  [[nodiscard]] Json catalogue() const { return catalogue_; }
  [[nodiscard]] Json publication(std::string_view account, bool replay) const;
  /// Changes whenever a publication may have changed.
  [[nodiscard]] std::uint64_t revision() const { return revision_; }
  Json change(const Json& command, std::string_view account, bool replay);
  void evaluate(const std::string& account, bool replay, md::Timestamp now,
      const std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>>& metrics,
      const TradingView& view, const std::function<PlaybookInputs(const std::string&, const Json&)>& inputs,
      const Preview& preview, const Send& send, const Cancel& cancel);
  trading::OrderRequest take(const std::string& account, const std::string& staged);
 private:
  void save(const Json& next);
  std::filesystem::path file_;
  Json catalogue_ = {{"schema", 1}, {"definitions", Json::object()}, {"modes", Json::object()}};
  std::map<std::string, Json> staged_, reasons_;
  /// Setups suppressed for a New York day, with the reason shown meanwhile.
  std::map<std::string, std::pair<md::Date, std::string>> suppressed_;
  std::uint64_t revision_ = 0;
};
}
