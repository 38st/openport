#include "openport/providers/replay.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "openport/providers/factory.hpp"

namespace openport::providers {
namespace {
class SystemReplayClock final : public ReplayClock {
 public:
  TimePoint now() override { return std::chrono::steady_clock::now(); }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex_);
    return !wake_.wait_until(lock, deadline, [&] { return stop.load(); });
  }
  void interrupt() override {
    const std::lock_guard lock(mutex_);
    wake_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
};

/// Unsigned subtraction covers the full int64 timestamp range without overflow.
/// Backward wall-clock steps add no delay; subsequent positive gaps still count.
ReplayClock::TimePoint advance(ReplayClock::TimePoint deadline, md::Timestamp previous,
                               md::Timestamp received, int speed, std::uint64_t& remainder) {
  if (received <= previous || speed == 0) return deadline;
  const auto gap = static_cast<std::uint64_t>(received) - static_cast<std::uint64_t>(previous);
  const auto divisor = static_cast<std::uint64_t>(speed);
  const auto fractional = remainder + gap % divisor;
  const auto delta = gap / divisor + fractional / divisor;
  remainder = fractional % divisor;
  const auto room =
      std::chrono::duration_cast<std::chrono::nanoseconds>(ReplayClock::TimePoint::max() - deadline)
          .count();
  if (room < 0 || delta > static_cast<std::uint64_t>(room)) return ReplayClock::TimePoint::max();
  return deadline + std::chrono::nanoseconds(delta);
}
}  // namespace

ReplayProvider::ReplayProvider(Options options)
    : options_(std::move(options)),
      reader_(options_.file),
      name_("replay (" + reader_.header().provider + ")") {
  if (options_.speed != 0 && options_.speed != 1 && options_.speed != 10 && options_.speed != 60)
    throw std::invalid_argument("replay: speed must be 1, 10, 60 or max");
  if (!options_.clock) options_.clock = std::make_shared<SystemReplayClock>();
}
ReplayProvider::~ReplayProvider() {
  stop();
}
md::Capabilities ReplayProvider::capabilities() const noexcept {
  return reader_.header().capabilities;
}

void ReplayProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  if (started_) throw std::logic_error("ReplayProvider::start may be called only once");
  started_ = true;
  validate_subscription("replay", subscription);
  const auto& available = reader_.header().subscription.underlyings;
  std::string names;
  for (const auto& symbol : available) names += (names.empty() ? "" : ", ") + symbol;
  if (subscription.underlyings.empty())
    throw std::invalid_argument("replay: no symbols subscribed");
  for (const auto& symbol : subscription.underlyings)
    if (std::find(available.begin(), available.end(), symbol) == available.end())
      throw std::invalid_argument("replay: unknown symbol " + symbol + "; file contains: " + names);
  thread_ = std::thread([this, subscription, &sink] { run(subscription, sink); });
}

void ReplayProvider::stop() {
  if (thread_.joinable()) {
    stopping_ = true;
    options_.clock->interrupt();
    thread_.join();
  }
}

void ReplayProvider::run(md::Subscription subscription, md::EventSink& sink) {
  const std::set<std::string> symbols(subscription.underlyings.begin(),
                                      subscription.underlyings.end());
  try {
    do {
      std::unordered_map<md::InstrumentId, bool> admitted;
      auto deadline = options_.clock->now();
      std::optional<md::Timestamp> previous;
      std::uint64_t remainder = 0;
      bool any = false;
      while (!stopping_.load()) {
        auto record = reader_.next();
        if (!record) break;
        if (previous)
          deadline = advance(deadline, *previous, record->received, options_.speed, remainder);
        previous = record->received;
        const bool include = std::visit(
            [&](const auto& e) {
              using T = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<T, md::ContractDefinition>) {
                return admitted[e.id] = symbols.contains(e.contract.underlying);
              } else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) {
                return symbols.contains(e.symbol);
              } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
                return e.underlying.empty() || symbols.contains(e.underlying);
              } else {
                const auto it = admitted.find(e.id);
                if (it == admitted.end())
                  throw std::runtime_error("replay: event references undefined contract " +
                                           std::to_string(e.id));
                return it->second;
              }
            },
            record->event);
        if (!include) continue;
        if (options_.speed != 0 && !options_.clock->wait_until(deadline, stopping_)) break;
        if (stopping_.load()) break;
        sink.publish(std::move(record->event));
        any = true;
      }
      if (stopping_.load()) break;
      if (!reader_.diagnostic().empty()) {
        sink.publish(md::ProviderStatus{md::now(), md::FeedState::Stopped,
                                        name_ + ": " + reader_.diagnostic(), ""});
        return;  // A damaged input must never loop as though it were complete.
      }
      if (!options_.loop || !any) break;
      if (options_.speed != 0 && !options_.clock->wait_until(deadline, stopping_)) break;
      reader_.rewind();
    } while (!stopping_.load());
    sink.publish(md::ProviderStatus{md::now(), md::FeedState::Stopped,
                                    name_ + (stopping_.load() ? ": stopped" : ": end of recording"),
                                    ""});
  } catch (const std::exception& error) {
    sink.publish(
        md::ProviderStatus{md::now(), md::FeedState::Error, name_ + ": " + error.what(), ""});
  }
}

}  // namespace openport::providers
