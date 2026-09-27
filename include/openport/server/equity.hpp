#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "openport/trading/session.hpp"

namespace openport::server {
struct EquitySample {
  md::Timestamp time = 0;
  std::uint64_t attempt = 0;
  trading::Money equity;
  std::optional<trading::Money> floor;
  trading::Money peak;
  std::optional<trading::Money> target;
  std::optional<trading::Money> tomorrow_floor;
  std::uint64_t fill = 0;  ///< Zero for a minute sample; otherwise the fill ID.
  std::uint64_t stock_fill = 0;  ///< A share change, at its committed transaction mark.
};
/// Exact marked-equity changes from committed fills. Atomic combo legs share
/// one mark. The engine calls this after each reducer transaction with fills.
[[nodiscard]] std::vector<EquitySample> fill_equity_samples(const trading::TradingSession& session,
    const trading::TradingSnapshot& before);
/// Engine-thread owned. Append-only CSV beside the journal, monetary values in
/// micro-dollars, timestamps in nanoseconds. Keeps the current attempt and at
/// most 90 days / 100,000 samples of earlier attempts. Storage failures never throw.
class EquityStore {
 public:
  explicit EquityStore(std::filesystem::path file);
  void append(const EquitySample& sample) noexcept;
  [[nodiscard]] const std::vector<EquitySample>& samples() const { return samples_; }
  [[nodiscard]] const std::string& error() const { return error_; }
 private:
  void compact();
  std::filesystem::path file_;
  std::vector<EquitySample> samples_;
  std::size_t lines_ = 0;
  std::string error_;
};
}  // namespace openport::server
