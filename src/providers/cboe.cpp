#include "openport/providers/cboe.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including
// <ranges>; libc++ does not pull it in transitively.
// clang-format off
#include <ranges>
#include <simdjson.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
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

/// A number Cboe may send as a JSON number or as a string ("7770.810000").
double loose_number(simdjson::ondemand::object& object, std::string_view key) {
  simdjson::ondemand::value value;
  if (object[key].get(value) != simdjson::SUCCESS) return 0.0;
  simdjson::ondemand::json_type type;
  if (value.type().get(type) != simdjson::SUCCESS) return 0.0;
  double number = 0.0;
  const auto status = type == simdjson::ondemand::json_type::string
                          ? value.get_double_in_string().get(number)
                          : value.get_double().get(number);
  return status == simdjson::SUCCESS && std::isfinite(number) ? number : 0.0;
}

/// Oldest first, one bar per start; a later row for the same start wins.
std::vector<md::Bar> ordered(std::vector<md::Bar> bars) {
  std::stable_sort(bars.begin(), bars.end(),
                   [](const md::Bar& a, const md::Bar& b) { return a.start < b.start; });
  std::vector<md::Bar> out;
  out.reserve(bars.size());
  for (const auto& bar : bars) {
    if (!out.empty() && out.back().start == bar.start)
      out.back() = bar;
    else
      out.push_back(bar);
  }
  return out;
}

simdjson::ondemand::array chart_rows(simdjson::ondemand::document& doc, const char* what) {
  simdjson::ondemand::array rows;
  if (doc["data"].get_array().get(rows) != simdjson::SUCCESS)
    throw std::runtime_error(std::string("Cboe ") + what + " chart: missing data array");
  return rows;
}

}  // namespace

std::string cboe_chart_url(std::string_view underlying, CboeChart chart) {
  std::string url = "https://cdn.cboe.com/api/global/delayed_quotes/charts/";
  url += chart == CboeChart::Intraday ? "intraday/" : "historical/";
  if (md::is_index_underlying(underlying)) url += '_';
  url += underlying;
  url += ".json";
  return url;
}

std::vector<md::Bar> parse_cboe_intraday(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);
  std::vector<md::Bar> bars;
  for (simdjson::ondemand::object row : chart_rows(doc, "intraday")) {
    const auto closes = md::parse_datetime(text_or_empty(row, "datetime"), md::Zone::NewYork);
    simdjson::ondemand::object prices;
    if (!closes || row["price"].get_object().get(prices) != simdjson::SUCCESS) continue;
    const md::Bar bar{*closes - md::kNanosPerMinute, loose_number(prices, "open"),
                      loose_number(prices, "high"), loose_number(prices, "low"),
                      loose_number(prices, "close")};
    if (md::valid_bar(bar)) bars.push_back(bar);
  }
  return ordered(std::move(bars));
}

std::vector<md::Bar> parse_cboe_daily(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);
  std::vector<md::Bar> bars;
  for (simdjson::ondemand::object row : chart_rows(doc, "daily")) {
    const std::string text = text_or_empty(row, "date");
    md::Date day;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d%c", &day.year, &day.month, &day.day, &tail) != 3 ||
        !md::valid_date(day))
      continue;
    const md::Timestamp open = md::new_york_to_utc(day, 9, 30);
    const md::Bar bar{open, loose_number(row, "open"), loose_number(row, "high"),
                      loose_number(row, "low"), loose_number(row, "close")};
    if (open != md::kInvalidTimestamp && md::valid_bar(bar)) bars.push_back(bar);
  }
  return ordered(std::move(bars));
}

CboeChartHistory::CboeChartHistory(std::vector<std::string> underlyings, Sink sink, Options options)
    : underlyings_(std::move(underlyings)), sink_(std::move(sink)), options_(std::move(options)) {}

void CboeChartHistory::start() {
  stop();
  stopping_ = false;
  thread_ = std::thread(&CboeChartHistory::run, this);
}

void CboeChartHistory::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CboeChartHistory::run() {
  net::HttpClient http;
  while (!stopping_) {
    const md::Timestamp next = poll_once(http);
    const md::Timestamp wait =
        std::clamp<md::Timestamp>(next - options_.clock(), md::kNanosPerSecond, md::kNanosPerMinute);
    std::unique_lock lock(wake_mutex_);
    if (wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return stopping_.load(); }))
      break;
  }
}

