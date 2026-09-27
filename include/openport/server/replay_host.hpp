#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "openport/server/api.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/engine.hpp"

namespace openport::server {

/// Plays recordings back beside the live feed, one at a time: each replay has its
/// own engine, isolated paper account and chart history, all on the replay's
/// clock (the recorded receipt times), so sessions, delays and feed checks behave
/// as they did that day. Its API mirrors the live one under /api/replay/..., so the
/// terminal trades a recorded day exactly as it trades today.
///
///   GET    /api/replay   recordings, scenarios, history and the replay running
///   POST   /api/replay   {file | scenario, speed?, plan?, seed?, start_at?, paused?}
///   PUT    /api/replay   {speed?, paused?, skip?}: control it
///   DELETE /api/replay   stop it
///   *      /api/replay/X the live route /api/X, on the replay
class ReplayHost {
 public:
  struct Options {
    /// Recordings are listed from and read in this directory only.
    std::filesystem::path recordings;
    /// Analytics, paper and write settings. Replay journals live beside the main
    /// journal in replays/; generated recordings and candles remain temporary.
    Engine::Options engine;
    /// Offer simulated scenarios, cached by date and seed in a temporary directory.
    /// Listing prepares the default scenario with its own seed in the background.
    bool demo = true;
    std::filesystem::path scenario_dir = {};   ///< User additions and overrides.
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
  /// The dividends replays started from now on pay (Options::engine.dividends at first).
  void set_dividends(std::vector<trading::Dividend> dividends);
  /// Journal recovery attempts for saved-run summaries and full archives.
  [[nodiscard]] std::uint64_t history_recoveries() const;

 private:
  struct Session;
  class DemoRecordings;
  class History;
  void stop_session();  // control_mutex_ held
  void control(const ApiRequest& request, const ApiCompletion& complete);
  [[nodiscard]] std::shared_ptr<Session> current() const;

  Options options_;
  // Sessions play the demo's files, so they are declared after it and stop first.
  std::unique_ptr<DemoRecordings> demos_;
  std::vector<providers::Scenario> scenarios_;
  std::unique_ptr<History> history_;
  std::mutex control_mutex_;  // serializes controls and history handoffs
  mutable std::mutex mutex_;
  std::shared_ptr<Session> session_;
};

}  // namespace openport::server
