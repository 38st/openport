#include "openport/server/engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace openport::server {

Engine::Engine(md::Provider& provider, md::Subscription subscription, Options options)
    : provider_(provider), subscription_(std::move(subscription)), options_(options),
      queue_(md::kEventQueueCapacity, options.paper_enabled) {
  status_.provider = std::string(provider.name());
  status_.capabilities = provider.capabilities();
  status_.trading.fee_per_contract = options_.paper.fee_per_contract;
  status_.trading.initial_cash = options_.paper.initial_cash;
  for (const auto& symbol : subscription_.underlyings) status_.underlyings.try_emplace(symbol);
  health_ = status_.underlyings;
}

Engine::~Engine() { stop(); }

void Engine::start() {
  if (started_)
    throw std::logic_error("Engine::start may be called only once; construct a new Engine");
  started_ = true;
  stopping_ = false;
  {
    const std::lock_guard lock(mutex_);
    status_.started = options_.clock();
  }
  try {
    if (!options_.record_file.empty()) {
      auto recording = options_.recording;
      recording.clock = options_.clock;
      recorder_ = std::make_unique<md::RecordingSink>(
          options_.record_file, md::RecordingHeader{std::string(provider_.name()),
          provider_.capabilities(), subscription_, status_.started}, queue_, std::move(recording));
    }
    provider_started_ = true;
    provider_.start(subscription_, recorder_ ? static_cast<md::EventSink&>(*recorder_) : queue_);
    // Complete journal startup before returning so the daemon can report failures
    // even if constructing or binding its web server subsequently fails.
    start_trading();
    thread_ = options_.launch([this] { run(); });
  } catch (...) {
    // Even a partially started provider must stop before the queue can be destroyed.
    if (provider_started_) provider_.stop();
    provider_started_ = false;
    if (recorder_) recorder_->close();
    accounts_.clear();
    {
      const std::lock_guard lock(command_mutex_);
      accepting_commands_ = false;
    }
    throw;
  }
}

void Engine::stop() {
  if (provider_started_) {
    provider_.stop();
    provider_started_ = false;
  }
  if (recorder_) recorder_->close();
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  const std::lock_guard lock(mutex_);
  if (status_.trading.enabled && status_.trading.reason.empty()) status_.trading.reason = "ENGINE_STOPPING";
  status_.trading.write = "disabled";
}

md::RecordingStats Engine::recording_stats() const {
  return recorder_ ? recorder_->stats() : md::RecordingStats{};
}

std::string Engine::recording_error() const {
  return recorder_ ? recorder_->error() : std::string{};
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
  const auto queue = queue_.status();
  out.queue_depth = queue.depth;
  out.coalesced_events = queue.coalesced;
  out.dropped_events = queue.dropped;
  out.overloaded = queue.overloaded;
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
  // Persist storage failures even if subsequent market events report healthy.
  if (const auto error = recording_error(); !error.empty()) {
    out.feed_state = md::FeedState::Error;
    out.feed_message = error;
    for (auto& [symbol, health] : out.underlyings) {
      health.state = md::FeedState::Error;
      health.message = error;
      health.last_error = error;
    }
  }
  return out;
}

void Engine::update_health(const md::Event& event, md::Timestamp received) {
  if (const auto* feed = std::get_if<md::ProviderStatus>(&event)) {
    auto update = [&](UnderlyingHealth& health) {
      if (health.state != feed->state || health.message != feed->message) {
        health_changed_ = true;
        health.state = feed->state;
        health.message = feed->message;
      }
      if (feed->state == md::FeedState::Live || feed->state == md::FeedState::Delayed) {
        health.last_success = received;
      } else if (feed->state == md::FeedState::Error) {
        health.last_error = feed->message;
        health.last_error_time = received;
      }
    };
    if (feed->underlying.empty()) {
      for (auto& [symbol, health] : health_) update(health);
    } else {
      const auto [it, inserted] = health_.try_emplace(feed->underlying);
      health_changed_ = health_changed_ || inserted;
      update(it->second);
    }
    feed_updated_ = received;
    health_dirty_ = true;
  } else if (status_.capabilities.poll_interval == std::chrono::seconds(0)) {
    // Streaming feeds prove liveness with quotes. Snapshot feeds report success
    // after the whole poll, including unchanged or empty chains.
    const std::string* symbol = nullptr;
    if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
      if (const auto* option = book_.option(quote->id)) symbol = &option->contract.underlying;
    } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
      symbol = &spot->symbol;
    }
    if (symbol) {
      auto it = health_.find(*symbol);
      if (it == health_.end()) return;
      auto& health = it->second;
      const auto state =
          status_.capabilities.delay.count() > 0 ? md::FeedState::Delayed : md::FeedState::Live;
      if (health.state != state) {
        health_changed_ = true;
        health.state = state;
        health.message = status_.provider + " " + *symbol + ": receiving quotes";
      }
      health.last_success = received;
      feed_updated_ = received;
      health_dirty_ = true;
    }
  }
}

