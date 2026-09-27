#pragma once

#include <map>
#include <optional>
#include <set>
#include <vector>

#include "openport/md/recording.hpp"

namespace openport::providers {

struct ReplayBatch {
  std::vector<md::Event> events;
  md::Timestamp time = 0;      ///< market time through the complete batch
  md::Timestamp received = 0;  ///< recorded receipt clock, for feed-stall checks
};

/// Thread-free, lossless batching shared by playback and verification. Snapshots
/// end at SnapshotComplete; streams end on integral market-second boundaries.
class ReplayBatches {
 public:
  ReplayBatches(md::RecordingReader& reader, const md::Subscription& subscription);
  [[nodiscard]] std::optional<ReplayBatch> next();
  [[nodiscard]] bool snapshot_feed() const { return snapshots_; }
 private:
  bool include(const md::Event& event);
  md::RecordingReader& reader_;
  std::set<std::string> symbols_;
  std::map<md::InstrumentId, bool> admitted_;
  std::optional<md::RecordedEvent> pending_;
  md::Timestamp time_ = 0;
  bool snapshots_ = false;
};

}  // namespace openport::providers
