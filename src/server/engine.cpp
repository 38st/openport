#include "openport/server/engine.hpp"

#include <type_traits>

namespace openport::server {

Engine::Engine(md::Provider& provider, md::Subscription subscription, Options options)
    : provider_(provider), subscription_(std::move(subscription)), options_(options) {
  status_.provider = std::string(provider.name());
  status_.capabilities = provider.capabilities();
}

Engine::~Engine() { stop(); }

void Engine::start() {
  stop();
  stopping_ = false;
  {
    const std::lock_guard lock(mutex_);
    status_.started = md::now();
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
  for (const auto& [symbol, metrics] : metrics_) out.push_back(symbol);
  return out;
}

std::shared_ptr<const analytics::UnderlyingMetrics> Engine::metrics(const std::string& symbol) const {
  const std::lock_guard lock(mutex_);
  const auto it = metrics_.find(symbol);
  return it == metrics_.end() ? nullptr : it->second;
}

EngineStatus Engine::status() const {
  const std::lock_guard lock(mutex_);
  return status_;
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
      if (const auto* feed = std::get_if<md::ProviderStatus>(&event)) {
        const std::lock_guard lock(mutex_);
        status_.feed_state = feed->state;
        status_.feed_message = feed->message;
        status_.feed_updated = feed->ts;
      }
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
