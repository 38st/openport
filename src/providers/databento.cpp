#include "openport/providers/databento.hpp"

#include <databento/constants.hpp>
#include <databento/dbn.hpp>
#include <databento/enums.hpp>
#include <databento/live.hpp>
#include <databento/live_threaded.hpp>
#include <databento/record.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>

#include "openport/md/contract.hpp"

namespace openport::providers {
namespace {

namespace db = databento;

/// Databento prices are fixed-point integers in units of 1e-9.
[[nodiscard]] double price(std::int64_t fixed) noexcept {
  if (fixed == db::kUndefPrice) return 0.0;
  return static_cast<double>(fixed) / static_cast<double>(db::kFixedPriceScale);
}

[[nodiscard]] md::Timestamp nanos(db::UnixNanos ts) noexcept {
  return static_cast<md::Timestamp>(ts.time_since_epoch().count());
}

}  // namespace

std::vector<std::string> databento_parent_symbols(std::string_view underlying) {
  std::vector<std::string> symbols = md::option_roots(underlying);
  for (std::string& symbol : symbols) symbol += ".OPT";
  return symbols;
}

void DatabentoMapper::on_record(const db::Record& record) {
  if (const auto* def = record.GetIf<db::InstrumentDefMsg>()) {
    on_definition(*def);
  } else if (const auto* cbbo = record.GetIf<db::CbboMsg>()) {
    on_cbbo(*cbbo);
  } else if (const auto* cmbp = record.GetIf<db::Cmbp1Msg>()) {
    on_cmbp1(*cmbp);
  } else if (const auto* trade = record.GetIf<db::TradeMsg>()) {
    on_trade(*trade);
  } else if (const auto* stat = record.GetIf<db::StatMsg>()) {
    on_statistic(*stat);
  } else if (const auto* error = record.GetIf<db::ErrorMsg>()) {
    sink_.publish(md::ProviderStatus{md::now(), md::FeedState::Error, error->Err()});
  }
}

void DatabentoMapper::on_definition(const db::InstrumentDefMsg& def) {
  if (def.instrument_class != db::InstrumentClass::Call &&
      def.instrument_class != db::InstrumentClass::Put) {
    return;
  }
  // OPRA raw symbols are OSI symbols; parsing them gives the same contract model
  // (and the same exercise/settlement conventions) as every other provider.
  std::optional<md::OptionContract> contract = md::parse_osi(def.RawSymbol());
  if (!contract) return;

  const auto [it, inserted] =
      ids_.try_emplace(def.hd.instrument_id, static_cast<md::InstrumentId>(ids_.size()));
  if (inserted) sink_.publish(md::ContractDefinition{it->second, std::move(*contract)});
}

bool DatabentoMapper::lookup(std::uint32_t databento_id, md::InstrumentId& id) {
  const auto it = ids_.find(databento_id);
  if (it == ids_.end()) {
    ++undefined_records_;
    return false;
  }
  id = it->second;
  return true;
}

void DatabentoMapper::on_cbbo(const db::CbboMsg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  const auto& level = msg.levels[0];
  sink_.publish(md::OptionQuote{id, nanos(msg.hd.ts_event), price(level.bid_px), price(level.ask_px),
                                static_cast<double>(level.bid_sz),
                                static_cast<double>(level.ask_sz)});
}

void DatabentoMapper::on_cmbp1(const db::Cmbp1Msg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  const auto& level = msg.levels[0];
  sink_.publish(md::OptionQuote{id, nanos(msg.hd.ts_event), price(level.bid_px), price(level.ask_px),
                                static_cast<double>(level.bid_sz),
                                static_cast<double>(level.ask_sz)});
}

void DatabentoMapper::on_trade(const db::TradeMsg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  sink_.publish(md::OptionTrade{id, nanos(msg.hd.ts_event), price(msg.price),
                                static_cast<double>(msg.size)});
}

void DatabentoMapper::on_statistic(const db::StatMsg& msg) {
  if (msg.stat_type != db::StatType::OpenInterest || msg.quantity == db::kUndefStatQuantity) return;
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  sink_.publish(md::OpenInterest{id, nanos(msg.hd.ts_event), static_cast<double>(msg.quantity)});
}

struct DatabentoProvider::Session {
  explicit Session(md::EventSink& out) : sink(out), mapper(out) {}

