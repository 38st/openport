#pragma once
#include <vector>
#include "openport/md/provider.hpp"

namespace openport::test {
class BrokerSink final : public md::EventSink {
 public:
  void publish(md::Event event) override { events.push_back(std::move(event)); }
  template <typename T> std::vector<T> all() const {
    std::vector<T> result;
    for (const auto& event : events)
      if (const auto* value = std::get_if<T>(&event)) result.push_back(*value);
    return result;
  }
  std::vector<md::Event> events;
};
}  // namespace openport::test
