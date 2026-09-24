#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"

namespace openport::server {

/// Plays recordings back beside the live feed, one at a time: each replay has its
/// own engine, in-memory paper account and chart history, all on the replay's
/// clock (the recorded receipt times), so sessions, delays and feed checks behave
/// as they did that day. Its API mirrors the live one under /api/replay/..., so the
/// terminal trades a recorded day exactly as it trades today.
///
///   GET    /api/replay   recordings in the directory, the demo, and the replay running
///   POST   /api/replay   {file | demo: true, speed?, plan?}: start one (stopping any other)
///   PUT    /api/replay   {speed?, paused?, skip?}: control it
///   DELETE /api/replay   stop it
///   *      /api/replay/X the live route /api/X, on the replay
class ReplayHost {
 public:
  struct Options {
    /// Recordings are listed from and read in this directory only.
    std::filesystem::path recordings;
    /// Analytics, paper and write settings for replay engines; journals, recording
    /// and chart persistence are always off for a replay.
    Engine::Options engine;
    /// Offer the demo market: simulated days (providers::write_demo_recording)
    /// played like recordings, each generated once, into a directory only this
    /// process uses, and removed with the host. Listing the replays prepares the
    /// default day in the background.
    bool demo = true;
  };

  explicit ReplayHost(Options options);
  ~ReplayHost();
  ReplayHost(const ReplayHost&) = delete;
  ReplayHost& operator=(const ReplayHost&) = delete;

  /// Handles /api/replay and every /api/replay/... route; false for any other target.
  bool handle(const ApiRequest& request, const ApiCompletion& complete);
  /// The replay's WebSocket message, a tick typed "replay_tick" with the replay's
  /// state, or empty while none runs.
  [[nodiscard]] std::string tick() const;
  void stop();

 private:
  struct Session;
  class DemoRecordings;
  void control(const ApiRequest& request, const ApiCompletion& complete);
  [[nodiscard]] std::shared_ptr<Session> current() const;

  Options options_;
  // Sessions play the demo's files, so they are declared after it and stop first.
  std::unique_ptr<DemoRecordings> demos_;
  mutable std::mutex mutex_;
  std::shared_ptr<Session> session_;
};

}  // namespace openport::server
