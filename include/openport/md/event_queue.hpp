#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::md {

/// Unbounded multi-producer, single-consumer event queue. Providers publish from
/// their own threads; the consumer drains everything that is waiting in one go.
class EventQueue final : public EventSink {
 public:
  void publish(Event event) override {
    {
      const std::lock_guard lock(mutex_);
      events_.push_back(std::move(event));
    }
    ready_.notify_one();
  }

  /// Appends every queued event to `out`, waiting up to `timeout` for the first one.
  /// Returns how many events were moved.
  std::size_t drain(std::vector<Event>& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (events_.empty()) ready_.wait_for(lock, timeout, [this] { return !events_.empty(); });
    const std::size_t count = events_.size();
    if (out.empty()) {
      out.swap(events_);
    } else {
      out.insert(out.end(), std::make_move_iterator(events_.begin()),
                 std::make_move_iterator(events_.end()));
      events_.clear();
    }
    return count;
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<Event> events_;
};

}  // namespace openport::md
