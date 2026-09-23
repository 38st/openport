#include "openport/providers/cboe.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including
// <ranges>; libc++ does not pull it in transitively.
#include <ranges>
#include <simdjson.h>

#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <utility>

#include "openport/md/contract.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {
namespace {

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

}  // namespace

std::string cboe_chain_url(std::string_view underlying) {
  std::string url = "https://cdn.cboe.com/api/global/delayed_quotes/options/";
  if (md::is_index_underlying(underlying)) url += '_';
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
  // On-demand parsing is fastest when fields are read in document order, so take
  // them in the order Cboe writes them: the options array first, then the underlying.
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

md::Capabilities CboeDelayedProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.poll_interval = options_.poll_interval;
  caps.realtime = false;
  caps.delay = kDelay;
  caps.quotes = true;
  caps.trades = false;
  caps.open_interest = true;
  caps.vendor_greeks = true;
  caps.history = false;
  return caps;
}

std::string CboeDelayedProvider::poll(net::HttpClient& http, const std::string& underlying,
                                      const md::Subscription& subscription, md::EventSink& sink) {
  const net::HttpResponse response = http.get(cboe_chain_url(underlying), {}, options_.timeout);
  if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));

  const auto parse_started = std::chrono::steady_clock::now();
  const CboeChain chain = parse_cboe_chain(response.body);
  const auto parse_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - parse_started)
                            .count();
  publish_chain(chain, subscription, sink);

  char summary[192];
  std::snprintf(summary, sizeof summary, "cboe %s: %zu options, %.2f MB in %.0f ms, parsed in %.1f ms",
                underlying.c_str(), chain.options.size(),
                static_cast<double>(response.wire_bytes) / 1e6,
                static_cast<double>(response.elapsed.count()) / 1e3,
                static_cast<double>(parse_us) / 1e3);
  return summary;
}

void CboeDelayedProvider::publish_chain(const CboeChain& chain,
                                        const md::Subscription& subscription,
                                        md::EventSink& sink) {
  // Quotes describe the market 15 minutes before the snapshot was generated.
  const md::Timestamp ts =
      chain.as_of - std::chrono::duration_cast<std::chrono::nanoseconds>(kDelay).count();

  std::string underlying = chain.symbol;
  if (!underlying.empty() && underlying.front() == '^') underlying.erase(0, 1);
  sink.publish(md::UnderlyingQuote{underlying, ts, chain.bid, chain.ask, chain.price});

  std::vector<std::pair<const CboeOption*, md::OptionContract>> contracts;
  contracts.reserve(chain.options.size());
  std::set<md::Date> expiries;
  for (const CboeOption& option : chain.options) {
    if (std::optional<md::OptionContract> contract = md::parse_osi(option.symbol)) {
      expiries.insert(contract->expiry);
      contracts.emplace_back(&option, std::move(*contract));
    }
  }
  const ChainFilter filter(subscription, md::date_from_days(chain.as_of / md::kNanosPerDay),
                           chain.price, expiries);

  std::set<md::InstrumentId> seen;
  for (auto& [option, contract] : contracts) {
    if (!publisher_.known(option->symbol) && !filter.admits(contract)) continue;
    const md::InstrumentId id = publisher_.define(option->symbol, std::move(contract), sink);
    seen.insert(id);
    publisher_.quote(id, ts, option->bid, option->ask, option->bid_size, option->ask_size, sink);
    publisher_.open_interest(id, ts, option->open_interest, sink);
    publisher_.greeks(md::VendorGreeks{id, ts, option->iv, option->delta, option->gamma,
                                       option->vega, option->theta, option->rho},
                      sink);
  }
  publisher_.finish(underlying, seen, ts, sink);
}

}  // namespace openport::providers