md::Timestamp CboeChartHistory::poll_once(net::HttpClient& http) {
  md::Timestamp next = std::numeric_limits<md::Timestamp>::max();
  for (const auto& underlying : underlyings_) {
    for (const auto chart : {CboeChart::Intraday, CboeChart::Daily}) {
      const auto key = std::pair{underlying, chart};
      md::Timestamp& due = due_[key];
      const md::Timestamp now = options_.clock();
      if (stopping_) return now;
      if (due <= now) {
        const auto seconds = [](std::chrono::seconds s) { return s.count() * md::kNanosPerSecond; };
        const bool active = md::market_session(now).open ||
                            md::market_session(now - 30 * md::kNanosPerMinute).open;
        due = now + seconds(chart == CboeChart::Daily ? options_.daily_interval
                            : active                   ? options_.session_interval
                                                       : options_.idle_interval);
        std::string failure;
        try {
          const auto response =
              http.get(cboe_chart_url(underlying, chart), {}, options_.timeout, &stopping_);
          if (response.status == 403 || response.status == 404) {
            due = now + seconds(options_.missing_interval);
            failure = "Cboe publishes no chart (HTTP " + std::to_string(response.status) + ")";
          } else if (response.status != 200) {
            failure = "HTTP " + std::to_string(response.status);
          } else {
            auto bars = chart == CboeChart::Intraday ? parse_cboe_intraday(response.body)
                                                     : parse_cboe_daily(response.body);
            sink_(underlying, chart, std::move(bars));
          }
        } catch (const std::exception& error) {
          if (stopping_) return now;
          failure = error.what();
          due = now + seconds(std::min(options_.session_interval, options_.daily_interval));
        }
        const std::lock_guard lock(error_mutex_);
        if (failure.empty())
          errors_.erase(key);
        else
          errors_[key] = "cboe " + underlying +
                         (chart == CboeChart::Intraday ? " minute bars: " : " daily bars: ") + failure;
      }
      next = std::min(next, due);
    }
  }
  return next;
}

std::string CboeChartHistory::error() const {
  const std::lock_guard lock(error_mutex_);
  std::string out;
  for (const auto& [key, message] : errors_) out += (out.empty() ? "" : "\n") + message;
  return out;
}

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
  chain.last_trade_time =
      md::parse_datetime(text_or_empty(data, "last_trade_time"), md::Zone::NewYork).value_or(0);
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
  const net::HttpResponse response =
      http.get(cboe_chain_url(underlying), {}, options_.timeout, cancellation());
  if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));

  const auto parse_started = std::chrono::steady_clock::now();
  const CboeChain chain = parse_cboe_chain(response.body);
  const auto parse_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - parse_started)
                            .count();
  check_cancelled();
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
  const md::Timestamp delayed =
      chain.as_of - std::chrono::duration_cast<std::chrono::nanoseconds>(kDelay).count();
  std::string underlying = chain.symbol;
  if (!underlying.empty() && underlying.front() == '^') underlying.erase(0, 1);
  const md::Timestamp ts = md::trading_session(underlying, delayed).market_time;
  // Stock/index prints have their own clock; they can be hours behind GTH options.
  sink.publish(
      md::UnderlyingQuote{underlying, chain.last_trade_time, chain.bid, chain.ask, chain.price});
  std::map<std::string, md::Timestamp> market_times;

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
    check_cancelled();
    const auto [clock, inserted] = market_times.try_emplace(contract.root);
    if (inserted) clock->second = md::trading_session(contract.root, delayed).market_time;
    const auto option_time = clock->second;
    const md::InstrumentId id = publisher_.define(option->symbol, std::move(contract), sink);
    seen.insert(id);
    publisher_.quote(id, option_time, option->bid, option->ask, option->bid_size, option->ask_size,
                     sink);
    publisher_.open_interest(id, option_time, option->open_interest, sink);
    publisher_.greeks(md::VendorGreeks{id, option_time, option->iv, option->delta, option->gamma,
                                       option->vega, option->theta, option->rho},
                      sink);
  }
  publisher_.finish(underlying, seen, ts, sink);
}

}  // namespace openport::providers
