#include "openport/providers/cboe.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including
// <ranges>; libc++ does not pull it in transitively.
#include <ranges>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>
#include <stdexcept>

#include "openport/md/contract.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {
namespace {

// Cboe serves index chains under a leading underscore.
constexpr std::array<std::string_view, 9> kIndexSymbols = {"SPX", "XSP", "NDX", "XND", "RUT",
                                                           "MRUT", "VIX", "DJX", "OEX"};

/// Reads a numeric field that Cboe sometimes sends as null or omits.
double number_or_zero(simdjson::ondemand::object& object, std::string_view key) {
  double value = 0.0;
  if (object[key].get_double().get(value) != simdjson::SUCCESS || !std::isfinite(value)) return 0.0;
  return value;
}

std::string text_or_empty(simdjson::ondemand::object& object, std::string_view key) {
  std::string_view value;
  if (object[key].get_string().get(value) != simdjson::SUCCESS) return {};
  return std::string(value);
}

bool changed(double a, double b) noexcept { return a != b; }

}  // namespace

std::string cboe_chain_url(std::string_view underlying) {
  const bool index =
      std::find(kIndexSymbols.begin(), kIndexSymbols.end(), underlying) != kIndexSymbols.end();
  std::string url = "https://cdn.cboe.com/api/global/delayed_quotes/options/";
  if (index) url += '_';
  url += underlying;
  url += ".json";
  return url;
}

CboeChain parse_cboe_chain(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);

  CboeChain chain;
  std::string_view timestamp;
  if (doc["timestamp"].get_string().get(timestamp) == simdjson::SUCCESS) {
    chain.as_of = md::parse_datetime(timestamp, md::Zone::Utc).value_or(0);
  }

  simdjson::ondemand::object data;
  if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
    throw std::runtime_error("Cboe chain: missing data object");
  }
  // On-demand parsing reads fields in document order, so take them in the order
  // Cboe writes them: the options array first, then the underlying's fields.
  simdjson::ondemand::array options;
  if (data["options"].get_array().get(options) != simdjson::SUCCESS) {
    throw std::runtime_error("Cboe chain: missing options array");
  }
  for (simdjson::ondemand::object row : options) {
    CboeOption option;
    option.symbol = text_or_empty(row, "option");
    option.bid = number_or_zero(row, "bid");
    option.bid_size = number_or_zero(row, "bid_size");
    option.ask = number_or_zero(row, "ask");
    option.ask_size = number_or_zero(row, "ask_size");
    option.iv = number_or_zero(row, "iv");
    option.open_interest = number_or_zero(row, "open_interest");
    option.volume = number_or_zero(row, "volume");
    option.delta = number_or_zero(row, "delta");
    option.gamma = number_or_zero(row, "gamma");
    option.vega = number_or_zero(row, "vega");
    option.theta = number_or_zero(row, "theta");
    option.rho = number_or_zero(row, "rho");
    option.theo = number_or_zero(row, "theo");
    if (!option.symbol.empty()) chain.options.push_back(std::move(option));
  }
  chain.symbol = text_or_empty(data, "symbol");
  chain.price = number_or_zero(data, "current_price");
  chain.bid = number_or_zero(data, "bid");
  chain.ask = number_or_zero(data, "ask");
  return chain;
}

CboeDelayedProvider::CboeDelayedProvider() : CboeDelayedProvider(Options{}) {}

CboeDelayedProvider::CboeDelayedProvider(Options options) : options_(options) {}

CboeDelayedProvider::~CboeDelayedProvider() { stop(); }

md::Capabilities CboeDelayedProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.realtime = false;
  caps.delay = kDelay;
  caps.quotes = true;
  caps.trades = false;
  caps.open_interest = true;
  caps.vendor_greeks = true;
  caps.history = false;
  return caps;
}

void CboeDelayedProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  stop();
  stopping_ = false;
  thread_ = std::thread(&CboeDelayedProvider::run, this, subscription, &sink);
}

void CboeDelayedProvider::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool CboeDelayedProvider::sleep_for(std::chrono::seconds duration) {
  std::unique_lock lock(wake_mutex_);
  return !wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
}

