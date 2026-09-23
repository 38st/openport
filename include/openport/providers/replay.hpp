#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "openport/md/recording.hpp"

namespace openport::providers {

/// A monotonic clock and interruptible wait keep timing tests independent of
/// scheduler jitter. interrupt() must wake a pending wait after stop is set.
class ReplayClock {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  virtual ~ReplayClock() = default;
  virtual TimePoint now() = 0;
  virtual bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) = 0;
  virtual void interrupt() = 0;
};

class ReplayProvider final : public md::Provider {
 public:
  struct Options {
    std::filesystem::path file;
    int speed = 1;  ///< 0 means maximum throughput; otherwise one of valid_speed's
    bool loop = false;
    std::shared_ptr<ReplayClock> clock;
  };

  /// 0 (as fast as possible), 1, 2, 5, 10, 30, 60, 120 or 300 times real time.
  [[nodiscard]] static bool valid_speed(int speed) noexcept;

  explicit ReplayProvider(Options options);
  ~ReplayProvider() override;
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

  /// Controls for a running replay, safe from any thread. A new speed applies to
  /// what is left of the current wait; time spent paused does not count; skip
  /// publishes the next event now, cutting short a long gap such as a closed market.
  void set_speed(int speed);
  void set_paused(bool paused);
  void skip();
  [[nodiscard]] int speed() const noexcept { return speed_.load(); }
  [[nodiscard]] bool paused() const noexcept { return paused_.load(); }
  /// The replay's clock: the recorded receipt time of the latest event published, 0 before the first.
  [[nodiscard]] md::Timestamp time() const noexcept { return time_.load(); }
  [[nodiscard]] bool finished() const noexcept { return finished_.load(); }
  [[nodiscard]] const md::RecordingHeader& header() const { return reader_.header(); }

 private:
  void run(md::Subscription subscription, md::EventSink& sink);
  /// Waits until `deadline`, honouring the controls; false once stopping.
  bool pace(ReplayClock::TimePoint& deadline);
  void wake();
  Options options_;
  md::RecordingReader reader_;
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  /// Set by stop or a control change: it ends a paced wait early.
  std::atomic<bool> interrupted_{false};
  std::atomic<int> speed_;
  std::atomic<bool> paused_{false};
  /// When the current pause was asked for, on the replay clock's time line.
  std::atomic<ReplayClock::TimePoint::rep> paused_at_{0};
  std::atomic<bool> skip_{false};
  std::atomic<md::Timestamp> time_{0};
  std::atomic<bool> finished_{false};
  std::mutex control_mutex_;
  std::condition_variable control_;
  bool started_ = false;
};

}  // namespace openport::providers
