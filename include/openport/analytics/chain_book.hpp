#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "openport/md/events.hpp"

namespace openport::analytics {

inline constexpr md::InstrumentId kNoInstrument = std::numeric_limits<md::InstrumentId>::max();

/// Latest known state of one option contract.
struct OptionState {
  md::OptionContract contract;
  md::Timestamp expiry_time = 0;

  bool has_quote = false;  ///< receipt is independent of price (zero bid is a real market)
  bool has_open_interest = false;
  double bid = 0.0;
  double ask = 0.0;
  double bid_size = 0.0;
  double ask_size = 0.0;
  md::Timestamp quote_ts = 0;

  double open_interest = 0.0;
  md::VendorGreeks vendor;  ///< vendor.iv == 0 when the provider sent none

  [[nodiscard]] bool two_sided() const noexcept {
    return has_quote && bid > 0.0 && ask >= bid && std::isfinite(bid) && std::isfinite(ask);
  }
  [[nodiscard]] double mid() const noexcept { return 0.5 * (bid + ask); }
};

struct StrikePair {
  md::InstrumentId call = kNoInstrument;
  md::InstrumentId put = kNoInstrument;
};

/// All strikes of one expiry. SPX (settled at the open) and SPXW (settled at the
/// close) expiring on the same day are different slices: they have different
/// amounts of time left.
struct ExpirySlice {
  md::Date expiry;
  md::Timestamp expiry_time = 0;
  std::string root;  ///< OEX/XEO disambiguation when settlement time alone is not unique
  std::map<double, StrikePair> strikes;
};

struct UnderlyingBook {
  std::string symbol;
  double spot = 0.0;
  md::Timestamp spot_ts = 0;
  md::Timestamp data_time = 0;  ///< latest market-data timestamp seen for this underlying
  // OEX and XEO settle together but must not form mixed-exercise strike pairs.
  std::map<std::pair<md::Timestamp, pricing::ExerciseStyle>, ExpirySlice> expiries;
  std::uint64_t version = 0;  ///< bumped on every change
};

/// The market as the feed has described it so far, rebuilt event by event. Owned
/// by a single thread; not thread-safe.
class ChainBook {
 public:
  void apply(const md::Event& event);

  /// nullptr for an id that has not been defined.
  [[nodiscard]] const OptionState* option(md::InstrumentId id) const noexcept;

  [[nodiscard]] const std::map<std::string, UnderlyingBook>& underlyings() const noexcept {
    return underlyings_;
  }

  [[nodiscard]] std::size_t contracts() const noexcept { return defined_count_; }
  [[nodiscard]] std::size_t nonstandard_contracts() const noexcept { return nonstandard_count_; }

 private:
  void on_definition(const md::ContractDefinition& e);
  OptionState* defined(md::InstrumentId id) noexcept;
  void touch(const OptionState& state, md::Timestamp ts);

  std::vector<OptionState> options_;
  std::vector<bool> defined_;
  std::size_t defined_count_ = 0;
  std::size_t nonstandard_count_ = 0;
  std::map<std::string, UnderlyingBook> underlyings_;
};

}  // namespace openport::analytics