  md::EventSink& sink;
  DatabentoMapper mapper;
  std::optional<db::LiveThreaded> client;
  std::atomic<bool> stopping{false};
  int restarts = 0;
};

DatabentoProvider::DatabentoProvider(Options options) : options_(std::move(options)) {
  if (options_.api_key.empty()) {
    throw std::invalid_argument("databento: an API key is required (set DATABENTO_API_KEY)");
  }
}

DatabentoProvider::~DatabentoProvider() { stop(); }

md::Capabilities DatabentoProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.realtime = true;
  caps.quotes = true;
  caps.trades = options_.trades;
  caps.open_interest = true;
  caps.vendor_greeks = false;
  caps.history = true;
  return caps;
}

void DatabentoProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  stop();
  const std::lock_guard lock(mutex_);
  auto session = std::make_unique<Session>(sink);

  std::vector<std::string> symbols;
  for (const std::string& underlying : subscription.underlyings) {
    for (std::string& symbol : databento_parent_symbols(underlying)) symbols.push_back(std::move(symbol));
  }

  sink.publish(md::ProviderStatus{md::now(), md::FeedState::Connecting, "Databento OPRA"});
  session->client.emplace(db::LiveThreaded::Builder()
                              .SetKey(options_.api_key)
                              .SetDataset(db::Dataset::OpraPillar)
                              .BuildThreaded());

  // The live gateway replays up to 24 hours on request: replay today's definitions
  // and open interest so the full chain is known before the first quote.
  const auto replay_from = db::UnixNanos{std::chrono::duration_cast<db::UnixNanos::duration>(
      std::chrono::system_clock::now().time_since_epoch() - std::chrono::hours(23))};
  session->client->Subscribe(symbols, db::Schema::Definition, db::SType::Parent, replay_from);
  session->client->Subscribe(symbols, db::Schema::Statistics, db::SType::Parent, replay_from);
  session->client->Subscribe(symbols,
                             options_.quotes == QuoteSchema::Cbbo1s ? db::Schema::Cbbo1S
                                                                    : db::Schema::Cmbp1,
                             db::SType::Parent);
  if (options_.trades) session->client->Subscribe(symbols, db::Schema::Trades, db::SType::Parent);

  Session* raw = session.get();
  md::EventSink* out = &sink;
  session->client->Start(
      [out](db::Metadata&&) {
        out->publish(md::ProviderStatus{md::now(), md::FeedState::Live, "Databento OPRA"});
      },
      [raw](const db::Record& record) {
        if (raw->stopping) return db::KeepGoing::Stop;
        raw->mapper.on_record(record);
        return db::KeepGoing::Continue;
      },
      [raw, out](const std::exception& error) {
        out->publish(md::ProviderStatus{md::now(), md::FeedState::Error, error.what()});
        // Reconnect after transient failures, but do not hammer the gateway (or a bad key).
        if (raw->stopping || ++raw->restarts > 5) return db::LiveThreaded::ExceptionAction::Stop;
        return db::LiveThreaded::ExceptionAction::Restart;
      });
  session_ = std::move(session);
}

void DatabentoProvider::stop() {
  const std::lock_guard lock(mutex_);
  if (!session_) return;
  session_->stopping = true;
  session_->client.reset();  // closes the connection and joins the client's thread
  session_->sink.publish(md::ProviderStatus{md::now(), md::FeedState::Stopped, "Databento OPRA"});
  session_.reset();
}

}  // namespace openport::providers
