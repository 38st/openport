#include "openport/server/engine.hpp"

#include <algorithm>
#include <type_traits>

namespace openport::server {

Engine::Engine(md::Provider& provider, md::Subscription subscription, Options options)
    : provider_(provider), subscription_(std::move(subscription)), options_(options) {
  status_.provider = std::string(provider.name());
  status_.capabilities = provider.capabilities();
  for (const auto& symbol : subscription_.underlyings) status_.underlyings.try_emplace(symbol);
}

Engine::~Engine() { stop(); }

void Engine::start() {
  stop();
  stopping_ = false;
  {
    const std::lock_guard lock(mutex_);
    status_.started = options_.clock();
    for (auto& [symbol, health] : status_.underlyings) health = {};
  }
  provider_.start(subscription_, queue_);
  thread_ = std::thread(&Engine::run, this);
}

void Engine::stop() {
  stopping_ = true;
  if (thread_.joinable()) {
    thread_.join();
    provider_.stop();
  }
}

std::vector<std::string> Engine::symbols() const {
  const std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const auto& [symbol, health] : status_.underlyings) out.push_back(symbol);
  return out;
}

std::shared_ptr<const analytics::UnderlyingMetrics> Engine::metrics(const std::string& symbol) const {
  const std::lock_guard lock(mutex_);
  const auto it = metrics_.find(symbol);
  return it == metrics_.end() ? nullptr : it->second;
}

EngineStatus Engine::status() const {
  const std::lock_guard lock(mutex_);
  EngineStatus out = status_;
  const auto now = options_.clock();
  const auto stale_after =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::max(3 * out.capabilities.poll_interval, std::chrono::seconds(60)))
          .count();
  auto severity = [](md::FeedState state) {
    switch (state) {
      case md::FeedState::Stale:
        return 5;
      case md::FeedState::Error:
        return 4;
      case md::FeedState::Connecting:
        return 3;
      case md::FeedState::Stopped:
        return 2;
      case md::FeedState::Delayed:
        return 1;
      case md::FeedState::Live:
        return 0;
    }
    return 5;
  };
  int worst = -1;
  out.feed_message.clear();
  for (auto& [symbol, health] : out.underlyings) {
    const auto since = health.last_success > 0 ? health.last_success : out.started;
    if (health.state != md::FeedState::Stopped && since > 0 && now - since > stale_after) {
      health.state = md::FeedState::Stale;
      health.message = symbol + ": no successful update within " +
                       std::to_string(stale_after / md::kNanosPerSecond) + " seconds";
    }
    if (severity(health.state) > worst) {
      worst = severity(health.state);
      out.feed_state = health.state;
    }
    if (!out.feed_message.empty()) out.feed_message += "; ";
    out.feed_message += health.message.empty() ? symbol + ": connecting" : health.message;
  }
  return out;
}

void Engine::update_health(const md::Event& event) {
  const auto received = options_.clock();
  const std::lock_guard lock(mutex_);
  if (const auto* feed = std::get_if<md::ProviderStatus>(&event)) {
    auto update = [&](UnderlyingHealth& health) {
      health.state = feed->state;
      health.message = feed->message;
      if (feed->state == md::FeedState::Live || feed->state == md::FeedState::Delayed) {
        health.last_success = received;
      } else if (feed->state == md::FeedState::Error) {
        health.last_error = feed->message;
        health.last_error_time = received;
      }
    };
    if (feed->underlying.empty()) {
      for (auto& [symbol, health] : status_.underlyings) update(health);
    } else {
      update(status_.underlyings[feed->underlying]);
    }
    status_.feed_updated = received;
  } else if (status_.capabilities.poll_interval == std::chrono::seconds(0)) {
    // Streaming feeds prove liveness with quotes. Snapshot feeds report success
    // after the whole poll, including unchanged or empty chains.
    std::string symbol;
    if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
      if (const auto* option = book_.option(quote->id)) symbol = option->contract.underlying;
    } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
      symbol = spot->symbol;
    }
    if (!symbol.empty()) {
      auto& health = status_.underlyings[symbol];
      health.state =
          status_.capabilities.delay.count() > 0 ? md::FeedState::Delayed : md::FeedState::Live;
      health.message = status_.provider + " " + symbol + ": receiving quotes";
      health.last_success = received;
      status_.feed_updated = received;
    }
  }
}

void Engine::run() {
  std::vector<md::Event> batch;
  auto last_analytics = std::chrono::steady_clock::now();
  last_rate_time_ = last_analytics;

  while (!stopping_) {
    batch.clear();
    queue_.drain(batch, std::chrono::milliseconds(50));
    for (const md::Event& event : batch) {
      book_.apply(event);
      ++events_;
      update_health(event);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_analytics >= options_.analytics_interval) {
      last_analytics = now;
      refresh_analytics();
    }
  }
}

void Engine::refresh_analytics() {
  const auto started = std::chrono::steady_clock::now();
  bool recomputed = false;
  for (const auto& [symbol, book] : book_.underlyings()) {
    auto& seen = analysed_versions_[symbol];
    if (book.version == seen || book.expiries.empty()) continue;
    seen = book.version;
    // Price as of the data's own clock, so delayed feeds get the right time to expiry.
    const md::Timestamp as_of = book.data_time > 0 ? book.data_time : md::now();
    auto result = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(book, book_, as_of, options_.analytics));
    const std::lock_guard lock(mutex_);
    metrics_[symbol] = std::move(result);
    recomputed = true;
  }

  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_rate_time_).count();
  const std::lock_guard lock(mutex_);
  status_.events = events_;
  status_.contracts = book_.contracts();
  status_.nonstandard_contracts = book_.nonstandard_contracts();
  if (recomputed) {
    status_.analytics_ms = std::chrono::duration<double, std::milli>(now - started).count();
  }
  if (elapsed >= 1.0) {
    status_.events_per_second = static_cast<double>(events_ - events_at_last_rate_) / elapsed;
    events_at_last_rate_ = events_;
    last_rate_time_ = now;
  }
}

}  // namespace openport::server
