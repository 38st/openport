#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
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
    int speed = 1;  ///< 0 means maximum throughput; otherwise 1, 10 or 60
    bool loop = false;
    std::shared_ptr<ReplayClock> clock;
  };

  explicit ReplayProvider(Options options);
  ~ReplayProvider() override;
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

 private:
  void run(md::Subscription subscription, md::EventSink& sink);
  Options options_;
  md::RecordingReader reader_;
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
};

}  // namespace openport::providers
