#include "openport/providers/replay_batches.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace openport::providers {
namespace {
md::Timestamp market_time(const md::Event& event) {
  return std::visit([](const auto& value) -> md::Timestamp {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::OptionQuote> || std::is_same_v<T, md::OptionTrade> ||
                  std::is_same_v<T, md::UnderlyingQuote>) return value.ts;
    else return 0;
  }, event);
}
}
ReplayBatches::ReplayBatches(md::RecordingReader& reader, const md::Subscription& subscription)
    : reader_(reader), symbols_(subscription.underlyings.begin(), subscription.underlyings.end()),
      snapshots_(false) {
  // Older recordings predate SnapshotComplete. They use the streaming clock
  // even when their header describes a polling provider.
  while (const auto record = reader_.next()) {
    if (std::holds_alternative<md::SnapshotComplete>(record->event)) { snapshots_ = true; break; }
  }
  if (!reader_.diagnostic().empty()) throw std::runtime_error("replay: " + reader_.diagnostic());
  reader_.rewind();
}
bool ReplayBatches::include(const md::Event& event) {
  return std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::ContractDefinition>) {
      return admitted_[value.id] = symbols_.contains(value.contract.underlying);
    } else if constexpr (std::is_same_v<T, md::UnderlyingQuote> || std::is_same_v<T, md::UnderlyingClose>) {
      return symbols_.contains(value.symbol);
    } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
      return value.underlying.empty() || symbols_.contains(value.underlying);
    } else if constexpr (std::is_same_v<T, md::SnapshotComplete>) {
      return symbols_.contains(value.underlying);
    } else {
      const auto it = admitted_.find(value.id);
      if (it == admitted_.end()) throw std::runtime_error("replay: event references undefined contract " + std::to_string(value.id));
      return it->second;
    }
  }, event);
}
std::optional<ReplayBatch> ReplayBatches::next() {
  ReplayBatch batch;
  md::Timestamp end = 0;
  while (true) {
    auto record = pending_ ? std::exchange(pending_, {}) : reader_.next();
    if (!record) {
      if (!reader_.diagnostic().empty()) throw std::runtime_error("replay: " + reader_.diagnostic());
      break;
    }
    if (!include(record->event)) continue;
    const auto time = std::max(time_, market_time(record->event));
    if (!snapshots_ && end > 0 && time > end) {
      pending_ = std::move(record);
      break;
    }
    time_ = time;
    if (!snapshots_ && end == 0 && time > 0) end = ((time - 1) / md::kNanosPerSecond + 1) * md::kNanosPerSecond;
    batch.received = record->received;
    const bool complete = std::holds_alternative<md::SnapshotComplete>(record->event);
    batch.events.push_back(std::move(record->event));
    if (snapshots_ && complete) break;
  }
  if (batch.events.empty()) return {};
  batch.time = snapshots_ ? time_ : std::max(time_, end);
  return batch;
}
}  // namespace openport::providers
