#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::md {

inline constexpr std::size_t kEventQueueCapacity = 65536;

struct QueueStatus {
  std::size_t depth = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t dropped = 0;
  bool overloaded = false;
};

/// Multi-producer queue of latest state. Definitions are ordering barriers.
/// Only trades may be discarded at capacity. Latest-value state, definitions and
/// status must survive overload: snapshot providers may never resend unchanged data.
/// Storage and dense-id indexes grow in blocks, without a node allocation per event.
class EventQueue final : public EventSink {
 public:
  explicit EventQueue(std::size_t capacity = kEventQueueCapacity, bool preserve_spot_prints = false)
      : capacity_(capacity), preserve_spot_prints_(preserve_spot_prints) {}

  void publish(Event event) override {
    {
      const std::lock_guard lock(mutex_);
      if (const auto* definition = std::get_if<ContractDefinition>(&event)) {
        if (definition->id < latest_.size()) latest_[definition->id].positions.fill(kNone);
      }
      std::size_t* previous = coalescing_slot(event);
      if (previous && *previous != kNone) {
        events_[*previous] = std::move(event);
        ++coalesced_;
        return;
      }
      const bool trade = std::holds_alternative<OptionTrade>(event);
      if (depth_ >= capacity_) {
        if (trade) {
          ++dropped_;
          dropped_since_drain_ = true;
          return;
        }
        // Tombstones preserve all remaining positions, including definition barriers.
        if (next_trade_ < trades_.size()) {
          events_[trades_[next_trade_++]].reset();
          --depth_;
          ++dropped_;
          dropped_since_drain_ = true;
        }
      }
      if (previous) *previous = events_.size();
      if (trade) trades_.push_back(events_.size());
      events_.emplace_back(std::move(event));
      ++depth_;
    }
    ready_.notify_one();
  }

  /// Appends every retained event in publication order, waiting for the first one.
  std::size_t drain(std::vector<Event>& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!depth_) ready_.wait_for(lock, timeout, [this] { return depth_ != 0; });
    const auto count = depth_;
    out.reserve(out.size() + count);
    for (auto& event : events_)
      if (event) out.push_back(std::move(*event));
    events_.clear();
    trades_.clear();
    next_trade_ = 0;
    // Invalidate all indexes without touching the full chain under the publisher lock.
    // Zero is reserved for unused entries; discard indexes on generation wraparound.
    if (++generation_ == 0) {
      latest_.clear();
      spots_.clear();
      generation_ = 1;
    }
    depth_ = 0;
    dropped_since_drain_ = false;
    return count;
  }

  [[nodiscard]] QueueStatus status() const {
    const std::lock_guard lock(mutex_);
    return {depth_, coalesced_, dropped_, depth_ > capacity_ || dropped_since_drain_};
  }

 private:
  static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

  struct Latest {
    std::uint64_t generation = 0;
    std::array<std::size_t, 3> positions{kNone, kNone, kNone};
  };

  struct Spot {
    std::string symbol;
    std::uint64_t generation = 0;
    std::size_t position = kNone;
  };

  std::size_t* coalescing_slot(const Event& event) {
    InstrumentId id;
    std::size_t kind;
    if (const auto* quote = std::get_if<OptionQuote>(&event)) {
      id = quote->id;
      kind = 0;
    } else if (const auto* greeks = std::get_if<VendorGreeks>(&event)) {
      id = greeks->id;
      kind = 1;
    } else if (const auto* interest = std::get_if<OpenInterest>(&event)) {
      id = interest->id;
      kind = 2;
    } else if (const auto* spot = std::get_if<UnderlyingQuote>(&event)) {
      // Settlement consumers need the first closing print, not the last in a drain.
      if (preserve_spot_prints_) return nullptr;
      // Only a handful of subscribed underlyings; retain their storage across drains.
      for (auto& entry : spots_) {
        if (entry.symbol != spot->symbol) continue;
        if (entry.generation != generation_) {
          entry.generation = generation_;
          entry.position = kNone;
        }
        return &entry.position;
      }
      spots_.push_back({spot->symbol, generation_, kNone});
      return &spots_.back().position;
    } else {
      return nullptr;
    }
    if (id >= latest_.size()) latest_.resize(static_cast<std::size_t>(id) + 1);
    auto& entry = latest_[id];
    if (entry.generation != generation_) {
      entry.generation = generation_;
      entry.positions.fill(kNone);
    }
    return &entry.positions[kind];
  }

  const std::size_t capacity_;
  const bool preserve_spot_prints_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<std::optional<Event>> events_;
  std::uint64_t generation_ = 1;
  std::vector<Latest> latest_;
  std::vector<Spot> spots_;
  std::vector<std::size_t> trades_;
  std::size_t next_trade_ = 0;
  std::size_t depth_ = 0;
  std::uint64_t coalesced_ = 0;
  std::uint64_t dropped_ = 0;
  bool dropped_since_drain_ = false;
};

}  // namespace openport::md