void Engine::run() {
  std::vector<md::Event> batch;
  auto last_analytics = std::chrono::steady_clock::now();
  last_rate_time_ = last_analytics;
  auto last_health = options_.monotonic_clock();
  constexpr auto kHealthInterval = std::chrono::milliseconds(100);

  while (!stopping_ || queue_.status().depth != 0) {
    batch.clear();
    queue_.drain(batch, std::chrono::milliseconds(50));
    std::deque<PendingCommand> commands;
    {
      const std::lock_guard lock(command_mutex_);
      commands.swap(commands_);
    }
    const auto received = options_.clock();
    for (const md::Event& event : batch) {
      book_.apply(event);
      if (const auto* quote = std::get_if<md::UnderlyingQuote>(&event); quote && options_.candles) {
        // Every print reaches the chart, however many arrive between analytics passes.
        const auto& book = book_.underlyings().at(quote->symbol);
        options_.candles->sample(quote->symbol, book.spot_ts, book.spot);
      }
      if (options_.paper_enabled) observe_trading(event);
      ++events_;
      update_health(event, received);
    }
    const auto health_now = options_.monotonic_clock();
    const bool publish_health =
        health_dirty_ && (health_changed_ || health_now - last_health >= kHealthInterval);
    if (!batch.empty() || publish_health) {
      const std::lock_guard lock(mutex_);
      status_.events = events_;
      if (publish_health) {
        if (health_changed_) {
          status_.underlyings = health_;
        } else {
          // Quote receipt changes only timestamps; reuse published nodes and strings.
          for (const auto& [symbol, health] : health_) {
            auto& published = status_.underlyings.at(symbol);
            published.last_success = health.last_success;
            published.last_error_time = health.last_error_time;
          }
        }
        status_.feed_updated = feed_updated_;
        last_health = health_now;
        health_dirty_ = false;
        health_changed_ = false;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const bool ended = std::any_of(batch.begin(), batch.end(), [](const md::Event& event) {
      const auto* status = std::get_if<md::ProviderStatus>(&event);
      return status && status->state == md::FeedState::Stopped;
    });
    if (ended || stopping_ || !commands.empty() ||
        (!batch.empty() && std::any_of(accounts_.begin(), accounts_.end(), [](const PaperAccount& account) {
           return account.session && (!account.session->snapshot()->positions.empty() ||
                                       !account.session->snapshot()->open_orders.empty());
         })) ||
        now - last_analytics >= options_.analytics_interval) {
      last_analytics = now;
      refresh_analytics();
    }
    // Each account catches its own failures; nothing else here can fail the others.
    update_trading(batch, commands);
    for (auto& command : commands) apply_command(command);
  }
  std::deque<PendingCommand> remaining;
  {
    const std::lock_guard lock(command_mutex_);
    accepting_commands_ = false;
    remaining.swap(commands_);
  }
  for (auto& command : remaining) apply_command(command);
  refresh_analytics();
  // Release the exclusive journal writers on their owner thread. Published values
  // remain readable, and a replacement Engine can recover as soon as stop returns.
  accounts_.clear();
}

void Engine::refresh_analytics() {
  const auto started = std::chrono::steady_clock::now();
  bool recomputed = false;
  const auto previous_curve = discount_curve_;
  std::vector<std::string> european_updates;
  auto has_style = [](const analytics::UnderlyingBook& book, pricing::ExerciseStyle style) {
    return std::any_of(book.expiries.begin(), book.expiries.end(),
                       [style](const auto& entry) { return entry.first.second == style; });
  };
  auto analyze = [&](const std::string& symbol, const analytics::UnderlyingBook& book) {
    // Delayed feeds retain their own market-data clock.
    const md::Timestamp as_of = book.data_time > 0 ? book.data_time : md::now();
    auto options = options_.analytics;
    if (discount_curve_) options.discount_curve = discount_curve_;
    auto result = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(book, book_, as_of, options));
    analysed_versions_[symbol] = book.version;
    // Quoted prints are charted as they arrive; a spot inferred from parity is charted
    // at the option data's market time.
    if (options_.candles && result->spot_source != "quote")
      options_.candles->sample(symbol, result->as_of, result->spot);
    {
      const std::lock_guard lock(mutex_);
      metrics_[symbol] = result;
    }
    recomputed = true;
    return result;
  };
  // Build all European curves first, irrespective of symbol/map ordering. Mixed
  // OEX/XEO books also enter this phase; only their European slices form a curve.
  for (const auto& [symbol, book] : book_.underlyings()) {
    if (!has_style(book, pricing::ExerciseStyle::European) ||
        book.version == analysed_versions_[symbol])
      continue;
    auto curve = analytics::make_discount_curve(*analyze(symbol, book));
    european_updates.push_back(symbol);
    if (curve) {
      discount_curves_[symbol] = curve;
      discount_curve_ = std::move(curve);  // latest, unless SPX is available below
    } else {
      discount_curves_.erase(symbol);
      if (discount_curve_ && discount_curve_->symbol() == symbol) discount_curve_.reset();
    }
  }
  if (const auto spx = discount_curves_.find("SPX"); spx != discount_curves_.end())
    discount_curve_ = spx->second;
  else if (!discount_curve_ && !discount_curves_.empty())
    discount_curve_ = discount_curves_.begin()->second;
  const bool curve_changed = previous_curve != discount_curve_;
  // A curve update invalidates American results even without new American quotes.
  for (const auto& [symbol, book] : book_.underlyings()) {
    if (book.expiries.empty() || !has_style(book, pricing::ExerciseStyle::American)) continue;
    const bool mixed_updated = std::find(european_updates.begin(), european_updates.end(),
                                         symbol) != european_updates.end();
    if (book.version != analysed_versions_[symbol] || curve_changed || mixed_updated)
      analyze(symbol, book);
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
