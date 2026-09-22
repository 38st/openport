#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "openport/md/provider.hpp"

namespace databento {
class Record;
struct InstrumentDefMsg;
struct CbboMsg;
struct Cmbp1Msg;
struct TradeMsg;
struct StatMsg;
}  // namespace databento

namespace openport::providers {

/// Databento's OPRA feed is subscribed by option root ("parent" symbology). An
/// index usually has more than one root: SPX -> {"SPX.OPT", "SPXW.OPT"}.
[[nodiscard]] std::vector<std::string> databento_parent_symbols(std::string_view underlying);

/// Turns Databento OPRA records into OpenPort events. Separate from the network
/// client so it can be exercised with hand-built records.
class DatabentoMapper {
 public:
  explicit DatabentoMapper(md::EventSink& sink) : sink_(sink) {}

  /// Dispatches any record; types the mapper does not use are ignored.
  void on_record(const databento::Record& record);

  void on_definition(const databento::InstrumentDefMsg& def);
  void on_cbbo(const databento::CbboMsg& msg);
  void on_cmbp1(const databento::Cmbp1Msg& msg);
  void on_trade(const databento::TradeMsg& msg);
  void on_statistic(const databento::StatMsg& msg);

  /// Records dropped because their instrument's definition had not arrived yet.
  [[nodiscard]] std::size_t undefined_records() const noexcept { return undefined_records_; }

 private:
  [[nodiscard]] bool lookup(std::uint32_t databento_id, md::InstrumentId& id);

  md::EventSink& sink_;
  std::unordered_map<std::uint32_t, md::InstrumentId> ids_;  // Databento id -> ours
  std::size_t undefined_records_ = 0;
};

/// Real-time OPRA options data from Databento, using your own API key.
///
/// Definitions and open interest are replayed from the start of the trading day so
/// the whole chain arrives on connect; quotes (consolidated BBO, one-second samples
/// by default) and trades stream live. Databento sends no Greeks: OpenPort computes
/// them from these quotes.
class DatabentoProvider final : public md::Provider {
 public:
  enum class QuoteSchema : std::uint8_t {
    Cbbo1s,  ///< consolidated BBO sampled every second: light enough for whole chains
    Cmbp1,   ///< every change to the consolidated BBO: far more data
  };

  struct Options {
    std::string api_key;
    QuoteSchema quotes = QuoteSchema::Cbbo1s;
    bool trades = true;
  };

  explicit DatabentoProvider(Options options);
  ~DatabentoProvider() override;

  [[nodiscard]] std::string_view name() const noexcept override { return "databento"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

 private:
  struct Session;

  Options options_;
  std::mutex mutex_;
  std::unique_ptr<Session> session_;
};

}  // namespace openport::providers
