#include "openport/providers/snapshot.hpp"

#include <cmath>
#include <iterator>

#include "openport/net/http.hpp"

namespace openport::providers {

md::InstrumentId SnapshotPublisher::define(const std::string& key, md::OptionContract contract,
                                           md::EventSink& sink) {
  const auto [it, inserted] = ids_.try_emplace(key, static_cast<md::InstrumentId>(last_.size()));
  if (inserted) {
    last_.emplace_back();
    sink.publish(md::ContractDefinition{it->second, std::move(contract)});
  }
  return it->second;
}

void SnapshotPublisher::quote(md::InstrumentId id, md::Timestamp ts, double bid, double ask,
                              double bid_size, double ask_size, md::EventSink& sink) {
  Last& last = last_[id];
  if (last.bid == bid && last.ask == ask && last.bid_size == bid_size && last.ask_size == ask_size) {
    return;
  }
  sink.publish(md::OptionQuote{id, ts, bid, ask, bid_size, ask_size});
  last.bid = bid;
  last.ask = ask;
  last.bid_size = bid_size;
  last.ask_size = ask_size;
}

void SnapshotPublisher::open_interest(md::InstrumentId id, md::Timestamp ts, double contracts,
                                      md::EventSink& sink) {
  Last& last = last_[id];
  if (last.open_interest == contracts) return;
  sink.publish(md::OpenInterest{id, ts, contracts});
  last.open_interest = contracts;
}

void SnapshotPublisher::greeks(const md::VendorGreeks& greeks, md::EventSink& sink) {
  if (!(greeks.iv > 0.0)) return;
  Last& last = last_[greeks.id];
  if (last.iv == greeks.iv && (last.delta == greeks.delta ||
                               (std::isnan(last.delta) && std::isnan(greeks.delta)))) {
    return;
  }
  sink.publish(greeks);
  last.iv = greeks.iv;
  last.delta = greeks.delta;
}

ChainFilter::ChainFilter(const md::Subscription& subscription, md::Date today, double spot,
                         const std::set<md::Date>& expiries)
    : spot_(spot),
      strike_window_(subscription.strike_window),
      limit_expiries_(subscription.max_expiries > 0),
      today_(today) {
  if (!limit_expiries_) return;
  for (const md::Date& expiry : expiries) {
    if (expiry < today) continue;
    if (expiries_.size() >= static_cast<std::size_t>(subscription.max_expiries)) break;
    expiries_.insert(expiry);
  }
}

bool ChainFilter::admits(const md::OptionContract& contract) const {
  if (contract.expiry < today_) return false;
  if (limit_expiries_ && !expiries_.contains(contract.expiry)) return false;
  if (strike_window_ > 0.0 && spot_ > 0.0 &&
      std::abs(contract.strike / spot_ - 1.0) > strike_window_ + 1e-12) {
    return false;
  }
  return true;
}

void PollingProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  stop();
  stopping_ = false;
  thread_ = std::thread(&PollingProvider::run, this, subscription, &sink);
}

void PollingProvider::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool PollingProvider::sleep(std::chrono::seconds duration) {
  std::unique_lock lock(wake_mutex_);
  return !wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
}

void PollingProvider::run(md::Subscription subscription, md::EventSink* sink) {
  const std::string label(name());
  sink->publish(md::ProviderStatus{md::now(), md::FeedState::Connecting, label});
  net::HttpClient http;
  while (!stopping_) {
    for (const std::string& underlying : subscription.underlyings) {
      if (stopping_) break;
      try {
        std::string summary = poll(http, underlying, subscription, *sink);
        sink->publish(md::ProviderStatus{md::now(), healthy_state(), std::move(summary)});
      } catch (const std::exception& error) {
        sink->publish(md::ProviderStatus{md::now(), md::FeedState::Error,
                                         label + " " + underlying + ": " + error.what()});
      }
    }
    if (!sleep(interval_)) break;
  }
  sink->publish(md::ProviderStatus{md::now(), md::FeedState::Stopped, label});
}

}  // namespace openport::providers
