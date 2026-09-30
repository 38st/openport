#pragma once

#include "openport/providers/demo.hpp"
#include "openport/providers/replay.hpp"
#include "openport/providers/scenario.hpp"

namespace openport::providers {

/// An endless rotation of simulated regular sessions, on successive trading dates.
class DemoProvider final : public md::Provider {
 public:
  struct Options {
    std::vector<std::string> days;  ///< Empty selects all built-in regular sessions.
    int speed = 1;
    md::Timestamp started = 0;  ///< Zero selects the server's start time.
    std::shared_ptr<ReplayClock> clock;
  };
  explicit DemoProvider(Options options);
  ~DemoProvider() override;
  [[nodiscard]] std::string_view name() const noexcept override { return kDemoProvider; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;
  void set_driver(ReplayProvider::Driver driver);
  /// Start on the trading date after recovered market time; zero keeps the default.
  /// May only be called before start().
  void start_after(md::Timestamp recovered_time);
  [[nodiscard]] md::Timestamp time() const noexcept { return time_.load(); }
  [[nodiscard]] const std::vector<std::string>& symbols() const { return symbols_; }
  [[nodiscard]] const std::vector<Scenario>& days() const { return days_; }
  [[nodiscard]] md::Date first_date() const { return first_date_; }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
  void validate(const md::Subscription& subscription) const;

  [[nodiscard]] static md::Date next_date(md::Date date);
  [[nodiscard]] static std::uint64_t seed(std::string_view id, md::Date date);
  /// Fit timed events proportionally into a shortened regular session.
  [[nodiscard]] static Scenario on_date(Scenario scenario, md::Date date);

 private:
  void run(md::Subscription subscription, md::EventSink& sink);
  Options options_;
  std::vector<Scenario> days_;
  std::vector<std::string> symbols_;
  md::Date first_date_;
  std::filesystem::path directory_;
  ReplayProvider::Driver driver_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<bool> stopping_{false};
  std::atomic<md::Timestamp> time_{0};
  bool started_ = false;
};

/// Removes the openport-feed-PID-* and openport-demo-PID-* directories that a process
/// no longer running left in the temporary directory, as a SIGKILL does; a running
/// process's directories and other users' are never touched. Returns how many went.
std::size_t remove_orphaned_demo_directories();

}  // namespace openport::providers