void CboeDelayedProvider::run(md::Subscription subscription, md::EventSink* sink) {
  sink->publish(md::ProviderStatus{md::now(), md::FeedState::Connecting, "Cboe delayed quotes"});
  net::HttpsClient http;
  while (!stopping_) {
    for (const std::string& underlying : subscription.underlyings) {
      if (stopping_) break;
      try {
        const net::HttpResponse response = http.get(cboe_chain_url(underlying), {}, options_.timeout);
        if (response.status != 200) {
          sink->publish(md::ProviderStatus{md::now(), md::FeedState::Error,
                                           underlying + ": HTTP " + std::to_string(response.status)});
          continue;
        }
        const auto parse_started = std::chrono::steady_clock::now();
        const CboeChain chain = parse_cboe_chain(response.body);
        const auto parse_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - parse_started)
                                  .count();
        publish_chain(chain, subscription, *sink);

        char message[192];
        std::snprintf(message, sizeof message,
                      "%s: %zu options, %.2f MB in %.0f ms, parsed in %.1f ms", underlying.c_str(),
                      chain.options.size(), static_cast<double>(response.wire_bytes) / 1e6,
                      static_cast<double>(response.elapsed.count()) / 1e3,
                      static_cast<double>(parse_us) / 1e3);
        sink->publish(md::ProviderStatus{md::now(), md::FeedState::Delayed, message});
      } catch (const std::exception& error) {
        sink->publish(md::ProviderStatus{md::now(), md::FeedState::Error,
                                         underlying + ": " + error.what()});
      }
    }
    if (!sleep_for(options_.poll_interval)) break;
  }
  sink->publish(md::ProviderStatus{md::now(), md::FeedState::Stopped, "Cboe delayed quotes"});
}

void CboeDelayedProvider::publish_chain(const CboeChain& chain,
                                        const md::Subscription& subscription,
                                        md::EventSink& sink) {
  // Quotes describe the market 15 minutes before the snapshot was generated.
  const md::Timestamp ts =
      chain.as_of - std::chrono::duration_cast<std::chrono::nanoseconds>(kDelay).count();

  std::string underlying_symbol = chain.symbol;
  if (!underlying_symbol.empty() && underlying_symbol.front() == '^') underlying_symbol.erase(0, 1);
  sink.publish(md::UnderlyingQuote{underlying_symbol, ts, chain.bid, chain.ask, chain.price});

  // Optional filters: the nearest N expiries and a strike window around spot.
  std::set<md::Date> expiries;
  if (subscription.max_expiries > 0) {
    const md::Date today = md::date_from_days(chain.as_of / md::kNanosPerDay);
    for (const CboeOption& option : chain.options) {
      if (auto contract = md::parse_osi(option.symbol); contract && contract->expiry >= today) {
        expiries.insert(contract->expiry);
      }
    }
    while (expiries.size() > static_cast<std::size_t>(subscription.max_expiries)) {
      expiries.erase(std::prev(expiries.end()));
    }
  }

  for (const CboeOption& option : chain.options) {
    std::optional<md::OptionContract> contract = md::parse_osi(option.symbol);
    if (!contract) continue;
    if (subscription.max_expiries > 0 && !expiries.contains(contract->expiry)) continue;
    if (subscription.strike_window > 0.0 && chain.price > 0.0 &&
        std::abs(contract->strike / chain.price - 1.0) > subscription.strike_window) {
      continue;
    }

    auto [it, inserted] = ids_.try_emplace(option.symbol, static_cast<md::InstrumentId>(ids_.size()));
    const md::InstrumentId id = it->second;
    if (inserted) {
      published_.emplace_back();
      sink.publish(md::ContractDefinition{id, std::move(*contract)});
    }
    Published& last = published_[id];

    if (changed(last.bid, option.bid) || changed(last.ask, option.ask) ||
        changed(last.bid_size, option.bid_size) || changed(last.ask_size, option.ask_size)) {
      sink.publish(md::OptionQuote{id, ts, option.bid, option.ask, option.bid_size, option.ask_size});
      last.bid = option.bid;
      last.ask = option.ask;
      last.bid_size = option.bid_size;
      last.ask_size = option.ask_size;
    }
    if (changed(last.open_interest, option.open_interest)) {
      sink.publish(md::OpenInterest{id, ts, option.open_interest});
      last.open_interest = option.open_interest;
    }
    if (option.iv > 0.0 && (changed(last.iv, option.iv) || changed(last.delta, option.delta))) {
      sink.publish(md::VendorGreeks{id, ts, option.iv, option.delta, option.gamma, option.vega,
                                    option.theta, option.rho});
      last.iv = option.iv;
      last.delta = option.delta;
    }
  }
}

}  // namespace openport::providers
